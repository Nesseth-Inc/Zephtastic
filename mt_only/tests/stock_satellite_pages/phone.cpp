// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include "NodeJournal.h"
#include "companion/SatelliteHistory.h"
#include <string>
#include <map>
static std::map<void*,size_t> pageAllocations;
static size_t inUse=0,peak=0;
static bool outOfHeap=false;
static void *trackedPageAllocate(size_t n){
    if(outOfHeap)return nullptr;
    void *p=malloc(n);assert(p);pageAllocations.emplace(p,n);
    inUse+=n;if(inUse>peak)peak=inUse;return p;
}
static void trackedPageRelease(void *p){
    if(!p)return;auto i=pageAllocations.find(p);assert(i!=pageAllocations.end());
    inUse-=i->second;pageAllocations.erase(i);free(p);
}
#define malloc trackedPageAllocate
#define free trackedPageRelease
#include "companion/SatellitePageOwner.h"
#undef free
#undef malloc
#include "companion/PeerDirectoryOwner.h"
#include <map>
#include "../stock_warm_peers/Map.h"
using namespace mt::companion;
struct FileMap:Map {
    bool readFault=false,stageFault=false;
    int dropStage(){return stageFault?-EIO:0;}
    int dropBank(int page,unsigned bank){retire(page,bank);return 0;}
    using Map::get;
    int get(int page,unsigned bank,void *data,size_t n){
        return readFault?-EIO:Map::get(page,bank,data,n);
    }
};
struct JournalSettings:mt::SettingsBackend {
    std::map<std::string,std::vector<uint8_t>> rows;
    int read(const char *k,uint8_t *out,size_t n)override{
        auto row=rows.find(k);if(row==rows.end())return -ENOENT;if(row->second.size()>n)return -E2BIG;
        memcpy(out,row->second.data(),row->second.size());return int(row->second.size());
    }
    int write(const char *k,const uint8_t *p,size_t n)override{rows[k]={p,p+n};return 0;}
};
struct ReportHost:Host {
    PeerDirectoryOwner<119,2,40> dir;
    SatellitePageOwner<40,2> pages;
    FileMap dio,pio;
    bool blocked=false;unsigned offers=0;
    ReportHost(mt::Service &s):Host(s){
        decltype(dir)::Domain d{};d.node=OURS;d.key[0]=7;d.epoch[0]=2;
        assert(!dir.open(dio,d,4));pages.bind(dir.warmDomain().directory);
    }
    void satelliteReceived(uint32_t id,unsigned kind,unsigned channel,SatelliteReplay::Observation obs,
         const uint8_t *body,size_t size)override{
        ++offers;const int slot=s.nodes.find(id);
        int rc=pages.offer(dir.ticket(id),id,kind,channel,obs,body,size);
        if(rc==-EAGAIN&&slot>=0)pages.dirty(unsigned(slot),kind);else assert(!rc);
    }
    int satellitePacket(unsigned owner,unsigned kind,unsigned ordinal,SatelliteSample &r)override{
        return pages.request(dir,owner,kind,ordinal,r);
    }
    bool send(const uint8_t *p,size_t n)override{return !blocked&&Host::send(p,n);}
};
static std::vector<uint8_t> encoded(const pb_msgdesc_t *type,const void *v){
    std::vector<uint8_t> out(233);auto b=pb_ostream_from_buffer(out.data(),out.size());assert(pb_encode(&b,type,v));out.resize(b.bytes_written);return out;
}
static void liveWire(){
    mt::NodeCache nodes;mt::NodeCache::Entry identity;identity.identity(PEER,81,5);
    assert(nodes.accept(identity,OURS,-11,2)>=0&&nodes.observeReceivedPacket(PEER,1700000030,true));
    SatelliteReplay live;meshtastic_Position pos{};pos.has_latitude_i=pos.has_longitude_i=true;
    pos.longitude_i=300;pos.precision_bits=22;mt::PeerPosition position;assert(position.assign(pos));
    assert(nodes.observePosition(PEER,position,true));meshtastic_DeviceMetrics dm{};
    dm.has_battery_level=true;dm.battery_level=102;mt::PeerDeviceMetrics device;assert(device.assign(dm));
    assert(nodes.observeDeviceMetrics(PEER,device,true));mt::PeerStatus status;char msg[80]="online";assert(status.assign(msg));
    assert(nodes.observeStatus(PEER,status,true));meshtastic_Telemetry env{};env.which_variant=meshtastic_Telemetry_environment_metrics_tag;
    env.variant.environment_metrics.has_temperature=true;env.variant.environment_metrics.temperature=-7;
    const auto body=encoded(meshtastic_Telemetry_fields,&env);nodes.environmentReplay(PEER,true);
    for(unsigned k=0;k<4;k++)for(bool retained:{false,true}){
        auto kind=static_cast<SatelliteReplay::Kind>(k);
        assert(live.remember(nodes,PEER,kind,k,k==2?body.data():nullptr,k==2?body.size():0,
                             {1700000000,-29,uint8_t(retained?32+8+3:0)}));
        meshtastic_MeshPacket packet{};assert(SatelliteWire::livePacket(*nodes.get(0),live,kind,5,packet));
        assert(packet.from==PEER&&packet.to==UINT32_MAX&&packet.channel==k&&packet.rx_time==1700000030);
        assert(packet.rx_snr==(retained?-7.25f:-11.f)&&packet.via_mqtt==!retained);
        assert(packet.hop_start==5&&packet.hop_limit==(retained?2:3));
        const auto &d=packet.decoded;auto stream=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
        if(k==0){meshtastic_Position value{};assert(d.portnum==meshtastic_PortNum_POSITION_APP&&
            pb_decode(&stream,meshtastic_Position_fields,&value)&&!value.has_latitude_i&&value.longitude_i==300&&value.precision_bits==22);}
        else if(k==3){meshtastic_StatusMessage value{};assert(d.portnum==meshtastic_PortNum_NODE_STATUS_APP&&
            pb_decode(&stream,meshtastic_StatusMessage_fields,&value)&&!strcmp(value.status,msg));}
        else{meshtastic_Telemetry value{};assert(d.portnum==meshtastic_PortNum_TELEMETRY_APP&&
            pb_decode(&stream,meshtastic_Telemetry_fields,&value)&&value.time==packet.rx_time);
            if(k==1)assert(value.which_variant==2&&value.variant.device_metrics.battery_level==102);
            else assert(value.which_variant==3&&value.variant.environment_metrics.temperature==-7);}
    }
    puts("PASS paged profile live hot replay preserves four donor packet kinds, zero coordinate omission, age, hops, MQTT and restored quarter-dB SNR");
}
static void heapBudget(){
    assert(!inUse&&pageAllocations.empty());peak=0;
    auto f=std::make_unique<Fixture>();ReportHost h(f->s);
    uint8_t key[32];memset(key,7,sizeof(key));uint8_t bytes[233];memset(bytes,42,sizeof(bytes));
    for(unsigned i=0;i<5;i++){
        mt::NodeCache::Entry e;e.identity(PEER+i,81,5);e.bytes[e.KeyLength]=32;memcpy(e.bytes+e.Key,key,sizeof(key));
        h.dir.observe(e);assert(!h.dir.poll(h.dio,4));
    }
    assert(!h.pages.offer(h.dir.ticket(PEER),PEER,3,0,{},bytes,79,key,32));
    for(unsigned i=0;i<20;i++)assert(!h.pages.poll(h.dir,h.dio,h.pio));
    assert(!inUse);SatelliteSample found{};
    for(unsigned i=0;i<2;i++)assert(h.pages.request(h.dir,i,3,0,found)==-EAGAIN);
    for(unsigned i=0;i<8;i++)assert(!h.pages.poll(h.dir,h.dio,h.pio));
    for(unsigned i=0;i<2;i++)assert(h.pages.request(h.dir,i,3,0,found)==1);
    outOfHeap=true;assert(h.pages.offer(h.dir.ticket(PEER),PEER,2,0,{},bytes,233,key,32)==-EAGAIN);outOfHeap=false;
    for(unsigned i=0;i<4;i++)assert(!h.pages.offer(h.dir.ticket(PEER+i),PEER+i,2,0,{},bytes,233,key,32));
    assert(h.pages.offer(h.dir.ticket(PEER+4),PEER+4,2,0,{},bytes,233,key,32)==-EAGAIN);
    assert(pageAllocations.size()==6&&inUse==peak&&peak<=1792);
    printf("PASS four full keyed pending reports and two backpressured readers use %zu requested heap bytes in six allocations\n",peak);
    h.pages.close();assert(!inUse&&pageAllocations.empty());
}
static void correctedClock(){
    using E=mt::NodeCache::Entry;
    constexpr uint32_t corrected=1700000000u,future=corrected+86400,others=corrected+20000;
    auto fixture=std::make_unique<Fixture>();ReportHost h(fixture->s);
    auto receive=[&](uint32_t id,unsigned kind,uint32_t utc,const char *body){
        assert(!h.pages.offer(h.dir.ticket(id),id,kind,0,{utc,0,8},
                             reinterpret_cast<const uint8_t*>(body),strlen(body)));
    };
    auto pump=[&](){for(unsigned i=0;i<20;i++)assert(!h.pages.poll(h.dir,h.dio,h.pio));};
    auto cold=[&](unsigned kind){
        ReportHost boot(fixture->s);boot.dio=h.dio;boot.pio=h.pio;
        decltype(h.dir)::Domain d{};d.node=OURS;d.key[0]=7;d.epoch[0]=2;
        assert(!boot.dir.open(boot.dio,d,4));boot.pages.bind(boot.dir.warmDomain().directory);
        SatelliteSample sample{};assert(boot.pages.request(boot.dir,0,kind,0,sample)==-EAGAIN);
        for(unsigned i=0;i<4;i++)assert(!boot.pages.poll(boot.dir,boot.dio,boot.pio));
        assert(boot.pages.request(boot.dir,0,kind,0,sample)==1);
        boot.pages.close();return sample;
    };
    E peer{};peer.identity(PEER,81,5);
    for(unsigned i=0;i<41;i++){
        E e{};e.identity(PEER+i,81,5);E::put32(e.lastHeardBytes,i?others:future);
        h.dir.observe(e);assert(!h.dir.poll(h.dio,4));
        if(i<40){receive(PEER+i,3,i?others:future,"old");pump();}
    }
    // The status is already at the front when a later status supersedes a
    // device packet received in between. Queue order is no longer wire order.
    receive(PEER,3,future+1,"early");receive(PEER,1,future+2,"d");
    E pendingNode{};
    assert(h.dir.identity(1,PEER,pendingNode)==-EAGAIN);
    assert(!h.dir.poll(h.dio,4));
    assert(h.dir.identity(1,PEER,pendingNode)==1);
    receive(PEER,3,corrected,"corrected");pump();
    assert(h.dir.identity(1,PEER,pendingNode)==1&&pendingNode.lastHeard()==corrected);
    auto s=cold(3);assert(s.utc==corrected&&s.size==9&&!memcmp(s.bytes,"corrected",9));
    assert(cold(1).utc==corrected);
    mt::NodeCache empty;uint16_t online=0;
    assert(h.dir.visibleCount(empty,corrected+7200,&online)==41&&online==41);
    SatelliteOwner owner{};uint32_t heard=0;assert(!h.dir.binding(h.dio,PEER,owner,heard)&&heard==corrected);
    // Replaying an untimed NodeJournal seed on boot must preserve this owner's
    // stable clock; likewise an untrusted incoming report can't erase it.
    h.dir.observe(peer);assert(!h.dir.poll(h.dio,4));receive(PEER,3,0,"no clock");pump();
    assert(cold(3).utc==corrected);
    // A page generation can change while only the neighbor is edited. An
    // untimed receive on the newer ticket cannot drop the prior known clock.
    receive(PEER,3,corrected-3,"dated");
    E neighbor{};neighbor.identity(PEER+1,81,6);E::put32(neighbor.lastHeardBytes,others);
    h.dir.observe(neighbor);assert(!h.dir.poll(h.dio,4));
    receive(PEER,3,0,"undated");pump();assert(cold(3).utc==corrected-3);
    // NodeInfo has its own deferred directory write. Its observed timestamp
    // must survive an earlier report still waiting on the media.
    receive(PEER,3,future,"older RF");h.pages.supersedeHeader(PEER);
    E::put32(peer.lastHeardBytes,corrected+1);h.dir.observe(peer);
    assert(!h.dir.poll(h.dio,4));pump();assert(cold(3).utc==corrected+1);
    // Lose power after the directory update and before publishing the body.
    // A confirmed new header can accompany the previous complete payload.
    h.pio.fail=true;receive(PEER,3,corrected-1,"interrupted");int err=0;
    for(unsigned i=0;i<20&&!err;++i)err=h.pages.poll(h.dir,h.dio,h.pio);
    assert(err==-EIO);auto afterCut=cold(3);
    assert(afterCut.utc==corrected-1&&afterCut.size==8&&!memcmp(afterCut.bytes,"older RF",8));
    h.pio.fail=false;pump();assert(cold(3).size==11);
    // All 40 report slots are live. Eviction must select the corrected peer,
    // while its independent device report remains replayable.
    receive(PEER+40,3,others,"new peer");pump();
    SatellitePages<40> view;view.bind(h.dir.warmDomain().directory);
    for(unsigned i=0;i<40;i++){SatelliteSample entry{};assert(!view.read(h.pio,3,i,entry)&&entry.owner.id!=PEER);}
    assert(cold(1).owner.id==PEER);h.pages.close();
    puts("PASS UTC backstep preserves receive order across coalesced reports, newer NodeInfo, reboot seed, interrupted body write, online census and independent map eviction");
}
int main(){
    auto f=std::make_unique<Fixture>();ReportHost h(f->s);mt::companion::CompanionWorkspace ws;mt::Companion phone(f->s,h,81,ws,0);
    mt::NodeCache::Entry peer;peer.identity(PEER,81,5);peer.bytes[peer.KeyLength]=32;peer.bytes[peer.Key]=5;
    h.dir.observe(peer);assert(!h.dir.poll(h.dio,4));
    auto deliver=[&](meshtastic_PortNum port,const pb_msgdesc_t *type,const void *value,uint32_t to,unsigned id,uint32_t observed=0){
        auto payload=encoded(type,value);meshtastic_Data d{};d.has_bitfield=true;d.portnum=port;
        memcpy(d.payload.bytes,payload.data(),payload.size());d.payload.size=payload.size();auto data=encoded(meshtastic_Data_fields,&d);
        uint8_t frame[255];size_t n=0;assert(f->s.codec.encodeZeroHopData(PEER,to,id,data.data(),data.size(),frame,sizeof(frame),n));
        auto reads=h.pio.reads;phone.receive(frame,n,-80,-8,id,observed?observed:1700000000+id,true,uint8_t(-29),true,mt::Companion::WorkspaceProof{true});assert(h.pio.reads==reads);
    };
    meshtastic_Position pos{};pos.has_latitude_i=pos.has_longitude_i=true;pos.latitude_i=300;pos.longitude_i=900;pos.time=1700000999;
    meshtastic_StatusMessage status{};strcpy(status.status,"public");
    meshtastic_Telemetry t{};t.which_variant=meshtastic_Telemetry_device_metrics_tag;t.variant.device_metrics.has_voltage=true;t.variant.device_metrics.voltage=3.55;
    deliver(meshtastic_PortNum_POSITION_APP,meshtastic_Position_fields,&pos,UINT32_MAX,1);
    deliver(meshtastic_PortNum_TELEMETRY_APP,meshtastic_Telemetry_fields,&t,UINT32_MAX,2);
    t={};t.which_variant=meshtastic_Telemetry_environment_metrics_tag;t.variant.environment_metrics.has_temperature=true;t.variant.environment_metrics.temperature=12;
    deliver(meshtastic_PortNum_TELEMETRY_APP,meshtastic_Telemetry_fields,&t,UINT32_MAX,3);
    deliver(meshtastic_PortNum_NODE_STATUS_APP,meshtastic_StatusMessage_fields,&status,UINT32_MAX,4);
    strcpy(status.status,"private only");deliver(meshtastic_PortNum_NODE_STATUS_APP,meshtastic_StatusMessage_fields,&status,OURS,5);
    assert(h.offers==4);
    pos.time=0;pos.latitude_i=310;deliver(meshtastic_PortNum_POSITION_APP,meshtastic_Position_fields,&pos,UINT32_MAX,6);
    for(unsigned i=0;i<70;i++)assert(!h.pages.poll(h.dir,h.dio,h.pio));
    pos.latitude_i=320;deliver(meshtastic_PortNum_POSITION_APP,meshtastic_Position_fields,&pos,UINT32_MAX,7);
    for(unsigned i=0;i<20;i++)assert(!h.pages.poll(h.dir,h.dio,h.pio));
    assert(f->s.nodes.find(PEER)<0);
    ReportHost reboot(f->s);reboot.dio=h.dio;reboot.pio=h.pio;auto d=h.dir.warmDomain();
    decltype(h.dir)::Domain outer{};outer.node=OURS;outer.key[0]=7;outer.epoch[0]=2;assert(!reboot.dir.open(reboot.dio,outer,4));reboot.pages.bind(d.directory);
    mt::companion::CompanionWorkspace w2;mt::Companion reader(f->s,reboot,81,w2,0);meshtastic_ToRadio req{};req.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;req.want_config_id=69421;
    input(reader,req,55);
    for(unsigned i=0;i<1100;i++){const auto reads=reboot.pio.reads;reboot.blocked=i%5==0;
        reader.poll(mt::Companion::WorkspaceProof{true});assert(reads==reboot.pio.reads);reboot.blocked=false;assert(!reboot.pages.poll(reboot.dir,reboot.dio,reboot.pio));}
    unsigned count=0;
    for(auto &m:reboot.out)if(m.which_payload_variant==meshtastic_FromRadio_packet_tag){
        const auto &p=m.packet;assert(p.from==PEER&&p.to==UINT32_MAX&&p.channel==0&&p.rx_snr==-7.25f&&p.has_rx_time);
        if(p.decoded.portnum==meshtastic_PortNum_POSITION_APP){meshtastic_Position out{};auto is=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);
            assert(pb_decode(&is,meshtastic_Position_fields,&out)&&out.time==1700000999&&out.latitude_i==320);}
        if(p.decoded.portnum==meshtastic_PortNum_NODE_STATUS_APP){meshtastic_StatusMessage st{};auto is=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);
            assert(pb_decode(&is,meshtastic_StatusMessage_fields,&st)&&!strcmp(st.status,"public"));}
        ++count;
    }
    assert(count==4);
    // A known hot peer accepts a directed update. Withdraw its superseded
    // public status without passing any private payload to the archive.
    assert(f->s.nodes.accept(peer,OURS,-7,0)>=0);
    deliver(meshtastic_PortNum_NODE_STATUS_APP,meshtastic_StatusMessage_fields,&status,OURS,8);
    assert(h.offers==7);
    for(unsigned i=0;i<20;i++)assert(!h.pages.poll(h.dir,h.dio,h.pio));
    SatellitePages<40> view;view.bind(d.directory);SatelliteSample report{};
    assert(view.read(h.pio,3,0,report)==-ENOENT&&!view.read(h.pio,2,0,report));
    // A different channel cannot inherit a previous source's position time.
    uint8_t wire[32];size_t n=0;mt::PeerPosition next;assert(next.assign(pos)&&SatelliteWire::position(next,wire,n));
    assert(!h.pages.offer(h.dir.ticket(PEER),PEER,0,2,{1700000200,3,8},wire,n));
    for(unsigned i=0;i<20;i++)assert(!h.pages.poll(h.dir,h.dio,h.pio));
    assert(!view.read(h.pio,0,0,report));meshtastic_Position switched{};auto in=pb_istream_from_buffer(report.bytes,report.size);
    assert(pb_decode(&in,meshtastic_Position_fields,&switched)&&!switched.time&&report.channel==2);
    // A queued report and an already-prefetched phone row cannot cross a
    // same-key reenrollment, even when it reuses the same slot.
    SatelliteSample prefetched{};
    assert(h.pages.request(h.dir,0,1,0,prefetched)==-EAGAIN);
    for(unsigned i=0;i<5;i++)assert(!h.pages.poll(h.dir,h.dio,h.pio));
    assert(h.pages.request(h.dir,0,1,0,prefetched)==1);
    assert(!h.pages.offer(h.dir.ticket(PEER),PEER,3,0,{},(const uint8_t*)"old",3));
    assert(!h.dir.forget(h.dio,PEER));h.dir.observe(peer);assert(!h.dir.poll(h.dio,4));
    assert(h.pages.request(h.dir,0,1,0,prefetched)!=1);
    h.pages.disconnect(0);
    for(unsigned i=0;i<30;i++)assert(!h.pages.poll(h.dir,h.dio,h.pio));
    assert(view.read(h.pio,3,0,report)==-ENOENT);
    assert(h.pages.request(h.dir,0,1,0,prefetched)==-EAGAIN);
    for(unsigned i=0;i<5;i++)assert(!h.pages.poll(h.dir,h.dio,h.pio));
    assert(h.pages.request(h.dir,0,1,0,prefetched)==2);
    // Seed both public rows for this owner, then simulate a cold boot that
    // cannot read its private directory. Actual Companion::receive sees the
    // fresh shared position and targeted status during the outage.
    JournalSettings settings;mt::NodeJournal journal;
    assert(!journal.load(settings,f->s.nodes,OURS));journal.enableReplayEpochs(f->s.nodes);
    assert(f->s.nodes.accept(peer,OURS,-7,0)>=0&&journal.flush(settings,f->s.nodes,0,true,true)==1);
    assert(f->s.nodes.replayEpoch(0));
    for(unsigned i=0;i<2;i++){
        auto k=i?SatelliteSample::Status:SatelliteSample::Position;
        const auto *data=i?(const uint8_t*)"old status":wire;
        assert(!h.pages.offer(h.dir.ticket(PEER),PEER,k,0,{},data,i?10:n));
    }
    for(unsigned i=0;i<24;i++)assert(!h.pages.poll(h.dir,h.dio,h.pio));
    SatelliteHistory history;FileMap warm;h.dio.readFault=true;h.pages.close();
    auto open=[&](){return h.pages.openDirectory(h.dir,h.dio,outer,4,h.pio,warm,true,history);};
    assert(open()==-EIO&&!history.ready()&&!h.pages.bound());
    pos.latitude_i=555;pos.time=1700050000;
    t.which_variant=meshtastic_Telemetry_environment_metrics_tag;t.variant.environment_metrics.has_temperature=true;
    t.variant.environment_metrics.temperature=18;
    deliver(meshtastic_PortNum_TELEMETRY_APP,meshtastic_Telemetry_fields,&t,UINT32_MAX,14,1700200000);
    deliver(meshtastic_PortNum_POSITION_APP,meshtastic_Position_fields,&pos,UINT32_MAX,12,1700010000);
    deliver(meshtastic_PortNum_NODE_STATUS_APP,meshtastic_StatusMessage_fields,&status,OURS,13,1700300000);
    assert(f->s.nodes.get(0)->lastHeard()==1700300000&&f->s.satellites.publicHeader(PEER)==1700010000);
    assert(f->s.nodes.get(0)->replayPosition&&!f->s.nodes.get(0)->status.replayable());
    h.dio.readFault=false;assert(!open()&&history.ready());
    auto reconcile=[&](){for(unsigned i=0;i<150;i++){
        h.pages.reconcile(f->s.nodes,f->s.satellites,h.dir);
        assert(!h.pages.poll(h.dir,h.dio,h.pio));}};
    reconcile();unsigned cursor=0;
    for(unsigned i=0;i<4;i++){
        h.dir.capture(f->s.nodes,4,cursor,[&](const mt::NodeCache::Entry &e){
            h.dir.observe(e,f->s.satellites.publicHeader(e.id()));
        });
        assert(!h.dir.poll(h.dio,4));
    }
    SatelliteOwner liveOwner{};uint32_t publicUtc=0;
    assert(!h.dir.binding(h.dio,PEER,liveOwner,publicUtc)&&publicUtc==1700010000);
    view.bind(h.dir.warmDomain().directory);
    assert(view.read(h.pio,3,0,report)==-ENOENT&&!view.read(h.pio,0,0,report));
    in=pb_istream_from_buffer(report.bytes,report.size);meshtastic_Position fresh{};
    assert(pb_decode(&in,meshtastic_Position_fields,&fresh)&&fresh.latitude_i==555&&fresh.time==1700050000);
    // A committed channel rename clears the live observations. The selector
    // changes successfully, then stale-stage cleanup fails on the new domain.
    history.attempted(0,true);auto changed=f->s.config;
    assert(changed.setPrimaryName("Recovery"));f->s.stop();assert(f->s.apply(changed));
    assert(!f->s.nodes.get(0)->sampleMask());outer.channels[0]^=0x70;
    h.pio.stageFault=true;assert(open()==-EIO&&!history.ready()&&!h.pages.bound()&&history.due(10));
    history.attempted(10,false);assert(!history.due(60009)&&history.due(60010));
    h.pio.stageFault=false;assert(!open()&&history.ready());view.bind(h.dir.warmDomain().directory);
    h.dir.observe(peer);assert(!h.dir.poll(h.dio,4));reconcile();
    assert(view.read(h.pio,0,0,report)==-ENOENT&&view.read(h.pio,3,0,report)==-ENOENT);
    assert(!h.dir.fenceJournal(h.dio,4,5));
    puts("PASS late media preserves newer public radio data and private withdrawal, changed-channel cleanup failure reopens after retry without reviving prior rows");
    h.pages.close();reboot.pages.close();heapBudget();liveWire();correctedClock();
    puts("PASS cold queued and durable Position time, cross-channel isolation, private hot replacement and exact enrollment of prefetched/queued records");
    puts("PASS real RF channel decode to four cold reports, direct data exclusion, reboot and backpressured phone replay without hot peer or new trust");
}
