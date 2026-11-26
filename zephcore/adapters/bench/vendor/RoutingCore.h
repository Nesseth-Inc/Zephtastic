// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "RadioPacketTiming.h"
// Bounded transit-only adaptation of Meshtastic NextHopRouter, PacketHistory,
// FloodingRouter and ReliableRouter at firmware commit
// 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c. See bench/upstream-differential.
// This is not an endpoint: it never originates an ACK/NAK or a private message.
// A channel-decoded protobuf is eligibility evidence, NOT authentication.
#include "RelayCore.h"
#include "MtCodec.h"

namespace bench {
struct RoutingCounters {
  uint32_t opaque=0, decoded=0, directed=0, fallbacks=0, implicitAcks=0;
  uint32_t responseCancels=0, responseRejected=0, nakCancels=0;
  uint32_t routesLearned=0, routeRejected=0, routeStale=0, collisions=0;
  uint32_t knownFull=0, historyFull=0, pendingFull=0, routeFull=0;
  uint32_t pendingExpired=0, attemptLimit=0, queueCancelled=0, duplicateRelayCancelled=0, decodeRejected=0;
  uint32_t blockedAirtime=0, noTiming=0;
  uint32_t decodeOpaque=0, signatureOpaque=0, signatureRejected=0, cryptoFailure=0, signatureVerified=0;
};

class RoutingCore {
public:
  static constexpr uint8_t MAX_ATTEMPTS=3, QUEUE_SIZE=8, PENDING_SIZE=8;
  static constexpr uint8_t OPAQUE_SIZE=64, HISTORY_SIZE=64, KNOWN_SIZE=32, ROUTE_SIZE=16;
  static constexpr uint32_t QUEUE_TTL=30000, PENDING_TTL=90000, OPAQUE_TTL=60000, HISTORY_TTL=600000;
  static constexpr uint32_t ROUTE_TTL=1800000, KNOWN_TTL=1800000;
  using AirtimeProvider=uint32_t (*)(size_t wireBytes,void *context);
private:
  using ReadHint=uint8_t (*)(void *,uint32_t);
  using WriteHint=void (*)(void *,uint32_t,uint8_t);
  void *hintContext_=nullptr;ReadHint readHint_=nullptr;WriteHint writeHint_=nullptr;
  void storeHint(uint32_t to,uint8_t hop){if(writeHint_)writeHint_(hintContext_,to,hop);}

  struct Key { uint32_t from=0,id=0; };
  struct Opaque {
    Key key{}; uint32_t at=0,lastAttempt=0,to=0; uint16_t len=0;
#if defined(CONFIG_BOARD_HELTEC_T096)
    // Duplicate routing uses the packet key. This fingerprint only counts
    // payload variants after the relay queue releases the ciphertext.
    uint64_t payloadFingerprint=0;
#else
    uint8_t payload[239]{};
#endif
    uint8_t flags=0,best=0,attempts=0,channel=0;
    bool used=false;
  };
  struct History {
    Key key{}; uint32_t to=0,at=0,lastAttempt=0,lifecycleExpires=0; uint64_t payload=0;
    uint32_t queueLifetimeMs=QUEUE_TTL,historyLifetimeMs=HISTORY_TTL,historyExpires=0;
    uint16_t wireSize=0;
    uint8_t originalNext=0,best=0,ourTxHop=0,relayers[6]{},attempts=0,channel=0,channelContext=0;
    bool used=false,sent=false,cancelled=false;
  };
  struct Pending {
    Frame frame{}; uint32_t nextAt=0,delay=0,lifecycleExpires=0;
    bool used=false,queued=false,queueBlocked=false;
  };
  struct Known {
    uint32_t node=0,at=0,directAt=0; bool used=false,direct=false;
  };
  struct Route {
    uint32_t to=0,at=0; uint8_t hop=0,failures=0; bool used=false;
  };
  struct OriginHistory {
    uint32_t to=0,id=0,at=0; uint64_t payload=0; uint16_t len=0;
    uint8_t channel=0,channelContext=0,flags=0,relayers[3]{},relayerCount=0; bool used=false;
  };
  Opaque opaque_[OPAQUE_SIZE]{};
  History history_[HISTORY_SIZE]{};
  Frame queue_[QUEUE_SIZE]{};
  bool queueDecoded_[QUEUE_SIZE]{};
  Pending pending_[PENDING_SIZE]{};
  Known known_[KNOWN_SIZE]{};
  Route routes_[ROUTE_SIZE]{};
  OriginHistory origins_[16]{};
  uint32_t identity_=0;
  uint8_t relay_=0,channel_=0;
  uint8_t channelHashes_[8]{};unsigned channelHashCount_=0;bool explicitChannelSet_=false;
  bool channelAllowed(uint8_t hash)const {if(!explicitChannelSet_&&hash==channel_)return true;for(unsigned i=0;i<channelHashCount_;++i)if(channelHashes_[i]==hash)return true;return false;}
  bool ready_=false,continuous_=false,alwaysRelay_=true,otherOpaque_=false;
  uint32_t slotMs_=28,processingMs_=4500;
  uint8_t utilization_=0;
  int8_t rxSnr_=0;bool haveSnr_=false;
  bool dynamicLifetimes_=false;
  AirtimeProvider airtime_=nullptr;
  void *airtimeContext_=nullptr;
  struct ActiveKind {Key key{};uint64_t digest=0;uint16_t len=0;uint8_t channelContext=0;bool valid=false,decoded=false;} activeKind_{};

  bool decodedLifetime(size_t wireBytes, mt::PacketLifetime::Bounds &out) const {
    if (wireBytes < 17u || wireBytes > 255u) return false;
    if (!dynamicLifetimes_) {
      out = { QUEUE_TTL, PENDING_TTL, HISTORY_TTL };
      return true;
    }
    if (!airtime_ || slotMs_ < 1u || slotMs_ > mt::RadioSlot::MaxSlotMs) return false;
    if (processingMs_ > 60000u) return false;
    const uint32_t air = airtime_(wireBytes, airtimeContext_);
    mt::RadioTiming::Deadline deadline{};
    if (!mt::RadioTiming::resolve(air, deadline)) return false;
    const uint64_t retransmission = uint64_t(2u) * air
        + uint64_t(256u + 16u + 32u) * slotMs_
        + processingMs_;
    if (retransmission > UINT32_MAX) return false;
    mt::PacketLifetime::Bounds bounds{};
    if (!mt::PacketLifetime::resolve(deadline.hostMs, uint32_t(retransmission), bounds)) return false;
    out = bounds;
    return true;
  }

  static Key keyOf(const uint8_t *p) { return Key{le32(p+4),le32(p+8)}; }
  static bool same(Key a,Key b) { return a.from==b.from && a.id==b.id; }
  static bool contextMatches(const History &h,const Frame &frame) {
    return h.channel==frame.bytes[13]&&h.channelContext==frame.channelContext;
  }
  static bool contextMatches(const History &h,const uint8_t *p,const RouteMetadata &metadata) {
    return h.channel==p[13]&&h.channelContext==metadata.channelContext;
  }
  static uint64_t fingerprint(const uint8_t *p,size_t n) {
    // Diagnostics/association only. This is not a MAC or an authenticity check.
    uint64_t h=1469598103934665603ull;
    for(size_t i=0;i<n;++i) {h^=p[i];h*=1099511628211ull;}
    return h;
  }
  uint32_t initialDelay(uint32_t jitter) const {
    if(!haveSnr_) return 150+jitter%850;
    return mt::RebroadcastTiming::initialDelay(slotMs_, alwaysRelay_, rxSnr_, jitter);
  }
  uint32_t lateRebroadcastDelay(int8_t snr) const {
    return mt::RebroadcastTiming::worstDelay(slotMs_, snr);
  }
  void clampLateDuplicate(Frame *q,uint32_t now) const {
    if(q&&!q->lateWindow) {
      q->dueAt=now+lateRebroadcastDelay(q->hasRxSnr?q->rxSnr:0);
      q->lateWindow=true;
    }
  }
  Opaque *findOpaque(Key key) {
    for(auto &s:opaque_) if(s.used && same(s.key,key)) return &s;
    return nullptr;
  }
  History *findHistory(Key key) {
    for(auto &s:history_) if(s.used && same(s.key,key)) return &s;
    return nullptr;
  }
  Frame *findQueue(Key key) {
    for(auto &q:queue_) if(q.used && same(keyOf(q.bytes),key)) return &q;
    return nullptr;
  }
  Frame *freeQueue() { for(auto &q:queue_) if(!q.used) return &q; return nullptr; }
  void markQueueKind(Frame *q,bool decoded) {
    for(unsigned i=0;i<QUEUE_SIZE;++i) if(q==&queue_[i]) {queueDecoded_[i]=decoded;return;}
  }
  Pending *findPending(Key key) {
    for(auto &s:pending_) if(s.used && same(keyOf(s.frame.bytes),key)) return &s;
    return nullptr;
  }
  static bool wasRelayer(const History &h,uint8_t byte) {
    if(!byte) return false;
    for(auto r:h.relayers) if(r==byte) return true;
    return false;
  }
  static bool soleRelayer(const History &h,uint8_t byte) {
    if(!wasRelayer(h,byte)) return false;
    for(auto r:h.relayers) if(r && r!=byte) return false;
    return true;
  }
  static void addRelayer(History &h,uint8_t byte) {
    if(!byte || wasRelayer(h,byte)) return;
    for(unsigned i=5;i>0;--i) h.relayers[i]=h.relayers[i-1];
    h.relayers[0]=byte;
  }
  void observeRelayer(History &h,uint8_t hop,uint8_t relay) {
    // PacketHistory::wasSeenRecently only associates downstream relayers when
    // this node transmitted and the hop distance is our sent hop or one below.
    if(h.sent && (hop==h.ourTxHop || (h.ourTxHop && hop==h.ourTxHop-1)))
      addRelayer(h,relay);
  }
  Known *knownNode(uint32_t node) {
    for(auto &n:known_) if(n.used && n.node==node) return &n;
    return nullptr;
  }
  uint32_t resolveByte(uint8_t byte,bool direct,uint32_t now,bool count=true) {
    if(!byte || byte==relay_) return 0;
    uint32_t found=0;
    for(const auto &n:known_) if(n.used && uint8_t(n.node)==byte &&
                                 uint32_t(now-n.at)<KNOWN_TTL) {
      if(found && found!=n.node) {if(count) ++routingStats.collisions;return 0;}
      found=n.node;
    }
    if(!found) return 0;
    auto *n=knownNode(found);
    if(direct && (!n->direct || uint32_t(now-n->directAt)>=KNOWN_TTL)) return 0;
    return found;
  }
  void rememberNode(const uint8_t *p,const RouteMetadata &m,uint32_t now) {
    if(!m.nodeInfoValid || m.portnum!=4) return;
    const uint32_t from=le32(p+4);
    if(from==identity_) return;
    Known *node=knownNode(from);
    if(!node) for(auto &n:known_) if(!n.used) {node=&n;break;}
    if(!node) {++routingStats.knownFull;return;}
    node->used=true;node->node=from;node->at=now;
    const uint8_t hop=p[12]&7,start=p[12]>>5;
    // Decoded NodeInfo is not authenticated identity. A direct-header match is
    // bounded neighbor evidence; a relayed NodeInfo cannot create a neighbor.
    if(start && hop==start && p[15]==uint8_t(from)) {
      node->direct=true;node->directAt=now;
    }
  }
  Route *findRoute(uint32_t to) {
    for(auto &r:routes_) if(r.used && r.to==to) return &r;
    return nullptr;
  }
  void learnRoute(uint32_t to,uint8_t byte,uint32_t now) {
    if(!knownNode(to) || !resolveByte(byte,false,now)) {++routingStats.routeRejected;return;}
    Route *route=findRoute(to);
    if(!route) for(auto &r:routes_) if(!r.used) {route=&r;break;}
    if(!route) {++routingStats.routeFull;return;}
    const bool changed=!route->used || route->hop!=byte;
    route->used=true;route->to=to;route->hop=byte;route->at=now;
    if(changed) route->failures=0;
    storeHint(to,byte);
    ++routingStats.routesLearned;
  }
  void routeFailure(uint32_t to) {
    storeHint(to,0);
    auto *r=findRoute(to);
    if(r) {if(r->failures<255) ++r->failures;r->hop=0;}
  }
public:
  // Apply only the public header's route hint to a locally originated frame.
  // The encrypted/canonical Data bytes and generated packet ID remain owned
  // by Endpoint; callers invoke this again before each retry.
  void prepareOrigin(Frame &frame,uint8_t channelContext,uint32_t now) {
    if(frame.len<17||frame.len>255||le32(frame.bytes+4)!=identity_||
       le32(frame.bytes)==0xffffffffu||le32(frame.bytes)==identity_)return;
    frame.bytes[14]=nextHop(le32(frame.bytes),now);
    frame.bytes[15]=relay_;
    frame.channelContext=channelContext;
  }
  OriginHistory *findOrigin(const uint8_t *p,uint8_t channelContext) {
    for(auto &o:origins_)if(o.used&&o.id==le32(p+8)&&o.to==le32(p)&&
       o.channel==p[13]&&o.channelContext==channelContext)return &o;
    return nullptr;
  }
  void noteOriginTransmit(const Frame &frame,uint32_t now) {
    if(frame.len<17||frame.len>255||le32(frame.bytes+4)!=identity_||
       le32(frame.bytes)==0xffffffffu||le32(frame.bytes)==identity_||!le32(frame.bytes+8))return;
    OriginHistory *o=findOrigin(frame.bytes,frame.channelContext);
    if(o&&uint32_t(now-o->at)>=HISTORY_TTL)o=nullptr;
    if(!o)for(auto &candidate:origins_)if(!candidate.used||uint32_t(now-candidate.at)>=HISTORY_TTL){o=&candidate;break;}
    if(!o)return;
    const uint64_t payload=fingerprint(frame.bytes+16,frame.len-16);
    if(!o->used||o->id!=le32(frame.bytes+8)||o->to!=le32(frame.bytes)||o->payload!=payload||
       o->channel!=frame.bytes[13]||o->channelContext!=frame.channelContext) {
      *o=OriginHistory{};o->used=true;o->id=le32(frame.bytes+8);o->to=le32(frame.bytes);
      o->len=frame.len;o->payload=payload;o->channel=frame.bytes[13];o->channelContext=frame.channelContext;
      o->flags=uint8_t(frame.bytes[12]&0xf8u);
      if(relay_){o->relayers[0]=relay_;o->relayerCount=1;}
    }
    o->at=now;
  }
  // Repeated over-air copies of our own public request provide the bounded
  // relayer evidence used by stock PacketHistory. This is header evidence,
  // never authentication, and it is scoped to the same channel and payload.
  void observeOriginRelay(const uint8_t *p,size_t n,const RouteMetadata &m,uint32_t now) {
    if(!p||n<17||n>255||le32(p+4)!=identity_||le32(p)==0xffffffffu||
       !m.channelDecoded||m.decodeRejected||m.backendFailed)return;
    auto *o=findOrigin(p,m.channelContext);
    if(!o||uint32_t(now-o->at)>=HISTORY_TTL||o->len!=n||
       (p[12]&0xf8u)!=o->flags||
       o->payload!=fingerprint(p+16,n-16)||!p[15])return;
    for(unsigned i=0;i<o->relayerCount;++i)if(o->relayers[i]==p[15])return;
    if(o->relayerCount<3)o->relayers[o->relayerCount++]=p[15];
  }
  // Called only after Endpoint has admitted a channel-bound, correlated
  // successful response. Match stock's two public-header provenance cases:
  // a relayer already seen on our request path, or a direct response when we
  // were the sole observed request relayer. No PKI trust is inferred here.
  bool observeOriginResponse(const Frame &original,const uint8_t *p,size_t n,
                             const RouteMetadata &m,uint32_t now) {
    if(original.len<17||!p||n<17||n>255||le32(original.bytes+4)!=identity_||
       le32(original.bytes)==0xffffffffu||le32(p)!=identity_||
       le32(p+4)!=le32(original.bytes)||!m.request_id||
       m.request_id!=le32(original.bytes+8)||p[13]!=original.bytes[13]||
       m.channelContext!=original.channelContext)return false;
    auto *o=findOrigin(original.bytes,original.channelContext);
    if(!o||uint32_t(now-o->at)>=HISTORY_TTL||o->len!=original.len||
       o->payload!=fingerprint(original.bytes+16,original.len-16))return false;
    bool us=false,responseRelayer=false;
    for(unsigned i=0;i<o->relayerCount;++i){us|=o->relayers[i]==relay_;responseRelayer|=o->relayers[i]==p[15];}
    const bool direct=(p[12]&7u)==((p[12]>>5)&7u);
    const bool sole=us&&o->relayerCount==1;
    if(!((us&&responseRelayer)||(direct&&sole)))return false;
    learnRoute(le32(p+4),p[15],now);
    return true;
  }
  // An Endpoint may call this only after it has matched a PKI-authenticated
  // response to the exact private job. PKI packet marker zero does not carry
  // the shared channel hash, so this path correlates by that admitted job and
  // its Service channel context instead of comparing p[13]. The mutable air
  // header remains route evidence only; this method never upgrades node/key
  // trust and still applies the existing bounded origin/relay and route gates.
  bool observeAuthenticatedPrivateResponse(const Frame &original,const uint8_t *p,size_t n,
      const RouteMetadata &m,uint32_t expectedPeer,uint32_t now) {
    if(original.len<17||original.len>255||!p||n<17||n>255||
       le32(original.bytes+4)!=identity_||le32(original.bytes)!=expectedPeer||
       expectedPeer<=1||expectedPeer>=0xfffffffeu||original.bytes[13]!=0||
       !le32(original.bytes+8)||le32(p)!=identity_||le32(p+4)!=expectedPeer||!le32(p+8)||
       !m.request_id||m.request_id!=le32(original.bytes+8)||!p[15]||p[15]==relay_||
       (p[12]&7u)>((p[12]>>5)&7u)||m.backendFailed||m.decodeRejected||m.signatureRejected||
       (m.channelDecoded&&m.channelContext!=original.channelContext))return false;
    if(m.portnum==5&&(!m.routingPresent||!m.routingErrorPresent||m.routingError))return false;
    auto *o=findOrigin(original.bytes,original.channelContext);
    if(!o||uint32_t(now-o->at)>=HISTORY_TTL||o->len!=original.len||
       o->payload!=fingerprint(original.bytes+16,original.len-16))return false;
    bool us=false,responseRelayer=false;
    for(unsigned i=0;i<o->relayerCount;++i) {
      us|=o->relayers[i]==relay_;responseRelayer|=o->relayers[i]==p[15];
    }
    const bool direct=(p[12]&7u)==((p[12]>>5)&7u);
    if(!((us&&responseRelayer)||(direct&&us&&o->relayerCount==1)))return false;
    learnRoute(expectedPeer,p[15],now);return true;
  }
  // Exact immutable private ciphertext echo. Endpoint performs the full job
  // byte comparison before calling this bounded route-history update.
  bool observeOpaqueOriginRelay(const Frame &original,const uint8_t *p,size_t n,uint32_t now) {
    if(original.len<17||original.len>255||!p||n!=original.len||
       le32(original.bytes+4)!=identity_||le32(original.bytes)==0xffffffffu||
       original.bytes[13]!=0||le32(p+4)!=identity_||le32(p)!=le32(original.bytes)||
       le32(p+8)!=le32(original.bytes+8)||p[13]!=0||!p[15]||p[15]==relay_||
       (p[12]>>5)!=(original.bytes[12]>>5)||(p[12]&0x18u)!=(original.bytes[12]&0x18u)||
       (p[12]&7u)>(original.bytes[12]&7u)||memcmp(p+16,original.bytes+16,n-16))return false;
    auto *o=findOrigin(original.bytes,original.channelContext);
    if(!o||uint32_t(now-o->at)>=HISTORY_TTL||o->len!=original.len||
       o->payload!=fingerprint(original.bytes+16,original.len-16))return false;
    for(unsigned i=0;i<o->relayerCount;++i)if(o->relayers[i]==p[15])return true;
    if(o->relayerCount<3)o->relayers[o->relayerCount++]=p[15];
    return true;
  }
private:
  void stopPending(Key key,bool cancelQueue) {
    auto *h=findHistory(key);if(!h)return;
    auto *p=findPending(key);
    if(p&&contextMatches(*h,p->frame))p->used=false;
    h->cancelled=true;
    if(cancelQueue) {
      auto *q=findQueue(key);
      if(q&&contextMatches(*h,*q)) {q->used=false;++routingStats.queueCancelled;}
    }
  }
  bool controlResponse(const uint8_t *p,const RouteMetadata &m,uint32_t now) {
    // Unlike the endpoint ReliableRouter, this transit node does not interpret
    // a receipt for itself, create ACK proofs, or synthesize any ACK/NAK.
    // The pinned sniffReceived passes request_id to checkRelayers/cancel.
    // reply_id alone can be a quoted message, not a protocol receipt.
    const uint32_t id=m.request_id;
    if(!id || le32(p)==0xffffffffu) return false;
    auto *h=findHistory(Key{le32(p),id});
    if(!h || !contextMatches(*h,p,m) || h->to==0xffffffffu) {
      ++routingStats.responseRejected;return false;
    }
    const uint32_t responder=le32(p+4);
    const uint8_t relay=(p[12]>>5) ? p[15] : 0;
    const bool sameRelayer=wasRelayer(*h,relay);
    const bool direct=(p[12]&7)==(p[12]>>5);
    // Sender/to/request matching plus prior observed two-way relayer evidence
    // bounds the untrusted clear-header association. It does not authenticate.
    const bool provenance=h->sent && ((wasRelayer(*h,relay_) && sameRelayer) ||
                                     (direct && soleRelayer(*h,relay_)));
    // Stock NextHopRouter cancels by an exact (to, request_id) match for any
    // nonlocal, nonbroadcast response. The responder identity is not required
    // to be in NodeDB. This is cancellation only: it does not authenticate the
    // responder or widen route-learning provenance below.
    const bool nak=m.routingPresent && m.routingErrorPresent && m.routingError!=0;
    if(!nak && provenance) learnRoute(responder,relay,now);
    // Official NextHop/Flooding sniffReceived cancel by request_id, not reply_id.
    if(m.request_id) {
      stopPending(h->key,true);
      if(nak) ++routingStats.nakCancels; else ++routingStats.responseCancels;
    }
    return true;
  }
  void observation(Opaque &s,const uint8_t *p,size_t n) {
    lastSeen=true;lastPreviousFlags=s.flags;lastPayloadBaselineLength=s.len;
    const size_t body=n-16;
#if defined(CONFIG_BOARD_HELTEC_T096)
    if (body != s.len || fingerprint(p+16,body) != s.payloadFingerprint) ++stats.payloadVariants;
#else
    const size_t span=s.len>body?s.len:body;
    for(size_t i=0;i<span;++i) if(i>=s.len || i>=body || s.payload[i]!=p[16+i]) {
      ++lastPayloadDifferences;lastPayloadDiffMask[i/8]|=uint8_t(1u<<(i%8));
      if(lastPayloadFirstDifference==255) lastPayloadFirstDifference=uint8_t(i);
      lastPayloadLastDifference=uint8_t(i);
    }
    if(lastPayloadDifferences) ++stats.payloadVariants;
#endif
    if((s.flags&0xf8)!=(p[12]&0xf8)) ++stats.mutableCopies;
  }
  bool admitOpaque(const uint8_t *p,size_t n,const RouteMetadata &m,uint32_t now,uint32_t jitter) {
    const uint8_t hop=p[12]&7,start=p[12]>>5;
    if(!hop) {++stats.exhausted;return false;}
    const uint8_t next=start?p[14]:0;
    if(next && next!=relay_) {++stats.unsupported;return false;}
    auto *s=findOpaque(keyOf(p));auto *q=findQueue(keyOf(p));
    const bool originator=start && start==hop;
    if(s) {
      observation(*s,p,n);
      // Exact pinned opaque path: separate history, no better-hop routing
      // upgrade and no implicit ACK/cancellation/NodeDB side effects.
      // Stock's isolated opaque seen-set admits every genuine originator
      // retransmission. Sender retry cadence, packet lifetime, and hop limit
      // bound it; the decoded transit ARQ attempt cap does not apply here.
      if(!originator || q) {
        if(q&&m.routerLateRole&&!originator) {
          clampLateDuplicate(q,now);
        } else if(q&&m.clientBaseRole) {
          if(m.clientBaseFavoriteTraffic)clampLateDuplicate(q,now);
          else if(!m.mqttIngress){q->used=false;++routingStats.queueCancelled;++routingStats.duplicateRelayCancelled;}
        }
        ++stats.duplicates;return false;
      }
      ++stats.retries;
    }
    q=freeQueue();if(!q) {++stats.full;return false;}
    if(!s) for(auto &h:opaque_) if(!h.used) {s=&h;break;}
    if(!s) {++stats.full;++routingStats.historyFull;return false;}
    if(!s->used) {
      *s=Opaque{};s->used=true;s->key=keyOf(p);s->at=now;s->len=uint16_t(n-16);
#if defined(CONFIG_BOARD_HELTEC_T096)
      s->payloadFingerprint=fingerprint(p+16,n-16);
#else
      memcpy(s->payload,p+16,n-16);
#endif
      s->flags=p[12];s->best=hop;
    }
    s->to=le32(p);s->channel=p[13];
    *q=Frame{};q->used=true;q->len=uint16_t(n);q->channelContext=0;memcpy(q->bytes,p,n);
    markQueueKind(q,false);
    q->bytes[12]=uint8_t((p[12]&0xf8)|(hop-((m.preserveHopLimit&&start&&hop<start)?0:1)));
    if(le32(p)==0xffffffffu) q->bytes[12]&=uint8_t(~8u);
    // Router::send, rather than NextHopRouter::send, is called by the pinned
    // opaque path: preserve modern next_hop exactly. Legacy bytes are ignored.
    q->bytes[14]=next;q->bytes[15]=relay_;
    q->attempts=s->attempts;q->dueAt=now+initialDelay(jitter);q->expires=now+QUEUE_TTL;
    q->rxSnr=rxSnr_;q->hasRxSnr=haveSnr_;q->lateWindow=false;
    ++stats.accepted;++routingStats.opaque;return true;
  }
  bool admitDecoded(const uint8_t *p,size_t n,const RouteMetadata &m,uint32_t now,uint32_t jitter) {
    const uint8_t hop=p[12]&7,start=p[12]>>5,next=start?p[14]:0,relay=start?p[15]:0;
    const Key key=keyOf(p);
    auto *h=findHistory(key);auto *q=findQueue(key);
    if((h&&!contextMatches(*h,p,m))||(q&&(q->bytes[13]!=p[13]||q->channelContext!=m.channelContext))){++stats.mutableCopies;++stats.duplicates;return false;}
    const bool seen=h!=nullptr,upgrade=seen && hop>h->best;
    const bool weWereNext=seen && h->originalNext==relay_;
    const bool fallback=seen && h->originalNext && h->originalNext!=relay_ && !next &&
      wasRelayer(*h,relay) && !wasRelayer(*h,relay_) && !wasRelayer(*h,h->originalNext);
    const bool repeated=start && start==hop;
    if(h && h->to!=le32(p)) {++stats.mutableCopies;++stats.duplicates;return false;}
    if(h && h->payload!=fingerprint(p+16,n-16) && !repeated &&
       !(m.signatureLength==64 && m.signatureVerified)) {
      // An overheard copy keeps its original encrypted body. A different body
      // under the same (from,id) is not an implicit ACK or a trusted upgrade.
      // Originator retransmissions and locally verified renewed signatures
      // remain eligible under the bounded retry/better-hop rule.
      ++stats.payloadVariants;++stats.duplicates;return false;
    }
    mt::PacketLifetime::Bounds lifetime{};
    if(!seen&&!decodedLifetime(m.encodedDataSize?size_t(m.encodedDataSize)+16:n,lifetime)) {
      rememberNode(p,m,now);controlResponse(p,m,now);++routingStats.noTiming;return false;
    }
    if(!h) for(auto &s:history_) if(!s.used) {h=&s;break;}
    if(!h) {++stats.full;++routingStats.historyFull;return false;}
    if(!seen) {
      *h=History{};h->used=true;h->key=key;h->to=le32(p);h->originalNext=next;h->channel=p[13];h->channelContext=m.channelContext;
      h->at=now;h->best=hop;h->payload=fingerprint(p+16,n-16);
      h->wireSize=m.encodedDataSize ? uint16_t(m.encodedDataSize+16) : uint16_t(n);
      h->queueLifetimeMs=lifetime.queueMs;h->historyLifetimeMs=lifetime.historyMs;
      h->historyExpires=now+lifetime.historyMs;h->lifecycleExpires=now+lifetime.pendingMs;
    } else {
      lastSeen=true;
      if(h->payload!=fingerprint(p+16,n-16)) ++stats.payloadVariants;
      observeRelayer(*h,hop,relay);
      if(hop>h->best) h->best=hop;
    }
    h->at=now;
    if(seen && !upgrade) {
      // NextHopRouter stops hop-level retransmissions on a LoRa duplicate.
      // Opaque traffic deliberately never takes this path.
      if(findPending(key)) {
        const bool preserveRouterDuplicate=m.routerLateRole ||
          (m.clientBaseRole&&m.clientBaseFavoriteTraffic);
        stopPending(key,!alwaysRelay_&&!preserveRouterDuplicate&&h->attempts>1);
        ++routingStats.implicitAcks;
      }
      if(q || (!fallback && !repeated)) {
        if(q&&m.routerLateRole&&!repeated) {
          clampLateDuplicate(q,now);
        } else if(q&&m.clientBaseRole&&m.clientBaseFavoriteTraffic) {
          clampLateDuplicate(q,now);
        } else if(q && q->used && !m.mqttIngress && !alwaysRelay_ && !weWereNext && !repeated) {
          q->used=false;++routingStats.queueCancelled;++routingStats.duplicateRelayCancelled;
        }
        ++stats.duplicates;return false;
      }
      if(h->attempts>=MAX_ATTEMPTS || (h->attempts && uint32_t(now-h->lastAttempt)<1000)) {
        ++routingStats.attemptLimit;++stats.duplicates;return false;
      }
      ++stats.retries;if(fallback) ++routingStats.fallbacks;
    }
    if(!seen || upgrade || fallback || repeated) {
      rememberNode(p,m,now);
      controlResponse(p,m,now);
    }
    // A valid zero-hop control response can cancel/learn but is never relayed.
    if(!hop) {++stats.exhausted;return false;}
    if(next && next!=relay_) {++stats.unsupported;return false;}
    if(h->attempts>=MAX_ATTEMPTS) {++routingStats.attemptLimit;return false;}
    if(!q) q=freeQueue();
    if(!q) {++stats.full;return false;}
    const uint32_t oldDue=q->dueAt,oldExpiry=q->expires;const bool replace=q->used;
    const int8_t oldSnr=q->rxSnr;const bool oldHasSnr=q->hasRxSnr,oldLateWindow=q->lateWindow;
    *q=Frame{};q->used=true;q->len=uint16_t(n);q->channelContext=m.channelContext;memcpy(q->bytes,p,n);
    markQueueKind(q,true);
    q->bytes[12]=uint8_t((p[12]&0xf8)|(hop-((m.preserveHopLimit&&start&&hop<start)?0:1)));
    if(le32(p)==0xffffffffu) q->bytes[12]&=uint8_t(~8u);
    // Flooded ingress remains flooding, even if we have a route. Only a packet
    // explicitly naming this relay invokes NextHopRouter::sendWithNextHop.
    q->bytes[14]=next ? nextHop(le32(p),now,p[15]) : 0;q->bytes[15]=relay_;
    q->attempts=h->attempts;q->dueAt=replace?oldDue:now+initialDelay(jitter);
    q->rxSnr=replace?oldSnr:rxSnr_;q->hasRxSnr=replace?oldHasSnr:haveSnr_;
    q->lateWindow=replace&&oldLateWindow;
    q->expires=replace?oldExpiry:now+h->queueLifetimeMs;
    if(due(q->expires,h->lifecycleExpires)) q->expires=h->lifecycleExpires;
    h->payload=fingerprint(p+16,n-16);
    h->wireSize=m.encodedDataSize ? uint16_t(m.encodedDataSize+16) : uint16_t(n);
    h->cancelled=false;
    if(upgrade) ++stats.upgrades;
    if(q->bytes[14]) ++routingStats.directed;
    ++stats.accepted;++routingStats.decoded;return true;
  }
  uint32_t retransmissionDelay(const History &h) {
    if(!airtime_) {++routingStats.noTiming;return 0;}
    const uint32_t air=airtime_(h.wireSize,airtimeContext_);
    mt::RadioTiming::Deadline timing{};
    if(!air || (dynamicLifetimes_?!mt::RadioTiming::resolve(air,timing):air>4000)) {++routingStats.noTiming;return 0;}
    const uint8_t cw=uint8_t(3+(uint32_t(utilization_)*5)/100);
    // RadioInterface::getRetransmissionMsec: CWmin=3, CWmax=8,
    // PROCESSING_TIME_MSEC=4500. Wire size (including unknown fields) gives a
    // conservative upper bound versus nanopb canonical decoded re-encoding.
    return 2*air+((1u<<cw)+16+(1u<<5))*slotMs_+processingMs_;
  }
  bool decodedFrame(const Frame &q,const History *h) {
    if(!h||!contextMatches(*h,q))return false;
    for(unsigned i=0;i<QUEUE_SIZE;++i) if(&q==&queue_[i]) return queueDecoded_[i];
    if(activeKind_.valid && q.channelContext==activeKind_.channelContext&&q.len==activeKind_.len && same(keyOf(q.bytes),activeKind_.key) &&
       fingerprint(q.bytes,q.len)==activeKind_.digest) return activeKind_.decoded;
    if(!h || !channelAllowed(q.bytes[13]) || (q.bytes[13]==0 && le32(q.bytes)!=0xffffffffu&&!q.channelContext) ||
       h->to!=le32(q.bytes) || h->payload!=fingerprint(q.bytes+16,q.len-16)) return false;
    auto *o=findOpaque(keyOf(q.bytes));
    // Ambiguous same-header admissions are quarantined as opaque. Configuration
    // changes clear both histories, so ordinary operation has a stable decoder.
    return !o || o->channel!=q.bytes[13] || o->to!=le32(q.bytes);
  }
  void accountAttempt(const Frame &q,uint32_t now) {
    auto *o=findOpaque(keyOf(q.bytes));auto *h=findHistory(keyOf(q.bytes));
    const bool decoded=decodedFrame(q,h);
    uint8_t count=q.attempts;
    if(!decoded && o) {
      if(!count) count=o->attempts==UINT8_MAX?UINT8_MAX:uint8_t(o->attempts+1);
      if(count>o->attempts)o->attempts=count;
      o->lastAttempt=now;
    }
    if(decoded && h) {if(!count) count=uint8_t(h->attempts+1);if(count>h->attempts) h->attempts=count;h->lastAttempt=now;}
  }
public:
  Counters stats;
  RoutingCounters routingStats;
  // T096 retains the observation length and aggregate variant counter only.
  // The byte offset/mask fields are populated by profiles retaining full bodies.
#if defined(CONFIG_BOARD_HELTEC_T096)
  static constexpr bool HasOpaquePayloadDiff = false;
#else
  static constexpr bool HasOpaquePayloadDiff = true;
#endif
  uint16_t lastPayloadDifferences=0,lastPayloadBaselineLength=0;
  uint8_t lastPayloadDiffMask[30]{},lastPayloadFirstDifference=255,lastPayloadLastDifference=255;
  uint8_t lastPreviousFlags=0;bool lastSeen=false;
  void clearObservation() {
    lastPayloadDifferences=lastPayloadBaselineLength=0;lastPreviousFlags=0;lastSeen=false;
    memset(lastPayloadDiffMask,0,sizeof(lastPayloadDiffMask));
    lastPayloadFirstDifference=lastPayloadLastDifference=255;
  }
  void clearPending() {
    for(auto &q:queue_) q=Frame{};
    for(auto &p:pending_) p=Pending{};
    for(auto &kind:queueDecoded_) kind=false;
    activeKind_=ActiveKind{};
  }
  void clearRoutingState() {
    clearPending();
    for(auto &s:opaque_) s=Opaque{};
    for(auto &s:history_) s=History{};
    for(auto &n:known_) n=Known{};
    for(auto &r:routes_) r=Route{};
    for(auto &o:origins_) o=OriginHistory{};
    clearObservation();
  }
  void reset() {clearRoutingState();ready_=false;continuous_=false;identity_=0;explicitChannelSet_=false;channelHashCount_=0;}
  bool setChannelHashes(const uint8_t *hashes,unsigned count) {
    if(count>8||(count&&!hashes))return false;
    // A complete configured set has explicit presence; byte zero is a hash,
    // not an absent channel or an implicit private-message classification.
    clearRoutingState();memset(channelHashes_,0,sizeof(channelHashes_));channelHashCount_=count;explicitChannelSet_=true;
    if(count)memcpy(channelHashes_,hashes,count);
    return true;
  }
  void configure(uint8_t nodeByte,uint8_t channelHash) {
    reset();channelHashCount_=0;memset(channelHashes_,0,sizeof(channelHashes_));relay_=nodeByte;channel_=channelHash;ready_=nodeByte!=0;
  }
  void configureContinuous(uint8_t nodeByte,uint8_t channelHash) {
    if(nodeByte!=relay_ || channelHash!=channel_) clearRoutingState();else clearPending();
    continuous_=true;relay_=nodeByte;channel_=channelHash;
    ready_=nodeByte!=0 && (!identity_ || uint8_t(identity_)==nodeByte);
  }
  void configureIdentity(uint32_t node) {
    if(node!=identity_) clearRoutingState();
    identity_=node;
    ready_=relay_ && (!node || (node>1 && node!=0xffffffffu && uint8_t(node)==relay_));
  }
  void setAlwaysRelay(bool on) {alwaysRelay_=on;}
  void setOtherOpaqueChannels(bool on) {otherOpaque_=on;}
  bool alwaysRelay() const {return alwaysRelay_;}
  uint32_t identity() const {return identity_;}
  bool enableDynamicLifetimes() {
    if (dynamicLifetimes_) return false;
    if (airtime_ == nullptr) return false;
    if (activeKind_.valid) return false;
    for (unsigned i = 0; i < OPAQUE_SIZE; ++i) if (opaque_[i].used) return false;
    for (unsigned i = 0; i < HISTORY_SIZE; ++i) if (history_[i].used) return false;
    for (unsigned i = 0; i < QUEUE_SIZE; ++i) if (queue_[i].used) return false;
    for (unsigned i = 0; i < PENDING_SIZE; ++i) if (pending_[i].used) return false;
    dynamicLifetimes_ = true;
    return true;
  }
  bool configureSlotTime(uint32_t milliseconds) {
    if(!dynamicLifetimes_||!milliseconds||milliseconds>mt::RadioSlot::MaxSlotMs||activeKind_.valid)return false;
    for(const auto &row:opaque_)if(row.used)return false;
    for(const auto &row:history_)if(row.used)return false;
    for(const auto &row:queue_)if(row.used)return false;
    for(const auto &row:pending_)if(row.used)return false;
    slotMs_=milliseconds;return true;
  }
  void setRouteHintStore(void *context,ReadHint read,WriteHint write){
    hintContext_=context;readHint_=read;writeHint_=write;
  }
  void forgetRoute(uint32_t id){
    if(auto *route=findRoute(id))*route={};
    for(auto &node:known_)if(node.used&&node.node==id)node={};
  }
  void setAirtimeProvider(AirtimeProvider provider,void *context=nullptr) {airtime_=provider;airtimeContext_=context;}
  void configureAirtime(AirtimeProvider provider,void *context=nullptr) {setAirtimeProvider(provider,context);}
  void observeUtilization(uint8_t percent){utilization_=percent<=100?percent:100;}
  void setRxSnr(int8_t snr) {rxSnr_=snr<-20?-20:snr>10?10:snr;haveSnr_=true;}
  void configureTiming(uint32_t slotMs,uint8_t utilPercent,uint32_t processingMs=4500) {
    slotMs_=slotMs && slotMs<=1000?slotMs:28;
    utilization_=utilPercent<=100?utilPercent:100;
    processingMs_=processingMs<=60000?processingMs:4500;
  }
  void noteBlockedAirtime(uint32_t milliseconds) {
    const uint32_t maximum=dynamicLifetimes_?263393u:60000u;
    if(milliseconds>maximum)milliseconds=maximum;
    for(auto &p:pending_) if(p.used&&!p.queued) {
      p.nextAt+=milliseconds;
      if(dynamicLifetimes_&&due(p.nextAt,p.lifecycleExpires))p.nextAt=p.lifecycleExpires;
    }
    routingStats.blockedAirtime+=milliseconds;
  }
  size_t pending() const {size_t n=0;for(const auto &q:queue_) n+=q.used;return n;}
  size_t retransmitting() const {size_t n=0;for(const auto &p:pending_) n+=p.used;return n;}
  size_t knownCount() const {size_t n=0;for(const auto &p:known_) n+=p.used;return n;}
  size_t routeCount() const {size_t n=0;for(const auto &p:routes_) n+=p.used && p.hop;return n;}
  bool knownObservation(unsigned index,uint32_t now,uint32_t &node,uint32_t &age,bool &direct) const {
    for(const auto &p:known_)if(p.used && uint32_t(now-p.at)<KNOWN_TTL) {
      if(index--==0){node=p.node;age=uint32_t(now-p.at)/1000;direct=p.direct && uint32_t(now-p.directAt)<KNOWN_TTL;return true;}
    }
    return false;
  }
  bool routeObservation(unsigned index,uint32_t now,uint32_t &to,uint32_t &age,uint8_t &hop) const {
    for(const auto &p:routes_)if(p.used && p.hop && p.failures<3 && uint32_t(now-p.at)<ROUTE_TTL) {
      if(index--==0){to=p.to;age=uint32_t(now-p.at)/1000;hop=p.hop;return true;}
    }
    return false;
  }
  size_t opaqueCount() const {size_t n=0;for(const auto &p:opaque_) n+=p.used;return n;}
  size_t historyCount() const {size_t n=0;for(const auto &p:history_) n+=p.used;return n;}
  bool hasHistory(uint32_t from,uint32_t id) {return findHistory(Key{from,id})!=nullptr;}
  bool hasOpaqueHistory(uint32_t from,uint32_t id) {return findOpaque(Key{from,id})!=nullptr;}
  bool hasQueued(uint32_t from,uint32_t id) {return findQueue(Key{from,id})!=nullptr;}
  bool hasActiveTransmission() const {return activeKind_.valid;}
  uint8_t historyAttempts(uint32_t from,uint32_t id) {
    auto *h=findHistory(Key{from,id});return h?h->attempts:0;
  }
  // Trace-only queued upgrade. Service has already proved exact canonical Data
  // and an immutable semantic association for this (source,id). This narrow
  // method deliberately bypasses only the generic ciphertext-equality check;
  // it neither learns nodes/routes nor interprets response correlation.
  bool upgradeTraceVariant(const uint8_t *p,size_t n,const RouteMetadata &m,uint32_t now) {
    expire(now);clearObservation();
    if(!ready_||!p||n<17||n>255||le32(p+4)<=1||le32(p+4)>=0xfffffffeu||
       le32(p)<=1||le32(p)>=0xfffffffeu||!le32(p+8)||!channelAllowed(p[13])||
       (p[12]&0x18)||!m.channelDecoded||m.backendFailed||m.decodeRejected||
       m.portnum!=70||!m.traceRouteValid||!m.hasBitfield||m.bitfield||m.reply_id||
       m.emoji||m.signatureLength||m.encodedDataSize>239)return false;
    const uint8_t hop=p[12]&7,start=p[12]>>5,next=start?p[14]:0;
    const Key key=keyOf(p);auto *h=findHistory(key);auto *q=findQueue(key);
    if(!hop||hop>start||start>7||!h||!q||!contextMatches(*h,p,m)||!decodedFrame(*q,h)||h->to!=le32(p)||
       hop<=h->best||(next&&next!=relay_)||h->attempts>=MAX_ATTEMPTS)return false;
    const uint32_t dueAt=q->dueAt,expires=q->expires;const uint8_t attempts=q->attempts;
    *q=Frame{};q->used=true;q->len=uint16_t(n);q->channelContext=m.channelContext;memcpy(q->bytes,p,n);markQueueKind(q,true);
    q->bytes[12]=uint8_t((p[12]&0xf8)|(hop-1));q->bytes[14]=next?nextHop(le32(p),now,p[15]):0;
    q->bytes[15]=relay_;q->attempts=attempts;q->dueAt=dueAt;q->expires=expires;
    h->at=now;h->best=hop;h->payload=fingerprint(p+16,n-16);
    h->wireSize=m.encodedDataSize?uint16_t(m.encodedDataSize+16):uint16_t(n);
    ++stats.upgrades;++stats.accepted;++routingStats.decoded;
    if(q->bytes[14])++routingStats.directed;
    return true;
  }
  uint8_t nextHop(uint32_t to,uint32_t now,uint8_t relaySource=0) {
    if(to>=0xfffffffeu)return 0;
    auto *r=findRoute(to);
    uint8_t hop=r?r->hop:(readHint_?readHint_(hintContext_,to):0);
    if(r&&(uint32_t(now-r->at)>=ROUTE_TTL||r->failures>=3)){
      r->hop=0;storeHint(to,0);++routingStats.routeStale;return 0;
    }
    // A restored hint has no RAM health entry, as in stock. Even then the
    // byte still has to resolve to one currently reachable direct neighbor.
    return hop&&hop!=relaySource&&resolveByte(hop,true,now)?hop:0;
  }
  bool hasRetransmission(uint32_t from,uint32_t id) {return findPending(Key{from,id})!=nullptr;}
  uint32_t retransmissionDeadline(uint32_t from,uint32_t id) {
    auto *p=findPending(Key{from,id});return p?p->nextAt:0;
  }
  void expire(uint32_t now) {
    for(auto &q:queue_) if(q.used && due(now,q.expires)) {
      auto *p=findPending(keyOf(q.bytes));
      if(p && decodedFrame(q,findHistory(keyOf(q.bytes)))) {p->used=false;++routingStats.pendingExpired;}
      q.used=false;++stats.expired;
    }
    for(auto &o:opaque_) if(o.used && uint32_t(now-o.at)>=OPAQUE_TTL) o.used=false;
    for(auto &h:history_) if(h.used && (dynamicLifetimes_?due(now,h.historyExpires):uint32_t(now-h.at)>=HISTORY_TTL)) h.used=false;
    for(auto &n:known_) if(n.used && uint32_t(now-n.at)>=KNOWN_TTL) n.used=false;
    for(auto &r:routes_) if(r.used && uint32_t(now-r.at)>=ROUTE_TTL){storeHint(r.to,0);r.used=false;}
    for(auto &p:pending_) if(p.used) {
      if(due(now,p.lifecycleExpires)) {p.used=false;++routingStats.pendingExpired;continue;}
      if(p.queued || !due(now,p.nextAt)) continue;
      auto *h=findHistory(keyOf(p.frame.bytes));
      if(!h || !contextMatches(*h,p.frame) || h->cancelled || h->attempts>=MAX_ATTEMPTS) {p.used=false;continue;}
      auto *q=freeQueue();
      if(!q) {if(!p.queueBlocked) ++routingStats.pendingFull;p.queueBlocked=true;continue;}
      *q=p.frame;q->used=true;q->attempts=h->attempts;q->dueAt=now;q->expires=now+h->queueLifetimeMs;
      markQueueKind(q,true);
      if(due(q->expires,p.lifecycleExpires)) q->expires=p.lifecycleExpires;
      if(h->attempts==MAX_ATTEMPTS-1) {
        // Exact intermediate last retry falls back to flooding. No NAK is
        // manufactured: the source/destination retain delivery semantics.
        q->bytes[14]=0;routeFailure(le32(q->bytes));++routingStats.fallbacks;
      } else q->bytes[14]=nextHop(le32(q->bytes),now);
      p.queued=true;p.queueBlocked=false;++stats.retries;
    }
  }
  bool ingest(Protocol protocol,const uint8_t *p,size_t n,uint32_t now,uint32_t jitter) {
    return ingestWithMetadata(protocol,p,n,RouteMetadata{},now,jitter);
  }
  bool ingestWithMetadata(Protocol protocol,const uint8_t *p,size_t n,const RouteMetadata &m,uint32_t now,uint32_t jitter) {
    expire(now);clearObservation();
    if(protocol!=Protocol::Meshtastic || !ready_) {++stats.unsupported;return false;}
    if(!p || n<17 || n>255 || !le32(p+4) || le32(p+4)==0xffffffffu) {++stats.malformed;return false;}
    const uint8_t hop=p[12]&7,start=p[12]>>5;
    if(start && hop>start) {++stats.malformed;return false;}
    const uint32_t to=le32(p),from=le32(p+4);
    if(!le32(p+8) || to<=1 || (identity_ && (from==identity_ || to==identity_))) {
      ++stats.unsupported;return false;
    }
    if(!channelAllowed(p[13]) && !(p[13]==0 && to!=0xffffffffu) && !otherOpaque_) {
      ++stats.unsupported;return false;
    }
    // A local crypto backend failure is a device/software failure, not evidence
    // of an unknown channel. Do not convert it into a routing decision.
    if(m.backendFailed) {++stats.malformed;++routingStats.cryptoFailure;return false;}
    if(m.channelAmbiguous) {++stats.malformed;++routingStats.decodeRejected;return false;}
    if(m.signatureRejected) {++stats.malformed;++routingStats.signatureRejected;return false;}
    // Router::passesRoutingAuthGate permits decode-failed TRANSIT frames as
    // opaque: an 8-bit channel hash can collide. Outer self/from/hop validation
    // has already run, and this isolated path cannot learn routes/cancel ARQ.
    if(m.decodeRejected) {
      ++routingStats.decodeRejected;++routingStats.decodeOpaque;
      return admitOpaque(p,n,m,now,jitter);
    }
    if(m.channelDecoded) {
      if(!channelAllowed(p[13]) || (p[13]==0 && to!=0xffffffffu&&!m.channelContext) || !m.portnum || m.encodedDataSize>239) {
        ++stats.malformed;return false;
      }
      // Exact upstream malformed-signature guard. A valid-length signature is
      // not proof: without authoritative public-key/XEdDSA verification, relay
      // it only opaquely instead of allowing unsigned local routing evidence.
      if(m.signatureLength) {
        if(m.signatureLength!=64) {++stats.malformed;++routingStats.signatureRejected;return false;}
        if(!m.signatureVerified) {++routingStats.signatureOpaque;return admitOpaque(p,n,m,now,jitter);}
        ++routingStats.signatureVerified;
      }
      return admitDecoded(p,n,m,now,jitter);
    }
    return admitOpaque(p,n,m,now,jitter);
  }
  Frame *next(uint32_t now) {expire(now);for(auto &q:queue_) if(q.used && due(now,q.dueAt)) return &q;return nullptr;}
  void noteTransmitStart(const Frame &q) {
    if(q.len<17 || q.len>255) {activeKind_=ActiveKind{};return;}
    const bool decoded=decodedFrame(q,findHistory(keyOf(q.bytes)));
    activeKind_.key=keyOf(q.bytes);activeKind_.digest=fingerprint(q.bytes,q.len);
    activeKind_.len=q.len;activeKind_.channelContext=q.channelContext;activeKind_.decoded=decoded;activeKind_.valid=true;
  }
  void defer(Frame &q,uint32_t now,uint32_t jitter) {q.dueAt=now+100+jitter%600;}
  bool retry(const Frame &frame,uint32_t now) {
    if(frame.len<17 || frame.len>255) {++stats.failures;return false;}
    const bool decoded=decodedFrame(frame,findHistory(keyOf(frame.bytes)));
    accountAttempt(frame,now);
    if(due(now,frame.expires) || frame.attempts>=MAX_ATTEMPTS) {++routingStats.attemptLimit;return false;}
    auto *q=freeQueue();if(!q) {++stats.full;return false;}
    *q=frame;q->used=true;q->dueAt=now+500;markQueueKind(q,decoded);
    activeKind_.valid=false;return true;
  }
  // retry() has already accounted this native send attempt. If its bounded
  // attempt/expiry/capacity policy refuses a new queue slot after the caller
  // has independently restored a healthy radio, retire only this packet. Do
  // not account the same attempt again through complete(), and do not retain a
  // stale active or retransmission classification.
  void retireRetryRefusal(Frame &frame) {
    const Key key=frame.len>=12?keyOf(frame.bytes):Key{};
    activeKind_=ActiveKind{};frame.used=false;
    if(frame.len>=17&&frame.len<=255)if(auto *pending=findPending(key))if(pending->frame.bytes[13]==frame.bytes[13]&&pending->frame.channelContext==frame.channelContext)pending->used=false;
    ++stats.failures;
  }
  void complete(Frame &q,uint32_t now,bool success) {
    if(q.len<17 || q.len>255) {q.used=false;++stats.failures;return;}
    const Key key=keyOf(q.bytes);accountAttempt(q,now);
    auto *h=findHistory(key);
    const bool decoded=decodedFrame(q,h);
    if(activeKind_.valid && q.channelContext==activeKind_.channelContext&&same(key,activeKind_.key) && q.len==activeKind_.len &&
       fingerprint(q.bytes,q.len)==activeKind_.digest) activeKind_.valid=false;
    q.used=false;
    if(success) ++stats.sent;else ++stats.failures;
    auto *pending=findPending(key);
    if(!success) {if(decoded && pending) pending->used=false;return;}
    if(!decoded || !h) return; // Opaque copies never enter ACK/history/route state.
    h->sent=true;h->ourTxHop=q.bytes[12]&7;addRelayer(*h,relay_);
    if(h->cancelled || h->attempts>=MAX_ATTEMPTS || !q.bytes[14] ||
       (!(q.bytes[12]&7) && !(q.bytes[12]&8))) {
      if(pending) pending->used=false;
      return;
    }
    const uint32_t delay=retransmissionDelay(*h);if(!delay) return;
    if(!pending) for(auto &p:pending_) if(!p.used) {pending=&p;break;}
    if(!pending) {++routingStats.pendingFull;return;}
    *pending=Pending{};pending->used=true;pending->frame=q;pending->frame.used=true;
    pending->frame.attempts=h->attempts;pending->nextAt=now+delay;pending->delay=delay;
    pending->lifecycleExpires=h->lifecycleExpires;
  }
};
}
