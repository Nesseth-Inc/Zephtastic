// SPDX-License-Identifier: GPL-3.0-only
#include "TrafficNodeInfoResponder.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include <string.h>

namespace mt::modules {

bool TrafficNodeInfoResponder::configure(const TrafficManagementSettings &settings) {
  meshtastic_ModuleConfig_TrafficManagementConfig c{};
  settings.get(c);
  // Stock gates the answer on a positive setting; the role further caps its range.
  const uint32_t requested=c.nodeinfo_direct_response_max_hops;
  maxHops_=requested;
  configured_=true;
  return true;
}

bool TrafficNodeInfoResponder::exactUserIdentity(const uint8_t *payload,uint16_t size,uint32_t nodeId) {
  if(!payload||!size||size>traffic::NodeInfoPayloadCache::kMaxPayload||!nodeId)return false;
  meshtastic_User user=meshtastic_User_init_zero;
  auto stream=pb_istream_from_buffer(payload,size);
  if(!pb_decode(&stream,meshtastic_User_fields,&user)||stream.bytes_left)return false;
  // Meshtastic User has no node_num field. Its canonical id is ! plus the
  // eight hex digits of the mesh node number. Do not serve incomplete or
  // mismatched identity claims from an opaque payload cache.
  if(user.id[0]!='!'||strlen(user.id)!=9)return false;
  uint32_t id=0;
  for(unsigned i=1;i<9;++i) {
    const char c=user.id[i];uint8_t n;
    if(c>='0'&&c<='9')n=uint8_t(c-'0');
    else if(c>='a'&&c<='f')n=uint8_t(c-'a'+10);
    else if(c>='A'&&c<='F')n=uint8_t(c-'A'+10);
    else return false;
    id=(id<<4)|n;
  }
  return id==nodeId&&user.long_name[0]&&user.short_name[0];
}

bool TrafficNodeInfoResponder::observePublicUser(uint32_t nodeId,const uint8_t *payload,
    uint16_t size,uint64_t observedMs,uint8_t channelIndex,uint8_t channelHash,bool publicAdmitted) {
  if(!configured_||!publicAdmitted||!exactUserIdentity(payload,size,nodeId))return false;
  if(!cache_.observe(nodeId,payload,size,observedMs))return false;
  unsigned slot=traffic::NodeInfoPayloadCache::kCapacity;
  for(unsigned i=0;i<traffic::NodeInfoPayloadCache::kCapacity;++i) {
    if(channels_[i].node==nodeId){slot=i;break;}
    if(!channels_[i].node&&slot==traffic::NodeInfoPayloadCache::kCapacity)slot=i;
  }
  if(slot==traffic::NodeInfoPayloadCache::kCapacity) {
    slot=0;
    for(unsigned i=1;i<traffic::NodeInfoPayloadCache::kCapacity;++i)
      if(channels_[i].observedMs<channels_[slot].observedMs)slot=i;
  }
  channels_[slot]={nodeId,observedMs,channelIndex,channelHash};
  return true;
}

TrafficNodeInfoResponder::Result TrafficNodeInfoResponder::respond(
    const TrafficNodeInfoRequest &r,uint32_t localIdentity,const TrafficNodeInfoOwner &owner) {
  if(!enabled())return Result::Disabled;
  if(!r.requestShapeValid||!r.requestId||!r.requester||!r.target||!localIdentity||
     r.requester==localIdentity||r.target==localIdentity||r.requester==r.target||r.hopsAway>7)
    return Result::Invalid;
  if(!r.publicRequest||!r.replayAdmitted||r.requesterIgnored||r.requesterMuted||
     r.targetIgnored||r.targetMuted||r.channelMuted)return Result::PolicyDenied;
  const uint32_t roleLimit=r.routerRole?3u:0u;
  uint32_t ceiling=maxHops_;
  if(r.routerHopCeiling&&r.routerHopCeiling<ceiling)ceiling=r.routerHopCeiling;
  if(ceiling>roleLimit)ceiling=roleLimit;
  if(r.hopsAway>ceiling)return Result::HopLimit;
  ChannelStamp *stamp=nullptr;
  for(auto &c:channels_)if(c.node==r.target){stamp=&c;break;}
  if(!stamp)return Result::NoFreshCache;
  if(stamp->channelIndex!=r.channelIndex||stamp->channelHash!=r.channelHash)return Result::ChannelMismatch;
  TrafficNodeInfoReply reply{};
  reply.proxyAuthor=r.target;reply.destination=r.requester;reply.requestId=r.requestId;
  reply.channelIndex=r.channelIndex;reply.channelHash=r.channelHash;reply.hopLimit=0;reply.hopStart=0;
  if(!cache_.copyFresh(r.target,r.nowMs,reply.userPayload,sizeof(reply.userPayload),reply.userPayloadSize))
    return Result::NoFreshCache;
  if(!exactUserIdentity(reply.userPayload,reply.userPayloadSize,r.target))return Result::NoFreshCache;
  stock::DirectResponseThrottle::Reservation rates;
  if(!throttle_.directResponseAllowed(r.requester,r.target,r.nowMs,rates))return Result::RateLimited;
  ReplayStamp *replaySlot=nullptr;
  for(auto &seen:replays_){
    if(seen.requester==r.requester&&seen.requestId==r.requestId&&
       (r.nowMs<seen.at||r.nowMs-seen.at<ReplayAgeMs))return Result::RateLimited;
    if(!replaySlot&&(!seen.requester||(r.nowMs>=seen.at&&r.nowMs-seen.at>=ReplayAgeMs)))replaySlot=&seen;
  }
  if(!replaySlot)return Result::ReplayCapacity;
  if(!owner.proxyRouteAvailable||!owner.allocatePublicPacketId||!owner.submitPublicProxy||
     !owner.proxyRouteAvailable(owner.context))return Result::OwnerUnavailable;
  if(!owner.allocatePublicPacketId(owner.context,reply.packetId)||!reply.packetId)return Result::NoPacketId;
  if(!owner.submitPublicProxy(owner.context,reply))return Result::QueueFailed;
  throttle_.accepted(rates,r.requester,r.target,r.nowMs);
  *replaySlot={r.requester,r.requestId,r.nowMs};
  return Result::Replied;
}

void TrafficNodeInfoResponder::forget(uint32_t nodeId) {
  if(!nodeId)return;
  cache_.forget(nodeId);
  for(auto &c:channels_)if(c.node==nodeId)c={};
}

} // namespace mt::modules
