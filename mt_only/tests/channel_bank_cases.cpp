// SPDX-License-Identifier: GPL-3.0-only
// Actual configuration, journal, Service and both Companion paths. Synthetic
// keys/peers/radio only; no hardware, private crypto or RF claims.
#include "device_config_fixture.h"
#include "known_channel_fixture.h"
#include "channel_bank/candidate_assertions.inc"
#include <array>
static void pass(const char *s){std::cout<<"PASS "<<s<<'\n';}
static mt::ChannelRecord secondary(const char *name="BatchLink",uint8_t alias=1) {
  mt::ChannelRecord r;assert(r.set(2,name,&alias,1));return r;
}
static void configCases() {
  mt::Config c;c.bytes[4]=1;c.seal();assert(c.valid()&&sizeof(c)==640);
  for(unsigned i=1;i<8;++i){auto r=secondary();assert(c.setSecondaryChannel(i,r));}
  mt::Config decoded;assert(mt::Config::decode(c.bytes,c.Size,decoded)&&c.same(decoded));
  for(unsigned i=1;i<8;++i){mt::ChannelRecord r;assert(decoded.channelRecord(i,r)&&r.role()==2&&!strcmp(r.name(),"BatchLink"));}
  // Independently build each old primary-only layout with its original CRC.
  for(unsigned version:{11u,12u}) {
    uint8_t old[128]{};memcpy(old,c.bytes,126);old[2]=uint8_t(version);
    if(version==11)old[121]=0;
    const auto crc=mt::Config::checksum(old,126);old[126]=uint8_t(crc);old[127]=uint8_t(crc>>8);
    assert(mt::Config::decode(old,sizeof(old),decoded)&&decoded.bytes[2]==mt::Config::Version&&decoded.hopLimit()==3);
    assert(!memcmp(decoded.bytes+6,c.bytes+6,72));
    for(unsigned i=1;i<8;++i){mt::ChannelRecord r;assert(decoded.channelRecord(i,r)&&!r.role());}
    const auto before=decoded;old[126]^=1;assert(!mt::Config::decode(old,sizeof(old),decoded)&&decoded.same(before));
  }
  {
    auto old22=c;old22.bytes[2]=22;old22.bytes[604]=old22.bytes[605]=0;old22.seal();
    assert(old22.validBody(22)&&mt::Config::decode(old22.bytes,old22.Size,decoded)&&
      decoded.bytes[2]==mt::Config::Version&&!decoded.primaryAead());
    auto badPrimary=old22;badPrimary.bytes[604]=1;badPrimary.seal();
    assert(!mt::Config::decode(badPrimary.bytes,badPrimary.Size,decoded));
    auto badSecondary=old22;const size_t slot7=126+6*mt::ChannelRecord::Size;
    badSecondary.bytes[slot7+2]|=8u;
    mt::ChannelRecord malformed;memcpy(malformed.bytes,badSecondary.bytes+slot7,sizeof(malformed));malformed.seal();
    memcpy(badSecondary.bytes+slot7,malformed.bytes,sizeof(malformed));badSecondary.seal();
    assert(!mt::Config::decode(badSecondary.bytes,badSecondary.Size,decoded));
  }
  pass("eight slots roundtrip and original128-byte v11/v12 migrate with empty secondaries and preserved owner settings");
  const auto original=c;
  for(unsigned i=126;i<574;++i){auto bad=original;bad.bytes[i]^=1;bad.seal();assert(!bad.valid());}
  auto r=secondary();assert(!c.setSecondaryChannel(0,r)&&!c.setSecondaryChannel(8,r));
  assert(r.set(1,"Other",nullptr,0)&&!c.setSecondaryChannel(1,r));
  assert(r.set(2,"BatchLink",nullptr,0)&&c.setSecondaryChannel(7,r));
  mt::PrimaryChannel::Selection primary,inherited;
  assert(c.channelSelection(0,primary)&&c.channelSelection(7,inherited)&&primary.length==inherited.length&&!memcmp(primary.bytes,inherited.bytes,primary.length));
  mt::PrimaryChannel::clear(primary);mt::PrimaryChannel::clear(inherited);
  pass("each stored record corruption and invalid slot/role fail; empty secondary key inherits primary bytes");
}
static void phoneCases() {
  for(bool ble:{false,true}) {
    Fixture f;f.provision();
    meshtastic_AdminMessage write{};write.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;
    auto &ch=write.set_channel;ch.index=7;ch.has_settings=true;ch.role=meshtastic_Channel_Role_SECONDARY;
    ch.settings.psk.size=1;ch.settings.psk.bytes[0]=1;strcpy(ch.settings.name,"BatchLink");
    ch.settings.has_module_settings=true;ch.settings.module_settings.is_muted=true;
    ch.settings.use_aead=true;
    f.admin(write,ble);assert((ble?f.ble:f.usb).error()==0);
    meshtastic_AdminMessage get{};get.which_payload_variant=meshtastic_AdminMessage_get_channel_request_tag;get.get_channel_request=8;
    f.admin(get,!ble);const auto read=(!ble?f.ble:f.usb).reply().get_channel_response;
    assert(read.index==7&&read.role==meshtastic_Channel_Role_SECONDARY&&read.settings.psk.size==1&&read.settings.psk.bytes[0]==1&&!strcmp(read.settings.name,"BatchLink")&&read.settings.module_settings.is_muted&&read.settings.use_aead);
    mt::PrimaryChannel::Selection selected,expected;const uint8_t alias=1;
    assert(f.service.config.channelSelection(7,selected)&&selected.aead&&
      mt::PrimaryChannel::normalize(&alias,1,expected,"BatchLink",true)&&selected.hash==expected.hash);
    const uint8_t expectedAeadHash=expected.hash;
    mt::PrimaryChannel::clear(selected);mt::PrimaryChannel::clear(expected);
    mt::SettingsJournal reboot;assert(!reboot.load(f.backend)&&reboot.config().same(f.service.config));
    assert(f.backend.entries["mtbase/state1"].size()==mt::SettingsJournal::RecordSize);
    const auto before=f.service.config;ch.settings.module_settings.position_precision=15;f.admin(write,ble);
    assert((ble?f.ble:f.usb).error()!=0&&f.service.config.same(before));
    ch.settings.module_settings.position_precision=0;ch.role=meshtastic_Channel_Role_DISABLED;f.admin(write,ble);
    assert((ble?f.ble:f.usb).error()==0);mt::ChannelRecord stored;assert(f.service.config.channelRecord(7,stored)&&!stored.role()&&!stored.aead()&&!strcmp(stored.name(),"BatchLink"));
    ch.role=meshtastic_Channel_Role_SECONDARY;ch.settings.use_aead=true;f.admin(write,ble);
    assert((ble?f.ble:f.usb).error()==0&&f.service.config.channelRecord(7,stored)&&stored.aead());
    ch.role=meshtastic_Channel_Role_PRIMARY;f.admin(write,ble);
    assert((ble?f.ble:f.usb).error()==0&&f.service.config.primaryAead());
    assert(f.service.config.channelSelection(0,selected)&&selected.aead&&selected.hash==expectedAeadHash);
    auto primary=f.service.config.node();assert(primary.bytes[4]==uint8_t(selected.hash^0xAEu));mt::PrimaryChannel::clear(selected);
    meshtastic_AdminMessage disabled{};disabled.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;
    disabled.set_channel.role=meshtastic_Channel_Role_DISABLED;f.admin(disabled,ble);
    assert((ble?f.ble:f.usb).error()==0&&!f.service.config.primaryAead());
  }
  pass("USB/BLE secondary AEAD roundtrip, hash distinction, journal reload, disable clearing, and primary promotion preserve/clear mode");
  Fixture f;f.provision();const auto before=f.service.config;
  auto c=before;assert(c.setSecondaryChannel(7,secondary()));f.backend.fail=true;
  assert(f.edit.save(c,20)==-EIO&&f.service.config.same(before)&&!f.journal.ready());
  pass("failed bank commit cannot publish RAM and invalidates further storage authority");
  Backend oldBackend;std::vector<uint8_t> old(144,0);old[0]='Z';old[1]='T';old[2]='J';old[3]=3;old[4]=17;old[8]=128;
  mt::Config primary;primary.bytes[4]=1;primary.seal();memcpy(old.data()+12,primary.bytes,126);old[14]=12;
  const auto crc=mt::Config::checksum(old.data()+12,126);old[138]=uint8_t(crc);old[139]=uint8_t(crc>>8);
  uint32_t sum=UINT32_MAX;for(unsigned i=0;i<140;++i){sum^=old[i];for(unsigned j=0;j<8;++j)sum=(sum>>1)^(0xedb88320u&uint32_t(-int32_t(sum&1)));}sum=~sum;
  for(unsigned i=0;i<4;++i)old[140+i]=uint8_t(sum>>(8*i));
  oldBackend.entries["mtbase/state0"]=old;
  mt::SettingsJournal migrated;assert(!migrated.load(oldBackend)&&migrated.generation()==17&&migrated.config().same(primary)&&!oldBackend.writes);
  auto next=migrated.config();assert(next.setSecondaryChannel(7,secondary())&&!migrated.save(oldBackend,next)&&oldBackend.entries["mtbase/state0"]==old&&oldBackend.entries["mtbase/state1"].size()==656);
  mt::SettingsJournal reboot;assert(!reboot.load(oldBackend)&&reboot.generation()==18&&reboot.config().same(next));
  pass("original144-byte v3 journal retained beside new656-byte v6 bank commit with exact generation and reboot continuity");
}
struct WireRadio:Radio {
  std::vector<std::vector<uint8_t>> frames;
  int send(uint8_t *p,size_t n)override{frames.emplace_back(p,p+n);return Radio::send(p,n);}
};
struct WireHost:mt::Companion::Host {
  std::vector<meshtastic_FromRadio> packets;
  bool ready()const override{return true;}
  int save(const mt::Config &)override{return -ENOTSUP;}
  bool send(const uint8_t *p,size_t n)override{meshtastic_FromRadio out{};auto s=pb_istream_from_buffer(p+4,n-4);assert(pb_decode(&s,meshtastic_FromRadio_fields,&out));packets.push_back(out);return true;}
};
// Records which private boundary was selected. No durable lease or private
// cryptography is established by this test double.
struct RecordingPrivate:mt::pki::PrivateRuntime {
  uint32_t local;unsigned privateCalls=0;
  explicit RecordingPrivate(uint32_t id):local(id){}
  uint32_t peer()const override{return 0x87654321;}
  bool ready(uint32_t node)const override{return node==local;}
  bool publicIdentity(uint8_t out[32])const override{memset(out,0x42,32);return true;}
  mt::pki::DestinationDisposition destinationDisposition(uint32_t node,const uint8_t *key,size_t size)const override{
    if(node!=peer())return mt::pki::DestinationDisposition::Unavailable;
    if(!key&&!size)return mt::pki::DestinationDisposition::PinnedActive;
    if(!key||size!=32)return mt::pki::DestinationDisposition::KeyMismatch;
    for(size_t i=0;i<32;++i)if(key[i]!=0x24)return mt::pki::DestinationDisposition::KeyMismatch;
    return mt::pki::DestinationDisposition::PinnedActive;
  }
  bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,uint32_t,uint32_t,bool,uint32_t,bench::Frame &)override{return false;}
  void freeze()override{}bool matchesPeer(uint32_t)const override{return true;}bool revoke(uint32_t)override{return false;}
  void clearEvent()override{}void sweep(mt::Endpoint &)override{}
  int submit(mt::Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,bool,const uint8_t *,size_t,bool,uint32_t)override{++privateCalls;return 0;}
  bool encodeChannelControl(bench::MtCodec &codec,uint32_t to,uint32_t id,const uint8_t *data,size_t n,bench::Frame &frame)override {
    size_t size=0;const bool ok=codec.encodeZeroHopData(local,to,id,data,n,frame.bytes,sizeof(frame.bytes),size);if(ok)frame.len=size;return ok;
  }
  void receive(mt::Endpoint &,bench::MtCodec &,const uint8_t *,size_t,uint32_t,uint32_t,bool)override{}
  void proof(mt::Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t)override{}
  bool event(uint8_t,const uint8_t *,size_t,mt::pki::PrivateEvent &)override{return false;}
  bool encodePublicApplication(bench::MtCodec &codec,uint32_t to,uint32_t id,const uint8_t *data,size_t n,bench::Frame &frame)override {
    size_t size=0;const bool ok=codec.encodeZeroHopData(local,to,id,data,n,frame.bytes,sizeof(frame.bytes),size);if(ok)frame.len=size;return ok;
  }
};
static void input(mt::Companion &companion,const meshtastic_ToRadio &r,uint32_t now) {
  uint8_t data[512]{};auto out=pb_ostream_from_buffer(data,sizeof(data));assert(pb_encode(&out,meshtastic_ToRadio_fields,&r));companion.packet(data,out.bytes_written,now,mt::Companion::WorkspaceProof{true});
}
struct WireSide {
  mt::test::KnownChannelFixtureClock knownChannelClock;
  RecordingPrivate privateRuntime;WireRadio radio;mt::Service service;WireHost usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion c,b;
  WireSide(uint32_t id,bool withPrivate=false):privateRuntime(id),service(radio,true,id,81,true,false,withPrivate?&privateRuntime:nullptr),c(service,usb,81,workspace,0),b(service,ble,81,workspace,1) {
    knownChannelClock.bind(service);
    auto cfg=service.config;cfg.bytes[4]=1;cfg.seal();assert(cfg.setSecondaryChannel(7,secondary()));
    assert(service.apply(cfg)&&service.start(false,0)&&service.armEndpoint(true));
    meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=1;
    input(c,hello,1);input(b,hello,1);usb.packets.clear();ble.packets.clear();
  }
  void receive(const std::vector<uint8_t> &p,uint32_t now) {
    service.rx(p.data(),p.size(),3,now,700);c.receive(p.data(),p.size(),-70,3,now,mt::Companion::WorkspaceProof{true});b.receive(p.data(),p.size(),-70,3,now,mt::Companion::WorkspaceProof{true});
    assert(service.selectedChannel()==0);
  }
  void done(uint32_t now){radio.done=true;service.poll(now,0);}
};
static unsigned received(const WireHost &host,unsigned port,unsigned channel) {
  unsigned n=0;for(const auto &r:host.packets)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&
    r.packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&unsigned(r.packet.decoded.portnum)==port&&r.packet.channel==channel)++n;
  return n;
}
#include "channel_bank/context_adversaries.inc"
static void collisionCases() {
  constexpr uint32_t peer=0x87654321;
  auto f=std::make_unique<WireSide>(Node,true);f->service.stop();auto cfg=f->service.config;
  uint8_t a[16]{},b[16]{};a[0]=17;b[0]=18;b[1]=3;mt::ChannelRecord first,last;
  assert(first.set(2,"Collision",a,sizeof(a))&&last.set(2,"Collision",b,sizeof(b))&&cfg.setSecondaryChannel(1,first)&&cfg.setSecondaryChannel(7,last));
  mt::PrimaryChannel::Selection ka,kb;assert(cfg.channelSelection(1,ka)&&cfg.channelSelection(7,kb)&&ka.hash==kb.hash&&ka.hash!=8&&!f->service.sameChannel(1,7));
  assert(f->service.apply(cfg)&&f->service.start(false,0)&&f->service.armEndpoint(true));
  assert((cfg.channelCandidates(kb.hash)&0x82)==0x82&&!f->service.sameChannel(1,7));
  bench::MtCodec ca,cb;assert(ca.setKey(ka.hash,a,sizeof(a))&&cb.setKey(kb.hash,b,sizeof(b)));mt::PrimaryChannel::clear(ka);mt::PrimaryChannel::clear(kb);
  // Query is admitted directly through actual Service on slot7; the recording
  // provider is not credited with any ID reservation or private operation.
  uint8_t query[255]{};size_t querySize=0;
  {mt::Service::ChannelScope scope(f->service,7);assert(scope.valid()&&cb.encodeTelemetryQuery(Node,peer,1200,query,sizeof(query),querySize)&&!f->service.submitTelemetryQuery(0,query,querySize,100));}
  f->service.poll(300,0);assert(f->radio.frames.size()==1);f->done(301);
  uint8_t data[128]{},frameA[255]{},frameB[255]{};size_t dataSize=0,na=0,nb=0;
  assert(mt::TelemetryQuery::uptimeResponse(123,1200,data,sizeof(data),dataSize));
  assert(ca.encodeZeroHopData(peer,Node,1201,data,dataSize,frameA,sizeof(frameA),na)&&cb.encodeZeroHopData(peer,Node,1201,data,dataSize,frameB,sizeof(frameB),nb));
  f->receive({frameA,frameA+na},1000);assert(!received(f->usb,67,1)&&!received(f->usb,67,7));
  f->receive({frameB,frameB+nb},1100);assert(received(f->usb,67,7)==1&&!received(f->usb,67,1));
  pass("colliding hashes try every key; wrong-key same-peer/request/packet response cannot complete another slot request");
}
static void wireCases() {
  constexpr uint32_t peer=0x87654321;
  for(bool ble:{false,true}) {
    auto a=std::make_unique<WireSide>(Node);auto b=std::make_unique<WireSide>(peer);
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
    p.id=901;p.to=0xffffffffu;p.channel=7;p.hop_limit=1;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;p.decoded.payload.size=3;memcpy(p.decoded.payload.bytes,"abc",3);
    input(ble?a->b:a->c,r,100);a->service.poll(300,0);assert(a->radio.frames.size()==1);
    mt::PrimaryChannel::Selection key;assert(a->service.config.channelSelection(7,key)&&a->radio.frames[0][13]==key.hash&&key.hash!=8);mt::PrimaryChannel::clear(key);
    b->receive(a->radio.frames[0],400);assert(received(b->usb,1,7)==1&&received(b->ble,1,7)==1);
    assert(a->service.selectedChannel()==0&&b->service.codec.channelHash()==8);
  }
  pass("USB and BLE secondary text origins decode on both peer transports with local channel7 and restored primary codec");
  auto a=std::make_unique<WireSide>(Node);auto b=std::make_unique<WireSide>(peer);b->service.measuredUptime(123);
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
  p.id=902;p.to=peer;p.channel=7;p.hop_limit=1;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_TELEMETRY_APP;p.decoded.want_response=true;p.decoded.payload.size=2;p.decoded.payload.bytes[0]=0x12;p.decoded.payload.bytes[1]=0;
  input(a->c,r,100);a->service.poll(300,0);assert(a->radio.frames.size()==1);a->done(301);
  b->receive(a->radio.frames[0],400);b->service.poll(800,0);assert(b->radio.frames.size()==1&&b->radio.frames[0][13]==a->radio.frames[0][13]);
  a->receive(b->radio.frames[0],1000);assert(received(a->usb,67,7)==1&&received(a->ble,67,7)==0);
  pass("secondary telemetry query keeps response key/hash and correlation owner across deferred immutable TX");
  {
    auto origin=std::make_unique<WireSide>(Node);auto target=std::make_unique<WireSide>(peer);
    target->service.stop();auto cfg=target->service.config;assert(cfg.setFixedPosition(123,-456,false,0));cfg.setPositionSharing(true);
    assert(target->service.apply(cfg)&&target->service.start(false,0)&&target->service.armEndpoint(true));
    uint8_t frame[255]{};size_t size=0;
    {mt::Service::ChannelScope selected(origin->service,7);assert(selected.valid()&&origin->service.encodePositionQuery(peer,1300,frame,sizeof(frame),size));}
    target->receive({frame,frame+size},1000);target->service.poll(1400,0);assert(target->radio.frames.empty());
    assert(origin->service.encodePositionQuery(peer,1301,frame,sizeof(frame),size));
    target->receive({frame,frame+size},1500);target->service.poll(1900,0);assert(target->radio.frames.size()==1&&target->radio.frames[0][13]==8);
  }
  pass("primary fixed-position sharing cannot leak through precision-zero secondary; primary positive control still replies");
  for(unsigned port:{1u,256u,67u}) {
    auto f=std::make_unique<WireSide>(Node,true);meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=r.packet;p.id=1001;p.to=peer;p.channel=7;p.hop_limit=1;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=static_cast<meshtastic_PortNum>(port);
    if(port==67)p.decoded.want_response=true;
    if(port==67){p.decoded.payload.size=2;p.decoded.payload.bytes[0]=0x12;p.decoded.payload.bytes[1]=0;}
    else {p.decoded.payload.size=3;memcpy(p.decoded.payload.bytes,"abc",3);}
    input(f->c,r,100);f->service.poll(300,0);
    assert(f->privateRuntime.privateCalls==1&&f->radio.frames.empty());
  }
  pass("installed private boundary preserves automatic PKI text/generic/telemetry unicast independent of local channel");
  {
    auto f=std::make_unique<WireSide>(Node,true);meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=r.packet;p.id=1002;p.to=peer;p.channel=7;p.hop_limit=1;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_POSITION_APP;p.decoded.want_response=true;
    input(f->c,r,100);f->service.poll(300,0);
    mt::PrimaryChannel::Selection key;assert(f->service.config.channelSelection(7,key));
    assert(f->radio.frames.size()==1&&f->radio.frames[0][13]==key.hash&&f->privateRuntime.privateCalls==0);
    const auto &wire=f->radio.frames[0];bench::RouteMetadata metadata{};
    {mt::Service::ChannelScope selected(f->service,7);assert(selected.valid()&&
      f->service.codec.decode(wire.data(),wire.size(),metadata)&&metadata.portnum==3&&metadata.wantResponse&&
      !metadata.payloadLength&&!metadata.signatureLength&&metadata.hasBitfield&&metadata.bitfield==2);}
    assert(bench::le32(wire.data())==peer&&bench::le32(wire.data()+4)==Node&&wire[12]==uint8_t(1|(1<<5)));
    mt::PrimaryChannel::clear(key);
    pass("stock-excluded Position query uses selected channel control encoding without invoking private submission");
  }
  {
    auto f=std::make_unique<WireSide>(Node,true);meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=r.packet;p.id=1003;p.to=peer;p.channel=7;p.hop_limit=1;p.pki_encrypted=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_POSITION_APP;p.decoded.want_response=true;
    input(f->c,r,100);f->service.poll(300,0);
    unsigned refusals=0;
    for(const auto &out:f->usb.packets)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&
      out.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP&&out.packet.decoded.request_id==p.id){
      meshtastic_Routing routing{};auto in=pb_istream_from_buffer(out.packet.decoded.payload.bytes,out.packet.decoded.payload.size);
      assert(pb_decode(&in,meshtastic_Routing_fields,&routing)&&!in.bytes_left&&
        routing.error_reason==meshtastic_Routing_Error_PKI_FAILED);++refusals;
    }
    assert(f->radio.frames.empty()&&f->privateRuntime.privateCalls==0&&f->service.endpointFree()==4&&refusals==1);
    pass("explicit PKI Position request remains refused without channel downgrade or private submission");
  }
}
#include "channel_bank/full_review_adversaries.inc"
int main(){test_channel_candidates_mask();pass("Qwen candidate mask includes every colliding hash and both edge slots; invalid roles and null inputs");configCases();phoneCases();wireCases();collisionCases();m95_full_review::crossChannelTransitResponseMustNotCancel();m95_full_review::crossChannelOriginRepeatMustNotReplace();m95_full_review::primaryChangePreservesEncryptedHashZeroSecondary();m95_full_review::customSecondaryCanLoadBeforePrivateRuntimeBind();pass("independent routing context, inherited hash-zero validity and custom-secondary prebind/reboot adversaries repaired");m95_early_proposal::traceCollision();pass("colliding-key traceroute upgrade and loop associations isolate effective channel key with positive controls");m95_early_proposal::generatedNodeInfoACK();pass("generated secondary NodeInfo control keeps context through matching ACK and rejects primary-channel ACK");}
