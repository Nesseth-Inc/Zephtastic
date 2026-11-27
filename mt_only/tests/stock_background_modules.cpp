// SPDX-License-Identifier: GPL-3.0-only
// Pinned stock handlers execute with capture-only adapters alongside the production port.
#include "stock/modules/StatusMessageModule.h"
#include "PublicApplication.h"
#include "TxAdmission.h"
#include "stock/modules/RangeTestModule.h"
#include "stock/modules/NeighborInfoModule.h"
#include "stock/modules/MeshBeaconModule.h"
#include "stock/modules/TrafficManagementModule.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

namespace donor {
#define ARCH_NRF52 1
#define TRAFFIC_MANAGEMENT_CACHE_SIZE 16
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_TRACE(...) ((void)0)
#define TM_LOG_DEBUG(...) ((void)0)
constexpr uint32_t NODENUM_BROADCAST = UINT32_MAX, Self = 0x12345678, kRateTimeTickMs = 300000;
constexpr uint8_t default_traffic_mgmt_position_precision_bits = 19;
constexpr uint32_t MESH_BEACON_FLAG_LEGACY_SPLIT = 4;
constexpr unsigned MAX_LORA_PAYLOAD_LEN = 233, EVENT_CONTACT_FROM_PHONE = 1;
using NodeNum = uint32_t;
enum class ProcessMessage { CONTINUE };

struct {
    bool has_statusmessage = true, has_mesh_beacon = true;
    meshtastic_ModuleConfig_StatusMessageConfig statusmessage{};
    meshtastic_ModuleConfig_RangeTestConfig range_test{};
    meshtastic_ModuleConfig_MeshBeaconConfig mesh_beacon{};
    meshtastic_ModuleConfig_TrafficManagementConfig traffic_management{};
} moduleConfig;
struct { meshtastic_Config_LoRaConfig lora{}; meshtastic_Config_DeviceConfig device{}; } config;
struct {
    std::array<meshtastic_Channel, 8> slots{};
    int getNumChannels() const { return slots.size(); }
    const meshtastic_Channel &getByIndex(size_t i) const { return slots.at(i); }
} channels;
struct { void trigger(unsigned) {} } powerFSM;
struct Capture {
    meshtastic_MeshPacket pending{};
    std::vector<meshtastic_MeshPacket> sent;
    meshtastic_MeshPacket *allocDataPacket() {
        pending = meshtastic_MeshPacket_init_zero;
        pending.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        return &pending;
    }
    void sendToMesh(const meshtastic_MeshPacket *p) { sent.push_back(*p); }
} capture;
auto *service = &capture;
enum class LastByteResolution {None, Unique, Ambiguous};
struct ResolvedNode {LastByteResolution status; uint32_t num;};
struct {
    ResolvedNode resolution{LastByteResolution::None, 0};
    ResolvedNode resolveLastByte(uint8_t,bool direct) const {assert(direct);return resolution;}
    uint8_t getLastByteOfNodeNum(uint32_t id){return uint8_t(id)?uint8_t(id):255;}
    uint32_t from;
    meshtastic_StatusMessage status{};
    uint32_t getNodeNum() const { return Self; }
    void setNodeStatus(uint32_t source, const meshtastic_StatusMessage &value) { from = source; status = value; }
} db;
auto *nodeDB = &db;
bool isFromUs(const meshtastic_MeshPacket *p) { return p->from == Self; }
unsigned getHopsAway(const meshtastic_MeshPacket &p) { return p.hop_start - p.hop_limit; }
void stampRxTime(meshtastic_MeshPacket *) {}
size_t pb_encode_to_bytes(uint8_t *bytes, size_t capacity, const pb_msgdesc_t *type, const void *value) {
    auto stream = pb_ostream_from_buffer(bytes, capacity);
    return pb_encode(&stream, type, value) ? stream.bytes_written : 0;
}
bool pb_decode_from_bytes(const uint8_t *bytes, size_t length, const pb_msgdesc_t *type, void *value) {
    auto stream = pb_istream_from_buffer(bytes, length);
    return pb_decode(&stream, type, value);
}

struct AtakPluginModule { ProcessMessage handleReceived(const meshtastic_MeshPacket &); };
struct StatusMessageModule {
    meshtastic_MeshPacket *allocDataPacket() { return capture.allocDataPacket(); }
    int32_t runOnce();
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp);
};
uint32_t packetSequence;
struct RangeTestModuleRadio {
    unsigned writes = 0;
    meshtastic_MeshPacket *allocDataPacket() { return capture.allocDataPacket(); }
    bool appendFile(const meshtastic_MeshPacket &) { ++writes; return true; }
    bool removeFile() { return true; }
    void sendPayload(NodeNum dest=NODENUM_BROADCAST, bool wantReplies=false);
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp);
};
RangeTestModuleRadio *rangeTestModuleRadio;
struct RangeTestModule {
    bool firstTime=true;
    uint32_t started=0;
    int32_t runOnce();
    int32_t disable() { return -1; }
};
struct NeighborInfoModule {
    uint32_t observed = 0;
    void printNeighborInfo(const char *, const meshtastic_NeighborInfo *) {}
    void getOrCreateNeighbor(uint32_t, uint32_t last, uint32_t, float) { observed = last ? last : Self; }
    void updateNeighbors(const meshtastic_MeshPacket &, const meshtastic_NeighborInfo *);
    NodeNum lastTransmitter(const meshtastic_MeshPacket &, const meshtastic_NeighborInfo *);
    bool handleReceivedProtobuf(const meshtastic_MeshPacket &, meshtastic_NeighborInfo *);
};
constexpr unsigned MESH_BEACON_FLAG_LISTEN_ENABLED = 1, MESH_BEACON_FLAG_BROADCAST_ENABLED = 2;
constexpr uint32_t default_mesh_beacon_min_broadcast_interval_secs = 3600;
namespace Default {
uint32_t getConfiguredOrDefault(uint32_t value,uint32_t fallback){return value?value:fallback;}
uint32_t getConfiguredOrMinimumValue(uint32_t value,uint32_t minimum){return std::max(value,minimum);}
}
namespace Throttle {
uint32_t clockMs;
bool isWithinTimespanMs(uint32_t last,uint32_t interval){return clockMs-last<interval;}
}
uint32_t millis() { return Throttle::clockMs; }
struct AirTime {
#include "stock_channel_limits.inc"
    struct Held { explicit Held(AirTime *) {} };
    struct { float percent; float channelUtilizationPercent(const Held &) { return percent; } } w;
    bool allowed;
    bool isTxAllowedAirUtil(){return allowed;}
    bool isTxAllowedChannelUtil(bool polite);
} beaconAir, *airTime=&beaconAir;
struct BeaconHistory {
    uint32_t last=0, marked=0;
    uint32_t getLastSentToMeshMillis(unsigned){return last;}
    void setLastSentToMesh(unsigned){++marked;}
} beaconHistory, *transmitHistory=&beaconHistory;
struct MeshBeaconListenerModule {bool wantPacket(const meshtastic_MeshPacket *);};
struct MeshBeaconBroadcastModule {
    bool payloadCacheDirty = true;
    uint8_t payloadCache[meshtastic_MeshBeacon_size]{};
    pb_size_t payloadCacheSize = 0;
    meshtastic_MeshPacket *allocDataPacket() { return capture.allocDataPacket(); }
    void rebuildCache();
    void sendBeacon();
    int32_t runOnce();
    void setTargetRadioSettings(meshtastic_MeshPacket *, meshtastic_Config_LoRaConfig_ModemPreset, uint16_t, bool,
                                meshtastic_Config_LoRaConfig_RegionCode, bool, const meshtastic_ChannelSettings *) {}
    void sendBeaconPacket(const meshtastic_MeshPacket *p, meshtastic_Config_LoRaConfig_ModemPreset, bool,
                          const meshtastic_ChannelSettings *) { capture.sendToMesh(p); }
};
namespace concurrency { struct LockGuard { explicit LockGuard(int *) {} }; }
struct TrafficManagementModule {
    struct UnifiedCacheEntry {
        uint8_t rateCount=0, rateTime=0, unknownCount=0, unknownTime=0;
        uint8_t getRateCount() const { return rateCount; }
        uint8_t getRateTime() const { return rateTime; }
        uint8_t getUnknownCount() const { return unknownCount; }
        uint8_t getUnknownTime() const { return unknownTime; }
        void setRateCount(uint8_t x) { rateCount=x; }
        void setRateTime(uint8_t x) { rateTime=x; }
        void setUnknownCount(uint8_t x) { unknownCount=x; }
        void setUnknownTime(uint8_t x) { unknownTime=x; }
    } entry;
    int cacheLock = 0;
    uint8_t tick = 0;
    UnifiedCacheEntry *findOrCreateEntry(uint32_t, bool *fresh) { *fresh=false; return &entry; }
    uint8_t currentRateTick() const { return tick; }
    uint8_t currentUnknownTick() const { return tick; }
    uint8_t computePositionFingerprint(int32_t, int32_t, uint8_t);
    bool isRateLimited(NodeNum, uint32_t);
    bool shouldDropUnknown(const meshtastic_MeshPacket *, uint32_t);
};
#include "stock_handlers.inc"
} // namespace donor

static void compareAtak() {
    donor::AtakPluginModule stock;
    for (unsigned length : {0u, 1u, 5u, 127u, 128u, 224u}) {
        meshtastic_MeshPacket packet{};
        packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
        auto &data=packet.decoded;
        data.portnum=meshtastic_PortNum_ATAK_PLUGIN_V2;
        data.payload.size=length;
        for (unsigned i=0; i<length; ++i) data.payload.bytes[i]=uint8_t(i*71);
        // The receiver leaves binary data to the application.
        assert(stock.handleReceived(packet)==donor::ProcessMessage::CONTINUE);
        uint8_t bytes[239];
        size_t size=0;
        assert(mt::PublicApplication::encode(data.portnum,data.payload.bytes,length,0,0,bytes,sizeof(bytes),size));
        meshtastic_Data ported{};
        auto in=pb_istream_from_buffer(bytes,size);
        assert(pb_decode(&in,meshtastic_Data_fields,&ported) && !in.bytes_left);
        assert(ported.portnum==data.portnum && ported.payload.size==length &&
               !memcmp(ported.payload.bytes,data.payload.bytes,length));
    }
    puts("PASS ATAK opaque application payloads including embedded NULs");
}

static void compareStatus() {
    donor::StatusMessageModule stock;
    for (size_t length : {0, 1, 17, 79}) {
        auto &text = donor::moduleConfig.statusmessage.node_status;
        memset(text, 'q', length);
        text[length] = 0;
        donor::capture.sent.clear();
        assert(stock.runOnce() == mt::stock::StatusMessageModule::IntervalMs);
        bool called = false;
        auto send = [&](const meshtastic_Data &out) {
            const auto &packet = donor::capture.sent.at(0);
            assert(packet.to == UINT32_MAX && packet.channel == 0);
            assert(!packet.decoded.want_response && !out.want_response && out.has_bitfield);
            assert(out.portnum == meshtastic_PortNum_NODE_STATUS_APP && out.payload.size == packet.decoded.payload.size);
            assert(!memcmp(out.payload.bytes, packet.decoded.payload.bytes, out.payload.size));
            auto rx = packet;
            rx.from = 53;
            stock.handleReceived(rx);
            auto store = [&](uint32_t from, const mt::PeerStatus &s) {
                return donor::db.from == from && !strcmp(s.text(), donor::db.status.status);
            };
            assert(mt::stock::StatusMessageModule::handleReceived({rx.from, out.payload.bytes, out.payload.size},
                                                                  mt::stock::StatusMessageReceiver(store)));
            called = true;
        };
        assert(mt::stock::StatusMessageModule::runOnce(text, mt::stock::StatusMessageSender(send)) ==
               mt::stock::StatusMessageModule::IntervalMs);
        assert(called == (length != 0) && donor::capture.sent.size() == (length != 0));
    }
    puts("PASS status schedule, TX bytes and RX cache value");
}

static void compareRange() {
    donor::RangeTestModuleRadio stock;
    for (uint32_t sequence : {1u, 255u, 10000u, UINT32_MAX}) {
        donor::packetSequence = sequence - 1;
        donor::capture.sent.clear();
        stock.sendPayload(UINT32_MAX, false);
        assert(mt::stock::RangeTestModuleRadio::sendPayload(sequence, [&](const uint8_t *p, size_t n) {
            auto &packet = donor::capture.sent.at(0);
            return packet.to == UINT32_MAX && !packet.hop_limit && !packet.want_ack &&
                   packet.decoded.payload.size == n && !memcmp(packet.decoded.payload.bytes,p,n);
        }));
    }
    for (bool enabled : {false,true}) for (bool save : {false,true}) for (bool fromUs : {false,true}) {
        donor::moduleConfig.range_test.enabled = enabled;
        donor::moduleConfig.range_test.save = save;
        meshtastic_MeshPacket mp{}; mp.from = fromUs ? donor::Self : 33;
        const auto before = stock.writes;
        stock.handleReceived(mp);
        unsigned writes = 0;
        mt::stock::RangeTestModuleRadio::handleReceived(enabled,save,fromUs,[&] { ++writes; });
        assert(writes == stock.writes - before);
    }
    donor::moduleConfig.range_test={};
    donor::moduleConfig.range_test.enabled=true;
    donor::moduleConfig.range_test.sender=1;
    donor::packetSequence=0;
    donor::Throttle::clockMs=0;
    donor::RangeTestModule scheduled;
    mt::stock::RangeTestModule actual;
    actual.start(0);
    assert(scheduled.runOnce()==int32_t(actual.FirstDelayMs));
    donor::Throttle::clockMs=actual.FirstDelayMs;
    for (float percent : {0.f,24.99f,25.f,39.f,100.f,0.f}) {
        donor::beaconAir.w.percent=percent;
        donor::capture.sent.clear();
        assert(scheduled.runOnce()==1000);
        const bool channelAllowed=mt::TxAdmission::channel(true,percent,true)==mt::TxAdmission::Decision::Allow;
        bool sent=false;
        assert(actual.runOnce(donor::millis(),channelAllowed,[&](const uint8_t *p,size_t n) {
            const auto &reference=donor::capture.sent.at(0).decoded.payload;
            assert(n==reference.size && !memcmp(p,reference.bytes,n));
            sent=true;
            return true;
        }));
        assert(donor::capture.sent.size()==unsigned(sent));
        donor::Throttle::clockMs+=1000;
    }
    delete donor::rangeTestModuleRadio;
    donor::rangeTestModuleRadio=nullptr;
    puts("PASS range scheduler waits for a polite channel and preserves sequence across skipped heartbeats");
    puts("PASS range heartbeat bytes, zero hops and RX log eligibility");
}

static void compareNeighbors() {
    donor::NeighborInfoModule stock;
    for (unsigned hops : {0,1}) for (unsigned shape : {0,1,2,3}) {
        meshtastic_NeighborInfo info{};
        info.neighbors_count = shape == 1 ? 1 : 0;
        info.last_sent_by_id = shape == 3 ? 0 : 44;
        info.node_broadcast_interval_secs = 300;
        auto *np = shape ? &info : nullptr;
        meshtastic_MeshPacket mp{}; mp.from = 33; mp.hop_start = hops; mp.rx_snr = 2.0f;
        mp.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        stock.observed = 0;
        stock.handleReceivedProtobuf(mp,np);
        mt::NeighborObservations table;
        mt::NodeCache cache;
        if(np) np->last_sent_by_id=mt::stock::NeighborInfoModule::lastTransmitter(cache,donor::Self,mp.from,hops,mp.relay_node,1000,info);
        mt::stock::NeighborInfoModule::handleReceivedProtobuf(table,33,donor::Self,hops,1000,true,2.0f,300,np);
        meshtastic_NeighborInfo out{};
        bool observed = table.snapshot(cache,donor::Self,300,1000,out);
        assert(observed == (stock.observed != 0 && stock.observed != donor::Self));
        if (observed) assert(out.neighbors_count == 1 && out.neighbors[0].node_id == stock.observed);
    }
    mt::NodeCache peers;
    auto add=[&](uint32_t id,uint8_t hops,uint32_t last) {
        mt::NodeCache::Entry e{}; e.identity(id,81,0);
        assert(peers.accept(e,donor::Self,0,hops)>=0 && peers.observeReceivedPacket(id,last,false));
    };
    constexpr uint32_t relay=0x100100,other=0x2001ff;
    add(relay,0,500);
    meshtastic_NeighborInfo legacy{};legacy.node_id=33;legacy.last_sent_by_id=33;
    auto resolved=[&](int hops,uint8_t byte,uint32_t clock=1000) {
        return mt::stock::NeighborInfoModule::lastTransmitter(peers,donor::Self,33,hops,byte,clock,legacy);
    };
    assert(resolved(0,255)==33 && resolved(-1,255)==33 && resolved(1,255)==relay);
    assert(!resolved(1,0) && !resolved(1,255,7700));
    add(other,0,500);assert(!resolved(1,255));
    legacy.last_sent_by_id=other;assert(resolved(1,255)==other && !resolved(1,14));
    puts("PASS neighbor direct, legacy, fresh unique, ambiguous, stale and dummy RX observations");
}

static void compareBeacons() {
    using Broadcast = mt::stock::MeshBeaconBroadcastModule;
    using Listener = mt::stock::MeshBeaconListenerModule;
    donor::MeshBeaconListenerModule listener;
    for(unsigned flags=0;flags<8;++flags) for(bool hidden:{false,true})
    for(bool air:{false,true}) for(bool recent:{false,true}) {
        auto &cfg=donor::moduleConfig.mesh_beacon;
        cfg={};cfg.flags=flags;strcpy(cfg.broadcast_message,"test");
        donor::config.device.role=hidden?meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN:meshtastic_Config_DeviceConfig_Role_ROUTER;
        donor::beaconAir.allowed=air;
        donor::beaconHistory={recent?100u:0u,0};
        donor::Throttle::clockMs=200;
        donor::MeshBeaconBroadcastModule broadcaster;
        donor::capture.sent.clear();
        broadcaster.runOnce();
        assert(Broadcast::broadcastDue(cfg,donor::config.device.role,air,[&]{return !recent;})==
               (donor::beaconHistory.marked==1));
        for(auto port:{meshtastic_PortNum_MESH_BEACON_APP,meshtastic_PortNum_TEXT_MESSAGE_APP}) {
            meshtastic_MeshPacket p{};p.decoded.portnum=port;
            assert(Listener::wantPacket(flags & 1,port)==listener.wantPacket(&p));
        }
    }

    for (bool text : {false,true}) for (unsigned offer=0; offer<8; ++offer) for (bool split : {false,true}) {
        auto &cfg = donor::moduleConfig.mesh_beacon;
        cfg = {};
        if (text) strcpy(cfg.broadcast_message,"hello");
        cfg.has_broadcast_offer_channel = offer & 1;
        if (cfg.has_broadcast_offer_channel) strcpy(cfg.broadcast_offer_channel.name,"Test");
        cfg.has_broadcast_offer_preset = offer & 2;
        if (cfg.has_broadcast_offer_preset) cfg.broadcast_offer_preset = meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST;
        if (offer & 4) cfg.broadcast_offer_region = meshtastic_Config_LoRaConfig_RegionCode_US;
        if (split) cfg.flags = 4;
        donor::capture.sent.clear();
        donor::MeshBeaconBroadcastModule stock; stock.sendBeacon();
        mt::stock::BeaconPayloadBatch batch{};
        bool valid = mt::stock::MeshBeaconBroadcastModule::sendBeacon(cfg,batch,1);
        assert(valid == !donor::capture.sent.empty() && batch.count == donor::capture.sent.size());
        for (unsigned i=0; i<batch.count; ++i) {
            const auto &a = batch.packets[i]; const auto &b = donor::capture.sent[i];
            assert(a.portnum == b.decoded.portnum && !b.hop_limit && !b.want_ack && b.to == UINT32_MAX);
            assert(a.payload.size == b.decoded.payload.size && !memcmp(a.payload.bytes,b.decoded.payload.bytes,a.payload.size));
        }
    }
    puts("PASS 32 beacon text, offer and legacy-split combinations with wire payloads");
}

static void compareTraffic() {
    using Port = mt::stock::TrafficManagementModule;
    for (uint32_t threshold : {0u,1u,2u,60u,61u,UINT32_MAX}) for (uint32_t secs : {0u,1u,300u,4511u,UINT32_MAX}) {
        donor::TrafficManagementModule stock;
        auto &cfg = donor::moduleConfig.traffic_management;
        cfg.rate_limit_window_secs = secs; cfg.rate_limit_max_packets = cfg.unknown_packet_threshold = threshold;
        uint8_t rcount=0, rtime=0, ucount=0, utime=0;
        meshtastic_MeshPacket packet{}; packet.from = 33;
        for (unsigned step=0; step<150; ++step) {
            stock.tick = static_cast<uint8_t>((step < 76 ? 14 : step/6) & 15);
            assert(Port::isRateLimited(rcount,rtime,stock.tick,secs,threshold) == stock.isRateLimited(33,0));
            assert(Port::shouldDropUnknown(ucount,utime,stock.tick,threshold) == stock.shouldDropUnknown(&packet,0));
        }
        for (uint8_t p : {0,1,4,19,32,33,255})
            assert(Port::computePositionFingerprint(INT32_MIN,INT32_MAX,p) == stock.computePositionFingerprint(INT32_MIN,INT32_MAX,p));
    }
    puts("PASS traffic rate and unknown thresholds, saturation, wrap, and position fingerprints");
}

int main() {
    compareAtak(); compareStatus(); compareRange(); compareNeighbors(); compareBeacons(); compareTraffic();
}
