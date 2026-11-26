// SPDX-License-Identifier: GPL-3.0-only
#include "HopScalingModule.h" // independently compiled donor
#include "stock/modules/HopScalingModule.h"
#include "stock/modules/TraceRouteModule.h"
#include "ChannelWindow.h"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <initializer_list>

StockConfig config{};
struct StockHop : HopScalingModule { using HopScalingModule::runOnce; };

namespace donor {
constexpr unsigned NODENUM_BROADCAST=UINT32_MAX, ROUTE_SIZE=8;
struct {uint32_t my_node_num=42;} myNodeInfo;
template<class T> T clamp(T value,T lo,T hi){return std::min(std::max(value,lo),hi);}
struct TraceRouteModule {
    void insertUnknownHops(meshtastic_MeshPacket &,meshtastic_RouteDiscovery *,bool);
    void appendMyIDandSNR(meshtastic_RouteDiscovery *,float,bool,bool);
};
int8_t getHopsAway(const meshtastic_MeshPacket &,int8_t=-1);
#include "distance.inc"
#include "trace.inc"
struct Held {};
constexpr unsigned CHANNEL_UTILIZATION_EMA_DIVISOR=128;
struct AirTime {struct Windows {
    bool hasChannelUtilSample=false;
    float channelUtilAvg=0;
    void foldChannelUtil(float,unsigned,const Held &);
};};
#include "fold.inc"
}

int main() {
    unsigned traces=0, hops=0, ema=0;
    for(bool forward:{false,true}) for(bool last:{false,true})
    for(unsigned route=0;route<=7;++route) for(unsigned signals=0;signals<=route;++signals)
    for(unsigned taken=0;taken<=7;++taken) for(int snr:{-128,-32,-31,-8,0,7,31,32,127}) {
        meshtastic_RouteDiscovery a{},b{};
        auto path=mt::stock::TraceRouteModule::path(a,forward);
        for(unsigned i=0;i<route;++i)path.nodes[path.count++]=i+100;
        for(unsigned i=0;i<signals;++i)path.signals[path.signalCount++]=int8_t(i-4);
        b=a;
        meshtastic_MeshPacket p{};
        p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.has_bitfield=true;
        p.hop_start=7;p.hop_limit=7-taken;
        donor::TraceRouteModule stock;
        stock.insertUnknownHops(p,&b,forward);
        stock.appendMyIDandSNR(&b,float(snr),forward,last);
        mt::stock::TraceRouteModule::insertUnknownHops(path,taken);
        mt::stock::TraceRouteModule::appendMyIDandSNR(path,snr,42,last);
        assert(!memcmp(&a,&b,sizeof(a)));++traces;
    }
    for (auto role:{meshtastic_Config_DeviceConfig_Role_CLIENT,meshtastic_Config_DeviceConfig_Role_ROUTER,
                   meshtastic_Config_DeviceConfig_Role_ROUTER_LATE,meshtastic_Config_DeviceConfig_Role_CLIENT_BASE,
                   meshtastic_Config_DeviceConfig_Role_TRACKER, meshtastic_Config_DeviceConfig_Role_SENSOR}) {
        for(unsigned seed:{0u,721u}) {
            StockHop stock;
            mt::stock::HopScalingModule zt;
            config.device.role=role;stock.setHashSeed(seed);zt.seed(seed);
            bool observedReduction=false, observedRecovery=false;
            for(unsigned step=0;step<12*70;++step) {
                const unsigned phase=step/120, nodes=phase==0 ? 82 : phase==1 ? 500 : phase==4 ? 80 : 3;
                const float util=phase<2 || phase==4 ? 32.1f : phase==2 ? 19.6f : 5.0f;
                for(unsigned i=0;i<nodes;++i) {
                    const uint32_t id=0xabc00001u+i*17;
                    unsigned dist=phase==0 ? 0 : (i/20)%8;
                    stock.samplePacketForHistogram(id,dist);zt.samplePacketForHistogram(id,dist);
                }
                uint32_t now=30000+step*300000;
                assert(!zt.due(now-1)&&zt.due(now));
                stock.s_testChannelUtil=util;stock.runOnce();zt.runOnce(now,util,role);
                for (auto port:{meshtastic_PortNum_POSITION_APP,meshtastic_PortNum_TELEMETRY_APP,
                               meshtastic_PortNum_NODEINFO_APP,meshtastic_PortNum_NEIGHBORINFO_APP}) {
                    for (unsigned configured=0;configured<=7;++configured) {
                        assert(zt.limit(configured,UINT32_MAX,port)==std::min(configured,unsigned(stock.getLastRequiredHop())));
                        assert(zt.limit(configured,24,port)==configured);++hops;
                    }
                }
                assert(zt.limit(7,UINT32_MAX,meshtastic_PortNum_TEXT_MESSAGE_APP)==7);
                if(stock.getLastRequiredHop()<7)observedReduction=true;
                if(observedReduction&&stock.getLastRequiredHop()==7)observedRecovery=true;
            }
            assert(observedReduction&&observedRecovery);
        }
    }
    zt::ChannelWindow win;
    donor::AirTime::Windows expected;
    uint32_t buckets[6]{};
    uint64_t clock=0,last=0;
    for(unsigned k=0;k<290;++k) {
        clock+=k==144 ? UINT64_C(120000000000) : k%3==0 ? 35000 : k%7*1040;
        const auto now=clock/10000;
        if(k) {
            const uint64_t delta=now-last;
            const unsigned n=std::min<uint64_t>(6,delta);
            for(unsigned i=1;i<=n;++i) {
                unsigned sum=0;for(auto v:buckets)sum+=v;
                expected.foldChannelUtil(float(sum)/60000*100,1,{});
                buckets[(last+i)%6]=0;
            }
            expected.foldChannelUtil(0,unsigned(delta-n),{});
        }
        last=now;
        const unsigned val=(k*197)%2200;
        buckets[now%6]+=val;
        assert(win.record(clock,val));
        const auto magnitude=win.smoothedLoggedPercentAt(clock);
        assert(magnitude);
        if(expected.hasChannelUtilSample)assert(*magnitude==expected.channelUtilAvg);
        ++ema;
    }
    printf("PASS %u traceroute mutations; %u stock congestion/routine-hop comparisons; %u EMA samples\n",traces,hops,ema);
}
