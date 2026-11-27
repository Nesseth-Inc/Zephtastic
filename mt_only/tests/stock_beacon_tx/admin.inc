// SPDX-License-Identifier: GPL-3.0-only
// The provider substitutes the durable nonce backend. Service receives,
// authenticates the configured channel, stages the read, and schedules its reply.
struct AdminChannelProvider:RelayFixtureProvider {
    using RelayFixtureProvider::RelayFixtureProvider;
    bool encodeChannelControl(bench::MtCodec &codec,uint32_t to,uint32_t id,const uint8_t *data,
                              size_t n,bench::Frame &frame)override{
        size_t size=0;
        if(!codec.encodeAdminChannelResponse(local_,to,id,data,n,frame.bytes,sizeof(frame.bytes),size))return false;
        frame.len=size;return true;
    }
    bool broadcast(bench::MtCodec &codec,uint32_t id,const uint8_t *data,size_t n,bench::Frame &frame)override{
        size_t size=0;
        if(!codec.encodeZeroHopData(local_,UINT32_MAX,id,data,n,frame.bytes,sizeof(frame.bytes),size))return false;
        frame.len=size;return true;
    }
};
static bool ownerData(bool response,uint32_t id,uint8_t *out,size_t capacity,size_t &size){
    meshtastic_AdminMessage admin{};meshtastic_Data data{};data.portnum=meshtastic_PortNum_ADMIN_APP;
    data.has_bitfield=true;
    if(response){
        admin.which_payload_variant=meshtastic_AdminMessage_get_owner_response_tag;
        admin.session_passkey.size=8;memset(admin.session_passkey.bytes,0x5a,8);data.request_id=id;
    }else{
        admin.which_payload_variant=meshtastic_AdminMessage_get_owner_request_tag;admin.get_owner_request=true;
        data.want_response=true;data.bitfield=2;
    }
    auto payload=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
    if(!pb_encode(&payload,meshtastic_AdminMessage_fields,&admin))return false;
    data.payload.size=payload.bytes_written;
    auto output=pb_ostream_from_buffer(out,capacity);
    if(!pb_encode(&output,meshtastic_Data_fields,&data))return false;
    size=output.bytes_written;return true;
}
static unsigned reads=0;
static void setupAdmin(Fixture &f){
    uint8_t key[16];memset(key,0x57,sizeof(key));mt::ChannelRecord record;
    assert(record.set(2,"admin",key,sizeof(key))&&f.home.setSecondaryChannel(1,record)&&f.home.setSecurityOptions(false,true));
    assert(f.service.setLegacyAdminHandler([](void *,const mt::admin::LegacyAdminRequest &request,uint32_t,
               uint8_t *out,size_t capacity,size_t &size){
        assert(request.kind()==mt::admin::LegacyAdminRequest::Kind::Get&&request.channelIndex()==1);
        ++reads;return ownerData(true,request.packetId(),out,capacity,size);
    },nullptr));
}
struct WaveSettings:mt::SettingsBackend {
    std::map<std::string,Bytes> records;unsigned writes=0;
    int read(const char *name,uint8_t *p,size_t n)override{
        const auto found=records.find(name);if(found==records.end())return -ENOENT;
        if(found->second.size()>n)return -E2BIG;
        memcpy(p,found->second.data(),found->second.size());return found->second.size();
    }
    int write(const char *name,const uint8_t *p,size_t n)override{
        records[name]=Bytes(p,p+n);++writes;return 0;
    }
};
static void stagedAdminRead(){
    AdminChannelProvider identity(Self);Fixture f(false,&identity,setupAdmin);reads=0;
    WaveSettings settings;mt::modules::ExtendedModuleJournal journal;
    mt::modules::ExtendedModuleEdit edit(f.service,journal,settings);
    assert(!journal.load(settings)&&edit.load());
    meshtastic_ModuleConfig cfg{};cfg.which_payload_variant=meshtastic_ModuleConfig_mesh_beacon_tag;
    auto &b=cfg.payload_variant.mesh_beacon;b.flags=3;strcpy(b.broadcast_message,"scheduled");
    assert(!edit.save(cfg,10,false));
    const auto baseline=settings.writes;
    Excursion::ResolvedTarget home{};home.profile=Runtime::runningProfile(f.home);
    const uint32_t t=60000,peer=0xabcdf003,id=832;
    assert(f.enqueue(5000,home,t));f.radio.blockScan=true;draw=3;f.poll(t);f.radio.blockScan=false;

    mt::PrimaryChannel::Selection channel{};assert(f.home.channelSelection(1,channel)&&channel.hash);
    bench::MtCodec peerChannel;assert(peerChannel.setKey(channel.hash,channel.bytes,channel.length));
    // A peer GET requests a response. The local zero-hop unsolicited
    // application encoder intentionally rejects that bit; use stock CTR wire framing.
    bench::Frame wire{};size_t size=0;
    assert(ownerData(false,id,wire.bytes+16,sizeof(wire.bytes)-16,size));
    auto put=[](uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));};
    put(wire.bytes,Self);put(wire.bytes+4,peer);put(wire.bytes+8,id);
    wire.bytes[13]=channel.hash;wire.bytes[15]=uint8_t(peer);
    uint8_t nonce[16];bench::MtCodec::nonce(peer,id,nonce);
    assert(bench::mtAesCtr(nullptr,channel.bytes,channel.length,nonce,wire.bytes+16,size));
    mt::PrimaryChannel::clear(channel);const auto n=size+16;
    bool routed=true;f.service.rx(wire.bytes,n,1,t+1,32,true,&routed);
    assert(!routed&&f.service.legacyAdminWorkQueued()&&f.service.storageStopSafe()&&!f.service.endpointControls());
    f.poll(t+2);
    assert(!f.runtime.busy()&&f.service.legacyAdminWorkQueued()&&f.radio.frames.empty());

    mt::modules::BeaconBroadcaster producer;unsigned queued=0;
    auto scheduled=[&](uint32_t at){
        assert(f.clock.set(at)&&producer.takeScheduledTick(edit,at));
        producer.pollScheduledTargets(f.service,edit,true,at,0,0x78563412,
            [&](const uint8_t *,size_t,uint32_t,unsigned){++queued;},f.runtime,1);
    };
    scheduled(t+3);
    assert(f.service.legacyAdminWorkQueued()&&!queued&&settings.writes==baseline&&!edit.config().sentBefore());
    assert(f.clock.set(t+4));f.service.poll(t+4,716);
    assert(reads==1&&f.service.endpointControls()==1);
    assert(f.clock.set(t+404));f.service.poll(t+404,19);
    assert(f.radio.frames.size()==1);
    const auto &reply=f.radio.frames.front();bench::RouteMetadata meta{};
    assert(bench::le32(reply.data())==peer&&peerChannel.decode(reply.data(),reply.size(),meta)&&
           meta.portnum==meshtastic_PortNum_ADMIN_APP&&meta.request_id==id);
    f.radio.complete=true;f.service.poll(t+405,19);
    assert(!producer.takeScheduledTick(edit,t+406));
    scheduled(t+3+3600000);
    assert(queued==1&&f.runtime.queued()==1&&settings.writes>baseline&&edit.config().sentBefore());
}
