// SPDX-License-Identifier: GPL-3.0-only
#include "stock/modules/DirectResponseThrottle.h"
#include "stock/modules/NeighborInfoModule.h"
#include <cassert>
#include <iterator>
#include <vector>

namespace mesh_donor {
using NodeNum=uint32_t;
#define LOG_DEBUG(...) ((void)0)
#define LOG_WARN(...) ((void)0)
constexpr uint32_t MAX_NUM_NEIGHBORS=10, NODENUM_BROADCAST=UINT32_MAX, NODENUM_BROADCAST_NO_LORA=1;
constexpr uint32_t default_telemetry_broadcast_interval_secs=3600, default_neighbor_info_broadcast_secs=21600;
struct { struct {bool transmit_over_lora; uint32_t update_interval;} neighbor_info; } moduleConfig;
struct { bool def; bool isDefaultChannel(unsigned) { return def; } unsigned getPrimaryIndex(){return 0;} } channels;
struct RadioInterface { static inline bool uses_default_frequency_slot=true; };
struct Air {
    bool channelAllowed, airAllowed;
    bool isTxAllowedChannelUtil(bool polite) { assert(polite); return channelAllowed; }
    bool isTxAllowedAirUtil() { return airAllowed; }
} air, *airTime=&air;
struct Default {
    static uint32_t getConfiguredOrDefault(uint32_t input,uint32_t fallback) { return input ? input : fallback; }
    static uint32_t getConfiguredOrDefaultMs(uint32_t input,uint32_t fallback) {
        auto value=getConfiguredOrDefault(input,fallback);
        return value>uint32_t(INT32_MAX)/1000 ? INT32_MAX : 1000*value;
    }
};
struct { NodeNum getNodeNum() { return 2; } } db, *nodeDB=&db;
uint32_t timeSeconds;
uint32_t getTime() { return timeSeconds; }
struct NeighborInfoModule {
    std::vector<meshtastic_Neighbor> neighbors;
    uint32_t sentTo=0;
    void printNodeDBNeighbors() {}
    void sendNeighborInfo(NodeNum dest,bool wantReplies) { assert(!wantReplies); sentTo=dest; }
    meshtastic_Neighbor *getOrCreateNeighbor(NodeNum,NodeNum,uint32_t,float);
    void cleanUpNeighbors();
    uint32_t collectNeighborInfo(meshtastic_NeighborInfo *);
    int32_t runOnce();
};
namespace concurrency { struct LockGuard { explicit LockGuard(int *) {} }; }
struct Time { static uint32_t skipZero(uint32_t t) { return t?t:1; } };
struct TrafficManagementModule {
    static constexpr uint32_t kDirectResponseTrackedNodes=8,kDirectResponsePerTargetMs=60000,
                              kDirectResponsePerRequesterMs=60000,kDirectResponseGlobalMs=1000;
    struct DirectResponseThrottleEntry {NodeNum key=0; uint32_t lastReplyMs=0;};
    DirectResponseThrottleEntry directRequesterSeen[8]{},directTargetSeen[8]{};
    uint32_t lastDirectResponseMs=0;
    int cacheLock=0;
    static DirectResponseThrottleEntry *directResponseSlot(DirectResponseThrottleEntry *,NodeNum,uint32_t,uint32_t);
    bool directResponseAllowed(NodeNum,NodeNum,uint32_t);
};
#include "mesh-oracle.inc"
}

static void responseThrottle() {
    mesh_donor::TrafficManagementModule expected;
    mt::stock::DirectResponseThrottle actual;
    auto compare=[&](uint32_t req,uint32_t target,uint32_t time) {
        mt::stock::DirectResponseThrottle::Reservation reservation;
        const bool admitted=actual.directResponseAllowed(req,target,time,reservation);
        assert(expected.directResponseAllowed(req,target,time)==admitted);
        if (admitted) actual.accepted(reservation,req,target,time);
        return admitted;
    };
    assert(compare(1,2,1));
    assert(!compare(3,4,1000));
    assert(compare(3,4,1001));
    assert(!compare(1,6,60000));
    assert(!compare(7,4,60001)); // target blocks; requester budget stays available.
    assert(compare(7,2,60001));
    for (unsigned i=1;i<=30;++i) compare(100+i%13,200+i%11,60001+i*1001);
    for (unsigned i=1;i<=60;++i) compare(100+i%13,200+i%17,92000+i*1000);

    // The ZT queue can refuse. It only consumes rate budget on acceptance.
    mt::stock::DirectResponseThrottle separate;
    mt::stock::DirectResponseThrottle::Reservation slot;
    assert(separate.directResponseAllowed(1,2,0,slot));
    assert(separate.directResponseAllowed(1,2,0,slot));
    separate.accepted(slot,1,2,0);
    assert(!separate.directResponseAllowed(3,4,0,slot));
    assert(separate.directResponseAllowed(3,4,UINT64_C(0x100000000),slot));
    separate.accepted(slot,3,4,UINT64_C(0x100000000));
    assert(!separate.directResponseAllowed(5,6,10000,slot));
}

static void neighborTable() {
    mesh_donor::NeighborInfoModule expected;
    mt::stock::NeighborTable actual;
    mesh_donor::moduleConfig.neighbor_info.update_interval=5;
    auto observe=[&](uint32_t sender,uint32_t id,uint32_t interval,float snr,uint32_t sec) {
        mesh_donor::timeSeconds=sec;
        expected.getOrCreateNeighbor(sender,id,interval,snr);
        actual.getOrCreateNeighbor(sender,id,interval,5,snr,uint64_t(sec)*1000,sender==id);
    };
    auto compare=[&](uint32_t sec) {
        meshtastic_NeighborInfo got{},want{};
        mesh_donor::timeSeconds=sec;
        expected.collectNeighborInfo(&want);
        actual.collectNeighborInfo(got,2,5,uint64_t(sec)*1000,[](uint32_t){return false;});
        assert(want.node_id==got.node_id && want.last_sent_by_id==got.last_sent_by_id && want.neighbors_count==got.neighbors_count);
        assert(want.node_broadcast_interval_secs==got.node_broadcast_interval_secs);
        for (size_t i=0;i<got.neighbors_count;++i) {
            const auto &a=got.neighbors[i],&b=want.neighbors[i];
            assert(a.node_id==b.node_id && a.snr==b.snr && !a.last_rx_time && !a.node_broadcast_interval_secs);
        }
    };
    observe(9,9,1,-4,1);
    observe(10,10,0,0,1);
    observe(99,9,900,2.75f,2); // a relay cannot replace the original sender's interval.
    compare(4); compare(5); compare(11); compare(12);
    for (unsigned i=20;i<40;++i) observe(i,i,8,float(i-20)/2,15);
    observe(30,30,3,4,16);
    compare(16); compare(22); compare(23); compare(31); compare(32);
}

static void neighborScheduling() {
    for (unsigned state=0;state<16;++state) for (uint32_t seconds : {0u,1u,590u,UINT32_MAX}) {
        mesh_donor::NeighborInfoModule expected;
        mesh_donor::moduleConfig.neighbor_info={bool(state&1),seconds};
        mesh_donor::channels.def=!(state&2);
        mesh_donor::air.channelAllowed=state&4;
        mesh_donor::air.airAllowed=state&8;
        bool overLoRa=false,called=false;
        auto next=mt::stock::NeighborInfoModule::runOnce(seconds,state&1,state&2,
            [&](){return bool(state&4);},[&](){return bool(state&8);},
            [&](bool rf){assert(!called);called=true;overLoRa=rf;});
        assert(called && next==uint32_t(expected.runOnce()));
        assert(overLoRa==(expected.sentTo==mesh_donor::NODENUM_BROADCAST));
    }
}
void compareMeshPolicies() { responseThrottle(); neighborTable(); neighborScheduling(); }
