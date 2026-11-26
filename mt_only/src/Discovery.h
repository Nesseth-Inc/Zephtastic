// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "MtNodeInfo.h"
#include "stock/modules/NodeInfoModule.h"
#include "PrimaryChannel.hpp"
#include "HopPolicy.h"
#include "DeviceRolePolicy.h"

namespace mt {
// State/payload only. Endpoint owns nonce reservations and bounded queues;
// Service owns every radio operation, runtime arm and shared airtime limit.
class Discovery {
  struct Peer {uint32_t node=0,at=0;};Peer peers_[16]{};
  bool armed_=false,admitted_=false;
  uint32_t next_=0,last_=0;
  static constexpr uint32_t ReplyWindow=stock::nodeinfo::RecentSenderMs;
  static void text(uint8_t *p,size_t &at,uint8_t tag,const char *s) {
    const auto n=strlen(s);p[at++]=uint8_t((tag<<3)|2);p[at++]=uint8_t(n);memcpy(p+at,s,n);at+=n;
  }
public:
  uint32_t queued=0,replies=0,limited=0;
  void observeArm(bool armed,uint32_t now) {
    if(armed&&!armed_&&(!admitted_||bench::due(now,next_)))next_=now+30000;
    armed_=armed;
  }
  void cancel(){armed_=false;} // Keep admission/peer limits across configuration.
  void reschedule(uint32_t seconds) {
    // Measure changed intervals from the previous admission, never from the
    // edit. Keep the startup gate when nothing has been admitted yet, as well
    // as the independent minimum admission and per-peer reply limits.
    if(admitted_)next_=last_+bench::MtNodeRecord::intervalMs(seconds);
  }
  bool allowed(uint32_t now,bool channelAllowed=true)const {
    return stock::nodeinfo::allocReply(channelAllowed,admitted_,now,last_,0,true,false,0);
  }
  bool automaticReply(uint32_t now,bool channelAllowed,uint32_t routineMs,bool licensed,uint32_t licensedIntervalMs)const{
    return stock::nodeinfo::allocReply(channelAllowed,admitted_,now,last_,routineMs,false,licensed,licensedIntervalMs);
  }
  bool periodic(uint32_t now)const {return armed_&&bench::due(now,next_)&&allowed(now);}
  void defer(uint32_t now){next_=now+1000;}
  void admitted(uint32_t now,uint32_t seconds) {admitted_=true;last_=now;next_=now+bench::MtNodeRecord::intervalMs(seconds);++queued;}
  bool replyAllowed(uint32_t from,uint32_t now)const {
    bool free=false;
    for(const auto &p:peers_) {
      if(p.node&&uint32_t(now-p.at)<ReplyWindow){if(p.node==from)return false;}
      else free=true;
    }
    return free; // Never evict a live peer rate-limit record under a flood.
  }
  // Verified sender activity renews the stock seen-request window even when
  // its reply is suppressed. Unsigned shared-channel traffic cannot renew it.
  bool observeRequest(uint32_t from,uint32_t now,bool verified) {
    if(!verified)return replyAllowed(from,now);
    Peer *available=nullptr;
    for(auto &p:peers_){
      if(p.node==from) {
        const bool expired=uint32_t(now-p.at)>=ReplyWindow;
        p.at=now;
        return expired;
      }
      if(!available&&(!p.node||uint32_t(now-p.at)>=ReplyWindow))available=&p;
    }
    if(available)*available={from,now};
    return available!=nullptr;
  }
  void replied(uint32_t from,uint32_t now,bool verified=false) {
    if(!verified)(void)observeRequest(from,now,true); // Store first successful unsigned exchange.
    ++replies;
  }
  static bool wantsResponse(const bench::RouteMetadata &m) {
    // Pinned Router merges bit1 into want_response after signature validation.
    return m.wantResponse||(m.hasBitfield&&(m.bitfield&2));
  }
  static bool request(const bench::RouteMetadata &m) {
    // Empty User is a valid stock NodeInfo query but not useful node identity.
    return m.portnum==4&&(m.nodeInfoValid||!m.payloadLength)&&wantsResponse(m)&&
      !m.request_id&&!m.reply_id&&!m.emoji;
  }
  static unsigned responseHop(const uint8_t *p,unsigned configured=HopPolicy::Initial) {
    const unsigned hop=p[12]&7,start=(p[12]>>5)&7;
    return HopPolicy::response(start,hop,configured);
  }
  static bool encode(bench::MtCodec &codec,const bench::MtNodeRecord &record,
                     uint32_t ours,uint8_t model,uint32_t to,uint32_t id,
                     bool wantResponse,uint32_t request,bench::Frame &frame,uint8_t role=1,
                     bool ownerUnmessagable=false) {
    if(!record.valid()||!PrimaryChannel::defaultKey(record)||!codec.configured()||codec.channelHash()!=record.bytes[4]||!DeviceRolePolicy::supportedRole(role)||to<=1||to==0xfffffffeu||
      (wantResponse&&request))return false;
    char name[25]{},shortName[5]{},identity[10]{};
    memcpy(name,record.bytes+bench::MtNodeRecord::Name,24);
    memcpy(shortName,record.bytes+bench::MtNodeRecord::Short,4);
    snprintf(identity,sizeof(identity),"!%08x",ours);
    uint8_t user[64]{},data[80]{};size_t n=0;
    text(user,n,1,identity);text(user,n,2,name);text(user,n,3,shortName);
    bench::appendNodeInfoHardwareModel(user,n,model);
    if(role){user[n++]=0x38;user[n++]=role;}
    user[n++]=0x48;user[n++]=ownerUnmessagable?1:0; // Keep the historical false default.
    size_t at=0;data[at++]=8;data[at++]=4;data[at++]=18;data[at++]=uint8_t(n);
    memcpy(data+at,user,n);at+=n;
    if(request){data[at++]=53;for(unsigned i=0;i<4;++i)data[at++]=uint8_t(request>>(8*i));}
    // Modern stock receivers merge Data.bitfield bit1. Leave legacy field3
    // absent; this preserves the codec's prohibition on arbitrary requests.
    data[at++]=72;data[at++]=wantResponse?2:0;
    frame={};size_t length=0;
    const bool ok=codec.encodeZeroHopData(ours,to,id,data,at,frame.bytes,sizeof(frame.bytes),length);
    frame.len=uint16_t(length);bench::wipeKeyBuffer(user,sizeof(user));bench::wipeKeyBuffer(data,sizeof(data));return ok;
  }
};
}
