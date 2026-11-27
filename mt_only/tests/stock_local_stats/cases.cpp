#include "stock/telemetry/NoiseFloor.h"
// SPDX-License-Identifier: GPL-3.0-only
#include "telemetry_query_fixture.h"
#include "ModuleController.h"
#include "companion/PeerDirectoryOwner.h"
#include <map>
#include "../stock_warm_peers/Map.h"

namespace donor {
static uint32_t utc=1700000000,elapsed=60;
static uint32_t getTime(){return utc;}
namespace Time {static uint32_t getUptimeSecs(){return elapsed;}}
struct Air {float channelUtilizationPercent(){return 0;}float utilizationTXPercent(){return 0;}} air;
static auto *airTime=&air;
struct meshtastic_NodeInfoLite {uint32_t last_heard;bool mqtt=false;};
static bool nodeInfoLiteViaMqtt(const meshtastic_NodeInfoLite *n){return n->mqtt;}
struct NodeDB {
    std::vector<meshtastic_NodeInfoLite> rows;
    decltype(rows) *meshNodes=&rows;
    int numMeshNodes=0;
    unsigned getNumMeshNodes(){return rows.size();}
    size_t getNumOnlineMeshNodes(bool);
} db;
static auto *nodeDB=&db;
struct RadioLibInterface {
    inline static RadioLibInterface *instance;
    uint32_t txGood=0,rxGood=8,rxBad=2,txRelay=0,txDrop=0;
    bool sampled=false;
    bool hasNoiseFloorSamples(){return sampled;}
    int getAverageNoiseFloor(){return -92;}
} radio;
struct Memory {
    uint32_t getHeapSize(){return 16384;}
    uint32_t getFreeHeap(){return 13000;}
} memGet;
struct Router {uint32_t rxDupe=0,txRelayCanceled=0;} route;
static auto *router=&route;
struct DeviceTelemetryModule {uint16_t numOnlineNodes=0; meshtastic_Telemetry getLocalStatsTelemetry();};
#define LOG_INFO(...) ((void)0)
#include "oracle.inc"
#undef LOG_INFO
}

static mt::NodeCache::Entry named(uint32_t id,uint32_t heard){
    mt::NodeCache::Entry e;e.identity(id,81,0);
    strcpy(reinterpret_cast<char*>(e.bytes+e.Name),"Peer");
    strcpy(reinterpret_cast<char*>(e.bytes+e.Short),"PR");
    e.heard=true;e.put32(e.lastHeardBytes,heard);return e;
}

template<unsigned Limit,unsigned Rows> struct Census {
    using Owner=mt::companion::PeerDirectoryOwner<Limit,2,Rows>;
    static inline Owner owner;
    static inline uint32_t captured=1700000000;
    static void observe(const mt::NodeCache &hot,mt::LocalStatsObservation &out){
        out.totalNodes=1+owner.visibleCount(hot,captured,&out.onlineNodes);
        out.noiseFloor=donor::radio.sampled?-92:0;out.badRx=2;out.packetsRx=10;out.heapTotal=16384;out.heapFree=13000;
    }
    static void run(uint8_t model){
        Fixture f(true,model);Map storage;typename Owner::Domain domain{};
        domain.node=OURS;domain.key[0]=1;domain.epoch[0]=1;
        assert(!owner.open(storage,domain,1));
        donor::db.rows={{captured}};
        for(unsigned i=0;i<Limit;i++){
            const uint32_t stamps[]={captured-7199,captured-7200,captured+5,0};
            const uint32_t t=stamps[i%4];auto e=named(1000+i,t);
            owner.observe(e);assert(!owner.poll(storage,1));donor::db.rows.push_back({t});
        }
        auto live=named(1000,captured-9000);assert(f.s.nodes.accept(live,OURS,0,0)>=0);
        assert(f.s.nodes.observeLastHeard(1000,live.lastHeard()));
        donor::db.rows[1].last_heard=live.lastHeard();
        donor::db.numMeshNodes=donor::db.rows.size();
        donor::DeviceTelemetryModule module;
        module.numOnlineNodes=donor::db.getNumOnlineMeshNodes(false);
        assert(!owner.open(storage,domain,1));uint16_t online=999;
        assert(owner.visibleCount(f.s.nodes,captured,&online)==Limit&&!online);
        constexpr auto pages=(Limit+Rows-1)/Rows;
        for(unsigned p=0;p<pages;p++){
            auto before=storage.reads;assert(!owner.poll(storage,1));assert(storage.reads==before+1);
        }
        auto reads=storage.reads;
        assert(owner.visibleCount(f.s.nodes,captured,&online)==Limit&&online==module.numOnlineNodes);
        assert(owner.visibleCount(f.s.nodes,0,&online)==Limit&&!online);
        f.s.bindLocalStats(observe);
        donor::RadioLibInterface::instance=&donor::radio;
        donor::elapsed=60;auto expected=module.getLocalStatsTelemetry();
        for(unsigned transport=0;transport<2;transport++){
            auto &phone=transport?f.bc:f.c;auto &host=transport?f.bh:f.h;
            host.uptime=60;host.uptimeValid=true;
            auto q=query(77,OURS,0);q.packet.decoded.payload.size=2;q.packet.decoded.payload.bytes[0]=0x32;
            if(transport)input(phone,q,161);
            else{
                uint8_t frame[512]{};auto out=pb_ostream_from_buffer(frame+4,sizeof(frame)-4);assert(pb_encode(&out,meshtastic_ToRadio_fields,&q));
                frame[0]=0x94;frame[1]=0xc3;frame[2]=out.bytes_written>>8;frame[3]=out.bytes_written;
                for(size_t i=0;i<4+out.bytes_written;i++)phone.byte(frame[i],161,0,mt::Companion::WorkspaceProof{true});
            }
            std::vector<uint8_t> payload;assert(telemetries(host,77,&payload)==1);
            expected.time=host.wallTime();assert(payload==encodeTelemetry(expected));
        }
        donor::elapsed=90;expected=module.getLocalStatsTelemetry();
        mt::ModuleController controller;mt::ModuleSettings config;unsigned reports=0;
        auto sink=[&](const uint8_t *p,size_t n,uint32_t id,bool idle){
            meshtastic_Data d{};auto in=pb_istream_from_buffer(p,n);assert(id&&pb_decode(&in,meshtastic_Data_fields,&d));
            if(d.portnum!=meshtastic_PortNum_TELEMETRY_APP)return;
            meshtastic_Telemetry t{};in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&in,meshtastic_Telemetry_fields,&t));
            if(t.which_variant!=meshtastic_Telemetry_local_stats_tag)return;
            assert(idle&&!d.request_id);expected.time=captured;assert(encodeTelemetry(t)==encodeTelemetry(expected));++reports;
        };
        controller.pollTimed(f.s,config,true,0,1,captured-90,sink);
        controller.pollTimed(f.s,config,true,90000,2,captured,sink);
        assert(reports==1&&storage.reads==reads);
        owner.invalidate(1002);
        assert(owner.visibleCount(f.s.nodes,captured,&online)==Limit-1&&online==module.numOnlineNodes-1);
        assert(!owner.forget(storage,1002));
        assert(!owner.clear(storage)&&owner.visibleCount(f.s.nodes,captured,&online)==1&&online==1);
        owner.close();
    }
};
static void terminalRelays(){
    Fixture f;auto cfg=f.s.config;assert(cfg.setDeviceRole(mt::DeviceRolePolicy::Role::CLIENT,0));
    f.s.stop();assert(f.s.apply(cfg)&&f.s.start(false,0)&&f.s.armEndpoint(true)&&f.s.relaying());
    auto frame=f.wire(PEER,0x88445566u,501,0,0,(3u<<5)|2u,true);
    f.rx(frame,100);assert(f.s.route.pending());
    auto copy=frame;copy[12]=(3u<<5)|1u;copy[15]=0x44;
    f.rx(copy,110);
    auto stats=f.s.localStats();
    assert(!f.s.route.pending()&&stats.duplicates==1&&stats.canceled==1&&!stats.relayed);
    frame=f.wire(PEER,0x88445566u,502,0,0,(3u<<5)|2u,true);
    f.rx(frame,1000);
    for(unsigned ms=2000;ms<10000&&!f.radio.sends;ms+=500)f.s.poll(ms,42);
    assert(f.radio.sends==1&&!f.s.localStats().relayed);
    f.done(10001);stats=f.s.localStats();assert(stats.packetsTx==1&&stats.relayed==1);
    f.done(10002);assert(f.s.localStats().packetsTx==1);
    // Failed completions never add a transmitted packet or relay.
    frame=f.wire(PEER,0x88445566u,503,0,0,(3u<<5)|2u,true);f.rx(frame,11000);
    for(unsigned ms=12000;ms<20000&&f.radio.sends==1;ms+=500)f.s.poll(ms,78);
    assert(f.radio.sends==2);f.radio.doneStatus=-EIO;f.done(20001);
    assert(f.s.localStats().packetsTx==1&&f.s.localStats().relayed==1);
}
namespace noise_reference {
struct Time {static inline uint32_t now; static uint32_t getMillis(){return now;}};
struct Throttle {static bool isWithinTimespanMs(uint32_t last,uint32_t interval){return Time::now-last<interval;}};
struct RadioLibInterface {
  static constexpr int NOISE_FLOOR_UPDATE_INTERVAL_MS=5000,NOISE_FLOOR_SAMPLES=20,NOISE_FLOOR_INVALID=-128,NOISE_FLOOR_VALID_MIN=-127,NOISE_FLOOR_DEFAULT=-120;
  bool isReceiving=true, busy=false, irq=false, isNoiseFloorBufferFull=false;
  const void *sendingPacket=nullptr;
  int16_t rssi=0; uint8_t currentSampleIndex=0; uint32_t lastNoiseFloorUpdate=0;
  int currentNoiseFloor=NOISE_FLOOR_DEFAULT, noiseFloorSamples[20]{};
  bool isActivelyReceiving(){return busy;} bool isIRQPending(){return irq;}
  int16_t getCurrentRSSI(){return rssi;}
  void updateNoiseFloor(); bool hasNoiseFloorSamples();
  uint8_t getNoiseFloorSampleCountInternal()const;int32_t getAverageNoiseFloorInternal()const;
};
#define LOG_DEBUG(...)
#define LOG_TRACE(...)
#include "noise.inc"
#undef LOG_DEBUG
#undef LOG_TRACE
void compare(){
  mt::stock::telemetry::NoiseFloor local;
  RadioLibInterface stock;
  uint32_t now=0xffff7000;
  for(unsigned i=0;i<240;++i,now+=1357){
    Time::now=now;stock.rssi=i%6==0?-128:i%9==0?0:-int16_t(1+i%127);
    stock.busy=i%7==0;stock.irq=i%11==0;stock.isReceiving=i%13!=0;
    stock.sendingPacket=i%17==0?&stock:nullptr;
    stock.updateNoiseFloor();
    local.poll(now,[&]{return stock.isReceiving&&!stock.sendingPacket&&!stock.busy&&!stock.irq;},[&]{return stock.rssi;});
    assert(local.measured()==(stock.hasNoiseFloorSamples()?stock.getAverageNoiseFloorInternal():0));
  }
  assert(stock.isNoiseFloorBufferFull);
}
}
int main(){
    noise_reference::compare();
    terminalRelays();
    Census<119,40>::run(127);
    donor::radio.sampled=true;
    Census<199,16>::run(81);
    puts("PASS pinned LocalStats byte comparison on USB, BLE and scheduled telemetry, complete 120/200-node census, source ages, redaction, bounded reads");
}
