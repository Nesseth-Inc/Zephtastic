#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <vector>
#include "mesh.pb.h"
#include "upstream-auth-declarations.inc"

// Fixture boundaries only: no real interface, clock, NodeDB persistence or key material.
using NodeNum = uint32_t;
using PacketId = uint32_t;
using ErrorCode = int;
using ChannelIndex = uint8_t;
using std::min;
using std::max;
constexpr NodeNum NODENUM_BROADCAST = 0xffffffff;
constexpr NodeNum NODENUM_BROADCAST_NO_LORA = 1;
constexpr uint8_t NO_NEXT_HOP_PREFERENCE = 0;
constexpr unsigned PACKETHISTORY_MAX = 64;
constexpr unsigned MESHTASTIC_HEADER_LENGTH = 16;
constexpr unsigned MAX_LORA_PAYLOAD_LEN = 255;
constexpr unsigned XEDDSA_SIGNATURE_SIZE = 64, XEDDSA_SIGNATURE_FIELD_BYTES = 66;
constexpr uint32_t NODEINFO_BITFIELD_HAS_XEDDSA_SIGNED_MASK = 1u << 9;
constexpr ErrorCode ERRNO_OK = 0, ERRNO_SHOULD_RELEASE = 100;
#define MESHTASTIC_EXCLUDE_PKT_HISTORY_HASH 0
#define MESHTASTIC_EXCLUDE_TRACEROUTE 1
#define HAS_TRAFFIC_MANAGEMENT 0
#define USERPREFS_EVENT_MODE 0
#define USERPREFS_BLOCK_POSITION_ON_EVENT_CHANNEL 0
#define NEXTHOP_EARLY_FLOOD_ON_UNVERIFIED 0
#define MESHTASTIC_EXCLUDE_PKI 0
#define MESHTASTIC_EXCLUDE_XEDDSA 0
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_TRACE(...) ((void)0)
#define DEBUG_HEAP_BEFORE ((void)0)
#define DEBUG_HEAP_AFTER(...) ((void)0)
template<class T, class... U> bool fixtureOneOf(T t, U... u) { return ((t == u) || ...); }
#define IS_ONE_OF(...) fixtureOneOf(__VA_ARGS__)
struct FixtureConfig { struct Device {
    meshtastic_Config_DeviceConfig_Role role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    meshtastic_Config_DeviceConfig_RebroadcastMode rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_ALL;
} device;
struct Security {
    meshtastic_Config_SecurityConfig_PacketSignaturePolicy packet_signature_policy =
        meshtastic_Config_SecurityConfig_PacketSignaturePolicy_PACKET_SIGNATURE_POLICY_BALANCED;
} security;
};
inline FixtureConfig config;
struct FixtureOwner { bool is_licensed = false; };
inline FixtureOwner owner;
namespace Time {
inline uint32_t clock = 1000;
inline uint32_t getMillis() { return clock; }
inline uint32_t stampMillis() { return clock; }
inline uint32_t skipZero(uint32_t t) { return t ? t : 1; }
}
inline uint32_t millis() { return Time::getMillis(); }
namespace Throttle { inline bool deadlinePassedAt(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
} }
namespace memaudit { inline void set(const char *, size_t) {} }
inline uint32_t nextPowerOf2(uint32_t n) { uint32_t r = 1; while (r < n) r <<= 1; return r; }
enum class LastByteResolution { None, Unique, Ambiguous };
struct ResolvedNode { LastByteResolution status = LastByteResolution::None; NodeNum num = 0; };
struct meshtastic_NodeInfoLite {
    NodeNum num = 0;
    uint8_t next_hop = 0;
    bool known = false, favorite = false, direct = false;
    meshtastic_Config_DeviceConfig_Role role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    meshtastic_NodeInfoLite_public_key_t public_key{};
    bool authoritative = false;
    uint32_t bitfield = 0;
};
inline bool nodeInfoLiteHasUser(const meshtastic_NodeInfoLite *n) { return n && n->known; }
inline bool nodeInfoLiteIsFavorite(const meshtastic_NodeInfoLite *n) { return n && n->favorite; }
inline void nodeInfoLiteSetBit(meshtastic_NodeInfoLite *n,uint32_t mask,bool value) {
    if(value)n->bitfield|=mask;else n->bitfield&=~mask;
}
struct FixtureNodeDB {
    NodeNum identity = 0x1234007c;
    std::map<NodeNum, meshtastic_NodeInfoLite> nodes;
    std::map<NodeNum, meshtastic_NodeInfoLite_public_key_t> authoritativeWarmKeys;
    bool failCreate = false;
    NodeNum getNodeNum() const { return identity; }
    uint8_t getLastByteOfNodeNum(NodeNum n) const { return static_cast<uint8_t>(n); }
    meshtastic_NodeInfoLite *getMeshNode(NodeNum n) { auto i = nodes.find(n); return i == nodes.end() ? nullptr : &i->second; }
    ResolvedNode resolveLastByte(uint8_t b, bool directOnly) {
        ResolvedNode r;
        for (auto &entry : nodes) if (static_cast<uint8_t>(entry.first) == b && entry.second.known && (!directOnly || entry.second.direct)) {
            if (r.status == LastByteResolution::Unique) return {LastByteResolution::Ambiguous, 0};
            r = {LastByteResolution::Unique, entry.first};
        }
        return r;
    }
    bool resolveUniqueLastByte(uint8_t b, bool directOnly, NodeNum *out = nullptr) {
        auto r = resolveLastByte(b, directOnly); if (out) *out = r.num; return r.status == LastByteResolution::Unique;
    }
    bool isFromOrToFavoritedNode(const meshtastic_MeshPacket &p) {
        return nodeInfoLiteIsFavorite(getMeshNode(p.from)) || nodeInfoLiteIsFavorite(getMeshNode(p.to));
    }
    void updateFrom(const meshtastic_MeshPacket &) {}
    void add(NodeNum n,bool direct=true) {
        auto &node=nodes[n];node=meshtastic_NodeInfoLite{};
        node.num=n;node.known=true;node.direct=direct;
    }
    meshtastic_NodeInfoLite *getOrCreateMeshNode(NodeNum n) {
        if(auto *node=getMeshNode(n))return node;
        if(failCreate)return nullptr;
        add(n);return getMeshNode(n);
    }
    bool copyPublicKeyAuthoritative(NodeNum n,meshtastic_NodeInfoLite_public_key_t &out) {
        if(auto *node=getMeshNode(n);node&&node->authoritative&&node->public_key.size==32) {out=node->public_key;return true;}
        auto found=authoritativeWarmKeys.find(n);
        if(found!=authoritativeWarmKeys.end()) {out=found->second;return true;}
        return false;
    }
    bool isKnownXeddsaSigner(NodeNum n) {
        auto *node=getMeshNode(n);return node&&(node->bitfield&NODEINFO_BITFIELD_HAS_XEDDSA_SIGNED_MASK);
    }
};
inline FixtureNodeDB fixtureDB;
inline FixtureNodeDB *nodeDB = &fixtureDB;
inline NodeNum getNodeNum() { return nodeDB->getNodeNum(); }
NodeNum getFrom(const meshtastic_MeshPacket *p);
bool isBroadcast(NodeNum n);
bool isToUs(const meshtastic_MeshPacket *p);
bool isFromUs(const meshtastic_MeshPacket *p);
int8_t getHopsAway(const meshtastic_MeshPacket &p, int8_t defaultIfUnknown = -1);
inline void printPacket(const char *, const meshtastic_MeshPacket *) {}
inline bool isBlockedEventCoordinatePacket(const meshtastic_MeshPacket *) { return false; }
RoutingAuthVerdict passesRoutingAuthGate(meshtastic_MeshPacket *);
bool checkXeddsaReceivePolicy(meshtastic_MeshPacket *);
inline bool fixtureDecodeOverride = false;
inline DecodeState fixtureDecodeState = DecodeState::DECODE_SUCCESS;
inline meshtastic_Data fixtureDecodedData{};
inline DecodeState perhapsDecode(meshtastic_MeshPacket *p) {
    if(fixtureDecodeOverride) {
        if(fixtureDecodeState==DecodeState::DECODE_SUCCESS) {
            p->which_payload_variant=meshtastic_MeshPacket_decoded_tag;p->decoded=fixtureDecodedData;
        }
        return fixtureDecodeState;
    }
    return p->which_payload_variant == meshtastic_MeshPacket_decoded_tag ? DecodeState::DECODE_SUCCESS : DecodeState::DECODE_OPAQUE;
}
namespace concurrency {
struct Lock { unsigned acquisitions=0; };
struct LockGuard {
    explicit LockGuard(Lock *lock) {assert(lock);++lock->acquisitions;}
};
}
inline concurrency::Lock fixtureCryptLock;
inline concurrency::Lock *cryptLock=&fixtureCryptLock;
struct FixtureCrypto {
    bool verifyResult=true;
    unsigned calls=0;
    NodeNum lastFrom=0,lastTo=0;
    PacketId lastId=0;
    bool xeddsa_verify(const uint8_t *,NodeNum from,PacketId id,NodeNum to,const meshtastic_Data *,const uint8_t *) {
        ++calls;lastFrom=from;lastId=id;lastTo=to;return verifyResult;
    }
};
inline FixtureCrypto fixtureCrypto;
inline FixtureCrypto *crypto=&fixtureCrypto;
void fixtureAuthReset();
uint32_t fixtureAuthEvaluationCount();
bool fixtureAuthCacheValid();
bool fixtureApplyAuthCache(meshtastic_MeshPacket *);
struct FixtureTextModule { bool recentlySeen(PacketId) { return false; } };
inline FixtureTextModule *textMessageModule = nullptr;
struct FixturePacketPool {
    std::array<meshtastic_MeshPacket, 64> packets{};
    std::array<bool, 64> used{};
    bool fail = false;
    meshtastic_MeshPacket *allocCopy(const meshtastic_MeshPacket &p) {
        if (fail) return nullptr;
        for (size_t i = 0; i < packets.size(); ++i) if (!used[i]) { used[i] = true; packets[i] = p; return &packets[i]; }
        return nullptr;
    }
    void release(meshtastic_MeshPacket *p) {
        for (size_t i = 0; i < packets.size(); ++i) if (p == &packets[i]) { assert(used[i]); used[i] = false; return; }
        assert(false && "release outside fixture pool");
    }
    size_t active() const { return std::count(used.begin(), used.end(), true); }
};
inline FixturePacketPool packetPool;
struct PacketHeader { uint8_t bytes[16]; };
inline uint8_t bytes[256]{};
inline const pb_msgdesc_t meshtastic_Data_msg{};
inline const pb_msgdesc_t meshtastic_Position_msg{},meshtastic_Telemetry_msg{},meshtastic_Waypoint_msg{},meshtastic_User_msg{};
inline size_t fixtureCanonicalDataSize = 16;
inline bool fixtureSizingSuccess=true,fixtureInnerDecodeSuccess=false,fixtureDataSizeTracksPayload=false;
inline size_t fixtureCanonicalInnerSize=8;
inline meshtastic_User fixtureBootstrapUser{};
inline uint32_t fixtureBootstrapCrc=0;
inline bool pb_get_encoded_size(size_t *out,const pb_msgdesc_t *fields,const void *value) {
    if(!fixtureSizingSuccess)return false;
    *out=fields==&meshtastic_Data_msg?fixtureCanonicalDataSize:fixtureCanonicalInnerSize;
    if(fields==&meshtastic_Data_msg&&fixtureDataSizeTracksPayload)
        *out+=static_cast<const meshtastic_Data *>(value)->payload.size;
    return true;
}
inline bool pb_decode_from_bytes(const uint8_t *,size_t,const pb_msgdesc_t *fields,void *out) {
    if(!fixtureInnerDecodeSuccess)return false;
    if(fields==&meshtastic_User_msg)std::memcpy(out,&fixtureBootstrapUser,sizeof(fixtureBootstrapUser));
    return true;
}
inline uint32_t crc32Buffer(const uint8_t *,size_t) {return fixtureBootstrapCrc;}
inline size_t pb_encode_to_bytes(uint8_t *, size_t, const pb_msgdesc_t *, const meshtastic_Data *) { return fixtureCanonicalDataSize; }
inline uint32_t pow_of_2(unsigned n) { return uint32_t(1) << n; }
inline uint32_t fixtureRandom = 0;
inline uint32_t random(uint32_t low, uint32_t high) { assert(high > low); return low + fixtureRandom % (high - low); }
inline long map(long value, long lo, long hi, long outLo, long outHi) { return (value - lo) * (outHi - outLo) / (hi - lo) + outLo; }
struct FixtureAirTime { float utilization = 0; float channelUtilizationPercent() { return utilization; } };
inline FixtureAirTime fixtureAirTime;
inline auto *airTime = &fixtureAirTime;
struct FixtureRegion { bool wideLora = false; };
inline FixtureRegion fixtureRegion;
inline auto *myRegion = &fixtureRegion;
struct RadioInterface {
    static constexpr uint8_t CWmin = 3, CWmax = 8;
    static constexpr uint32_t PROCESSING_TIME_MSEC = 4500;
    static constexpr uint8_t NUM_SYM_CAD = 2, NUM_SYM_CAD_24GHZ = 4;
    uint32_t slotTimeMsec = 28, airtime = 1000, lastLength = 0;
    bool lengthAirtime = false;
    uint8_t sf = 11;
    float bw = 250;
    uint32_t getPacketTime(uint32_t n, bool = false) { lastLength = n; return lengthAirtime ? 50 + n * 10 : airtime; }
    uint32_t getPacketTime(const meshtastic_MeshPacket *, bool = false);
    uint32_t getRetransmissionMsec(const meshtastic_MeshPacket *);
    uint32_t computeSlotTimeMsec();
    uint8_t getCWsize(float);
    uint32_t getTxDelayMsecWeightedWorst(float);
    bool shouldRebroadcastEarlyLikeRouter(meshtastic_MeshPacket *);
    uint32_t getTxDelayMsecWeighted(meshtastic_MeshPacket *);
    bool removePendingTXPacket(NodeNum, PacketId, uint32_t);
    void clampToLateRebroadcastWindow(NodeNum, PacketId) {}
};
