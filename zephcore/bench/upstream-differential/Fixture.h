#pragma once
#include "FixtureTypes.h"
#include "upstream-packet-history.inc"
#include "upstream-structs.inc"

struct FixtureAck { meshtastic_Routing_Error error; NodeNum to; PacketId request; uint8_t hops; bool wanted; };
inline std::vector<meshtastic_MeshPacket> fixtureQueue;
inline std::vector<FixtureAck> fixtureAcks;
inline ErrorCode fixtureSendResult = ERRNO_OK;
inline RadioInterface fixtureRadio;
inline unsigned rxDupe = 0, txRelayCanceled = 0;

class Router {
public:
    RadioInterface *iface = &fixtureRadio;
    virtual ~Router() = default;
    virtual ErrorCode send(meshtastic_MeshPacket *p) {
        if (p->to == NODENUM_BROADCAST_NO_LORA || fixtureSendResult == ERRNO_SHOULD_RELEASE) return ERRNO_SHOULD_RELEASE;
        // Exact upstream Router::send routing-header statements, mechanically extracted.
#include "upstream-send-header.inc"
        if (fixtureSendResult == ERRNO_OK) fixtureQueue.push_back(*p);
        packetPool.release(p);
        return fixtureSendResult;
    }
    virtual bool shouldFilterReceived(const meshtastic_MeshPacket *) { return false; }
    virtual void sniffReceived(const meshtastic_MeshPacket *, const meshtastic_Routing *) {}
    virtual bool perhapsRebroadcast(const meshtastic_MeshPacket *) { return false; }
    virtual bool relayOpaquePacket(const meshtastic_MeshPacket *) { return false; }
    bool shouldDecrementHopLimit(const meshtastic_MeshPacket *);
    bool findInTxQueue(NodeNum from, PacketId id) {
        return std::any_of(fixtureQueue.begin(), fixtureQueue.end(), [=](const auto &p) { return p.from == from && p.id == id; });
    }
    bool cancelSending(NodeNum from, PacketId id) {
        const size_t n = fixtureQueue.size();
        fixtureQueue.erase(std::remove_if(fixtureQueue.begin(), fixtureQueue.end(), [=](const auto &p) {
            return p.from == from && p.id == id;
        }), fixtureQueue.end());
        return fixtureQueue.size() != n;
    }
    void setReceivedMessage() {}
    void sendAckNak(meshtastic_Routing_Error error, NodeNum to, PacketId id, ChannelIndex, uint8_t hops = 0,
                    bool wanted = false, const meshtastic_MeshPacket * = nullptr) {
        fixtureAcks.push_back({error, to, id, hops, wanted});
    }
};
inline bool RadioInterface::removePendingTXPacket(NodeNum from, PacketId id, uint32_t less) {
    const size_t n = fixtureQueue.size();
    fixtureQueue.erase(std::remove_if(fixtureQueue.begin(), fixtureQueue.end(), [=](const auto &p) {
        return p.from == from && p.id == id && p.hop_limit < less;
    }), fixtureQueue.end());
    return fixtureQueue.size() != n;
}
class FloodingRouter : public Router, public PacketHistory {
public:
    FloodingRouter() = default;
    ErrorCode send(meshtastic_MeshPacket *) override;
    bool shouldFilterReceived(const meshtastic_MeshPacket *) override;
    bool perhapsHandleUpgradedPacket(const meshtastic_MeshPacket *);
    bool reprocessPacket(const meshtastic_MeshPacket *);
    bool roleAllowsCancelingDupe(const meshtastic_MeshPacket *);
    void perhapsCancelDupe(const meshtastic_MeshPacket *);
    bool isRebroadcaster();
    void sniffReceived(const meshtastic_MeshPacket *, const meshtastic_Routing *) override;
};
class NextHopRouter : public FloodingRouter {
public:
    static constexpr uint8_t NUM_INTERMEDIATE_RETX = 3, NUM_RELIABLE_RETX = 3, NUM_RELIABLE_UNICAST_ATTEMPTS = 5;
    static constexpr uint8_t ROUTE_HEALTH_MAX = 32, ROUTE_FAILURE_THRESHOLD = 3, OPAQUE_SEEN_MAX = 32;
    static constexpr uint32_t ROUTE_TTL_MSEC = 30UL * 60 * 1000;
    std::map<GlobalPacketId, PendingPacket> pending;
    RouteHealth routeHealth[ROUTE_HEALTH_MAX]{};
    struct OpaqueSeen { NodeNum sender = 0; PacketId id = 0; };
    OpaqueSeen opaqueSeen[OPAQUE_SEEN_MAX]{};
    uint8_t opaqueSeenNext = 0;
    NextHopRouter() = default;
    ~NextHopRouter() override { while (!pending.empty()) stopRetransmission(pending.begin()->first); }
    bool relayOpaquePacket(const meshtastic_MeshPacket *) override;
    bool opaqueWasSeenRecently(NodeNum, PacketId);
    ErrorCode send(meshtastic_MeshPacket *) override;
    ErrorCode sendWithNextHop(meshtastic_MeshPacket *, bool);
    bool shouldFilterReceived(const meshtastic_MeshPacket *) override;
    void sniffReceived(const meshtastic_MeshPacket *, const meshtastic_Routing *) override;
    bool perhapsRebroadcast(const meshtastic_MeshPacket *) override;
    std::optional<uint8_t> getNextHop(NodeNum, uint8_t);
    PendingPacket *findPendingPacket(GlobalPacketId);
    PendingPacket *findPendingPacket(NodeNum n, PacketId id) { return findPendingPacket(GlobalPacketId(n, id)); }
    bool stopRetransmission(NodeNum, PacketId);
    bool stopRetransmission(GlobalPacketId);
    bool roleAllowsCancelingFromTxQueue(const meshtastic_MeshPacket *);
    PendingPacket *startRetransmission(meshtastic_MeshPacket *, uint8_t = NUM_INTERMEDIATE_RETX);
    int32_t doRetransmissions();
    void setNextTx(PendingPacket *);
    RouteHealth *findRouteHealth(NodeNum);
    RouteHealth *getOrAllocRouteHealth(NodeNum, uint32_t);
    void noteRouteLearned(NodeNum, uint8_t, uint32_t);
    void noteRouteSuccess(NodeNum, uint32_t);
    void noteRouteFailure(NodeNum);
    bool isRouteStale(const RouteHealth &, uint32_t) const;
    void clearRouteHealth(NodeNum);
};
class ReliableRouter : public NextHopRouter {
public:
    void perhapsGenerateImplicitAckForOwnOverheard(const meshtastic_MeshPacket *);
    bool shouldSuccessAckWithWantAck(const meshtastic_MeshPacket *);
    bool shouldFilterReceived(const meshtastic_MeshPacket *) override;
};
inline void fixtureReset() {
    fixtureQueue.clear(); fixtureAcks.clear(); fixtureDB = FixtureNodeDB{};
    config = FixtureConfig{}; owner.is_licensed = false;
    assert(packetPool.active() == 0); packetPool.fail = false;
    fixtureSendResult = ERRNO_OK; fixtureAirTime.utilization = 0;
    fixtureRegion.wideLora = false; fixtureCanonicalDataSize = 16;
    fixtureRandom = 0;
    fixtureDecodeOverride=false;fixtureDecodedData={};fixtureDecodeState=DecodeState::DECODE_SUCCESS;
    fixtureCrypto=FixtureCrypto{};fixtureCryptLock=concurrency::Lock{};
    fixtureSizingSuccess=true;fixtureInnerDecodeSuccess=false;fixtureDataSizeTracksPayload=false;fixtureCanonicalInnerSize=8;
    fixtureBootstrapUser={};fixtureBootstrapCrc=0;fixtureAuthReset();
    fixtureRadio = RadioInterface{}; Time::clock = 1000; rxDupe = txRelayCanceled = 0;
}
