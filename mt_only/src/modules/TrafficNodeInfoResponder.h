// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "TrafficManagementSettings.h"
#include "NodeInfoPayloadCache.h"
#include "../stock/modules/DirectResponseThrottle.h"
#include <stdint.h>
#include <stddef.h>
#include "meshtastic/mesh.pb.h"

namespace mt::modules {

// The caller supplies only a request already admitted as public mesh traffic.
// It must derive hopsAway from hopStart-hopLimit and establish channel scope,
// replay admission, and local mute/ignore policy before invoking the responder.
struct TrafficNodeInfoRequest {
  uint32_t requester=0;
  uint32_t target=0;
  uint32_t requestId=0;
  uint32_t hopsAway=0;
  uint32_t routerHopCeiling=0;
  uint64_t nowMs=0;
  uint8_t channelIndex=0;
  uint8_t channelHash=0;
  bool publicRequest=false;
  bool requestShapeValid=false;
  bool replayAdmitted=false;
  bool requesterIgnored=false;
  bool requesterMuted=false;
  bool targetIgnored=false;
  bool targetMuted=false;
  bool channelMuted=false;
  bool routerRole=false;
};

struct TrafficNodeInfoReply {
  uint32_t proxyAuthor=0;  // Stock proxy attribution: requested target, unverified.
  uint32_t destination=0;
  uint32_t packetId=0;     // Fresh public packet ID allocated by owner routing.
  uint32_t requestId=0;    // Correlation only; never reused as packet ID.
  uint8_t channelIndex=0;
  uint8_t channelHash=0;
  uint8_t hopLimit=0;
  uint8_t hopStart=0;
  bool unsignedPublicProxy=true;
  uint8_t userPayload[traffic::NodeInfoPayloadCache::kMaxPayload]{};
  uint16_t userPayloadSize=0;
};

struct TrafficNodeInfoOwner {
  void *context=nullptr;
  // This explicit availability check makes the generated configuration
  // actionable only when the exact public-proxy route is actually bound.
  bool (*proxyRouteAvailable)(void *context)=nullptr;
  bool (*allocatePublicPacketId)(void *context,uint32_t &packetId)=nullptr;
  // Must encode a stock-shaped public unsigned proxy with from=proxyAuthor,
  // to=destination, request_id=requestId, hop_limit/start=0. It must not invoke
  // PKI signing or promote this claimed author to trusted identity state.
  bool (*submitPublicProxy)(void *context,const TrafficNodeInfoReply &reply)=nullptr;
};

class TrafficNodeInfoResponder {
 public:
  enum class Result:uint8_t {
    Disabled, Invalid, PolicyDenied, NoFreshCache, ChannelMismatch,
    HopLimit, RateLimited, ReplayCapacity, OwnerUnavailable, NoPacketId, QueueFailed, Replied
  };
  static constexpr uint64_t MaxCacheAgeMs=traffic::NodeInfoPayloadCache::kMaxAgeMs;
  static constexpr uint64_t PerRequesterMs=stock::DirectResponseThrottle::PerRequesterMs;
  static constexpr uint64_t PerTargetMs=stock::DirectResponseThrottle::PerTargetMs;
  static constexpr uint64_t GlobalFloorMs=stock::DirectResponseThrottle::GlobalFloorMs;
  static constexpr uint64_t ReplayAgeMs=600000;

  bool configure(const TrafficManagementSettings &settings);
  // Cache only an already admitted PUBLIC NodeInfo User payload. The payload
  // is decoded and its User.id must identify nodeId exactly. `publicAdmitted`
  // is caller-owned evidence from receive/decryption/signature policy.
  bool observePublicUser(uint32_t nodeId,const uint8_t *payload,uint16_t size,
                         uint64_t observedMs,uint8_t channelIndex,uint8_t channelHash,
                         bool publicAdmitted);
  Result respond(const TrafficNodeInfoRequest &request,uint32_t localIdentity,
                 const TrafficNodeInfoOwner &owner);
  void forget(uint32_t nodeId);
  bool enabled()const{return configured_&&maxHops_!=0;}
  bool supported(const TrafficNodeInfoOwner &owner)const {
    return configured_&&owner.proxyRouteAvailable&&owner.allocatePublicPacketId&&
      owner.submitPublicProxy&&owner.proxyRouteAvailable(owner.context);
  }
  uint32_t configuredMaxHops()const{return maxHops_;}

 private:
  struct ChannelStamp {uint32_t node=0;uint64_t observedMs=0;uint8_t channelIndex=0,channelHash=0;};
  struct ReplayStamp {uint32_t requester=0,requestId=0;uint64_t at=0;};
  traffic::NodeInfoPayloadCache cache_;
  ChannelStamp channels_[traffic::NodeInfoPayloadCache::kCapacity]{};
  stock::DirectResponseThrottle throttle_;
  ReplayStamp replays_[16]{};
  uint32_t maxHops_=0;
  bool configured_=false;

  static bool exactUserIdentity(const uint8_t *payload,uint16_t size,uint32_t nodeId);
};

} // namespace mt::modules
