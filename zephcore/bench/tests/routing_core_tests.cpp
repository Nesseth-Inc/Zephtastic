// SPDX-License-Identifier: GPL-3.0-only
#include "../../adapters/bench/vendor/RoutingCore.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace bench;
static constexpr uint32_t SELF=0x1234567c,A=0x111111a1,B=0x222222b2,R=0x333333c3;
static unsigned cases=0;
static void put(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;++i) p[i]=uint8_t(n>>(8*i));}
static void packet(uint8_t p[255],uint32_t from,uint32_t to,uint32_t id,
                   uint8_t hop=3,uint8_t start=3,uint8_t next=0,uint8_t relay=0,uint8_t channel=8) {
  memset(p,0,16);for(unsigned i=16;i<255;++i) p[i]=uint8_t(i*7+id);
  put(p,to);put(p+4,from);put(p+8,id);p[12]=uint8_t(start<<5)|hop|8;
  p[13]=channel;p[14]=next;p[15]=relay?relay:uint8_t(from);
}
static RouteMetadata data(uint32_t port=1) {
  RouteMetadata m;m.channelDecoded=true;m.portnum=port;m.encodedDataSize=32;return m;
}
static uint32_t air(size_t,void *) {return 1000;}
static void setup(RoutingCore &c) {
  c.configureContinuous(uint8_t(SELF),8);c.configureIdentity(SELF);
  c.configureAirtime(air);c.configureTiming(28,100,4500);
}
static Frame take(RoutingCore &c,uint32_t now) {
  auto *q=c.next(now);
  if(!q) {
    std::fprintf(stderr,
      "FAIL routing-core-take case=%u now=%u pending=%zu retransmitting=%zu active=%u sent=%u retries=%u expired=%u pending_expired=%u pending_full=%u\n",
      cases,now,c.pending(),c.retransmitting(),unsigned(c.hasActiveTransmission()),c.stats.sent,
      c.stats.retries,c.stats.expired,c.routingStats.pendingExpired,c.routingStats.pendingFull);
    std::fflush(stderr);
  }
  assert(q);c.noteTransmitStart(*q);Frame result=*q;q->used=false;
  ++result.attempts;return result;
}
static Frame send(RoutingCore &c,uint32_t now,bool ok=true) {
  Frame result=take(c,now);c.complete(result,now+10,ok);return result;
}
static void known(RoutingCore &c,uint32_t node,uint32_t id,uint32_t now) {
  uint8_t p[255];packet(p,node,0xffffffffu,id);auto m=data(4);m.nodeInfoValid=true;
  assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,now,0));send(c,now+150);
}
static void learn(RoutingCore &c,uint32_t now=1000) {
  known(c,B,0xb001,now);known(c,R,0xc001,now+200);
  uint8_t p[255];packet(p,A,B,0xa001);auto m=data();
  assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,now+400,0));
  auto tx=send(c,now+550);assert(tx.bytes[14]==0 && (tx.bytes[12]&7)==2);
  packet(p,A,B,0xa001,1,3,0,uint8_t(R));
  assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,now+600,0));
  packet(p,B,A,0xb002,2,3,0,uint8_t(R));m=data(5);m.request_id=0xa001;
  m.routingPresent=m.routingErrorPresent=true;
  assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,now+700,0));
  assert(c.nextHop(B,now+701)==uint8_t(R));send(c,now+850);
}
static void directed(RoutingCore &c,uint32_t id,uint32_t now) {
  uint8_t p[255];packet(p,A,B,id,3,3,uint8_t(SELF));auto m=data();
  assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,now,0));
}
int main() {
  uint8_t p[255];
  {
    RoutingCore c;setup(c);packet(p,A,B,1,3,3,0,0,0);
    const uint8_t originalFlags=p[12];
    assert(c.ingest(Protocol::Meshtastic,p,255,100,0));auto tx=send(c,250);
    assert(!memcmp(tx.bytes+16,p+16,239));assert(tx.bytes[12]==uint8_t(originalFlags-1));
    assert(tx.bytes[14]==0 && tx.bytes[15]==uint8_t(SELF));
    assert(le32(tx.bytes)==B && le32(tx.bytes+4)==A && le32(tx.bytes+8)==1);
    assert(!c.historyCount() && !c.knownCount() && !c.routeCount() && !c.retransmitting());++cases;
  }
  {
    RoutingCore c;setup(c);packet(p,A,B,2,3,3,uint8_t(SELF),0,0);
    assert(c.ingest(Protocol::Meshtastic,p,48,100,0));auto tx=send(c,250);
    assert(tx.bytes[14]==uint8_t(SELF));assert((tx.bytes[12]&8) && !c.retransmitting());
    packet(p,A,B,3,3,3,0xc3,0,0);assert(!c.ingest(Protocol::Meshtastic,p,48,300,0));++cases;
  }
  {
    RoutingCore c;setup(c);packet(p,A,0xffffffffu,3);assert(c.ingest(Protocol::Meshtastic,p,48,100,0));
    auto tx=send(c,250);assert(!(tx.bytes[12]&8));
    packet(p,A,B,4,2,0,0xfe,0xee);assert(c.ingest(Protocol::Meshtastic,p,48,300,0));
    tx=send(c,450);assert(tx.bytes[14]==0 && (tx.bytes[12]&7)==1 && (tx.bytes[12]&8));++cases;
  }
  {
    RoutingCore c;setup(c);packet(p,A,B,9);
    assert(!c.ingest(Protocol::MeshCore,p,48,100,0));assert(!c.pending());
    assert(!c.ingest(Protocol::Meshtastic,nullptr,48,100,0));
    assert(!c.ingest(Protocol::Meshtastic,p,16,100,0));assert(!c.ingest(Protocol::Meshtastic,p,256,100,0));
    p[12]=(2<<5)|3;assert(!c.ingest(Protocol::Meshtastic,p,48,100,0));
    packet(p,0,B,9);assert(!c.ingest(Protocol::Meshtastic,p,48,100,0));
    packet(p,0xffffffffu,B,9);assert(!c.ingest(Protocol::Meshtastic,p,48,100,0));
    packet(p,A,0,9);assert(!c.ingest(Protocol::Meshtastic,p,48,100,0));
    packet(p,A,1,9);assert(!c.ingest(Protocol::Meshtastic,p,48,100,0));
    packet(p,A,B,0);assert(!c.ingest(Protocol::Meshtastic,p,48,100,0));
    packet(p,SELF,B,9);assert(!c.ingest(Protocol::Meshtastic,p,48,100,0));
    packet(p,A,SELF,9);assert(!c.ingest(Protocol::Meshtastic,p,48,100,0));++cases;
  }
  {
    RoutingCore c;setup(c);packet(p,A,0xffffffffu,1,3,3,0,0,0);
    assert(!c.ingest(Protocol::Meshtastic,p,48,100,0));
    packet(p,A,B,1,3,3,0,0,9);assert(!c.ingest(Protocol::Meshtastic,p,48,100,0));
    c.setOtherOpaqueChannels(true);assert(c.ingest(Protocol::Meshtastic,p,48,100,0));send(c,250);
    auto m=data();assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,300,0));
    assert(!c.historyCount());++cases;
  }
  {
    RoutingCore c;setup(c);packet(p,A,B,1,2,3,0,0,0);
    assert(c.ingest(Protocol::Meshtastic,p,48,100,0));send(c,250);
    assert(!c.ingest(Protocol::Meshtastic,p,48,500,0));
    p[12]=(3<<5)|3|8;assert(c.ingest(Protocol::Meshtastic,p,48,501,0));
    assert(!c.ingest(Protocol::Meshtastic,p,48,502,0));
    assert(!c.next(650));send(c,651);
    assert(c.ingest(Protocol::Meshtastic,p,48,1400,0));send(c,1550);
    p[20]^=1;assert(c.ingest(Protocol::Meshtastic,p,48,2700,0));
    assert(c.lastSeen && c.lastPayloadBaselineLength==32 && c.stats.payloadVariants==1);
    if constexpr(RoutingCore::HasOpaquePayloadDiff) {
      assert(c.lastPayloadDifferences==1 && c.lastPayloadFirstDifference==4 &&
             c.lastPayloadLastDifference==4 && c.lastPayloadDiffMask[0]==(1u<<4));
    } else {
      assert(c.lastPayloadDifferences==0 && c.lastPayloadFirstDifference==255);
    }
    const auto tx=send(c,2850);assert(!memcmp(tx.bytes+16,p+16,32));
    assert(!c.historyCount() && !c.knownCount() && !c.routeCount() && !c.retransmitting());
    assert(c.ingest(Protocol::Meshtastic,p,48,4000,0));send(c,4150);
    assert(c.stats.sent==5 && !c.routingStats.attemptLimit && c.stats.payloadVariants);
    assert(!c.historyCount()&&!c.knownCount()&&!c.retransmitting());++cases;
  }
  {
    RoutingCore c;setup(c);for(uint32_t i=1;i<=8;++i) {packet(p,A,B,i,3,3,0,0,0);assert(c.ingest(Protocol::Meshtastic,p,48,100,0));}
    packet(p,A,B,9,3,3,0,0,0);assert(!c.ingest(Protocol::Meshtastic,p,48,100,0));
    assert(c.pending()==8);c.expire(30100);assert(!c.pending() && c.stats.expired==8);
    assert(c.ingest(Protocol::Meshtastic,p,48,30101,0));c.clearPending();
    assert(!c.pending());assert(c.opaqueCount()==9);++cases;
  }
  {
    RoutingCore c;setup(c);for(uint32_t i=1;i<=64;++i) {
      packet(p,A,B,i,3,3,0,0,0);uint32_t at=100+i*200;
      assert(c.ingest(Protocol::Meshtastic,p,48,at,0));send(c,at+150);
    }
    packet(p,A,B,65,3,3,0,0,0);assert(!c.ingest(Protocol::Meshtastic,p,48,14000,0));
    assert(c.opaqueCount()==64 && c.routingStats.historyFull==1);
    c.expire(75000);assert(!c.opaqueCount());assert(c.ingest(Protocol::Meshtastic,p,48,75001,0));++cases;
  }
  {
    RoutingCore c;setup(c);packet(p,A,B,1,2,3,0,0,0);
    assert(c.ingest(Protocol::Meshtastic,p,48,100,0));send(c,250);
    c.configureContinuous(uint8_t(SELF),8);assert(!c.ingest(Protocol::Meshtastic,p,48,1000,0));
    assert(c.identity()==SELF);c.clearRoutingState();assert(!c.opaqueCount() && !c.historyCount());
    assert(c.ingest(Protocol::Meshtastic,p,48,1001,0));assert(c.identity()==SELF);++cases;
  }
  {
    RoutingCore c;setup(c);auto m=data();packet(p,A,B,10,1,3);
    assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,100,0));auto *q=c.next(250);assert(q && (q->bytes[12]&7)==0);
    p[12]=(3<<5)|2|8;assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,260,0));
    assert((q->bytes[12]&7)==1 && q->dueAt==250 && c.stats.upgrades==1);
    p[12]=(3<<5)|1|8;assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,270,0));
    assert(q->used && (q->bytes[12]&7)==1);send(c,300);
    packet(p,A,R,10,3,3);assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,1500,0));++cases;
  }
  {
    RoutingCore c;setup(c);c.setAlwaysRelay(false);auto m=data();packet(p,A,B,10,2,3);
    assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,100,0));p[12]=(3<<5)|1|8;p[15]=uint8_t(R);
    assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,110,0));assert(!c.pending() && c.routingStats.queueCancelled==1);++cases;
  }
  {
    RoutingCore c;setup(c);auto m=data();m.decodeRejected=true;packet(p,A,B,10);
    assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,100,0));send(c,250);
    assert(!c.historyCount()&&!c.knownCount()&&!c.retransmitting()&&c.routingStats.decodeOpaque==1);
    m.decodeRejected=false;m.encodedDataSize=240;assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,100,0));
    m.encodedDataSize=32;p[13]=0;assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,100,0));++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);assert(c.routeCount()==1 && c.knownCount()==2);
    packet(p,A,B,20);auto m=data();assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,3000,0));
    auto tx=send(c,3150);assert(tx.bytes[14]==0 && !c.retransmitting());
    directed(c,21,3300);tx=send(c,3450);
    assert(tx.bytes[14]==uint8_t(R) && (tx.bytes[12]&8));assert(c.hasRetransmission(A,21));++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);directed(c,22,3000);send(c,3150);
    uint32_t first=c.retransmissionDeadline(A,22);assert(first==3160+15012);
    assert(!c.next(first-1));auto *q=c.next(first);assert(q && q->bytes[14]==uint8_t(R) && q->attempts==1);
    auto tx=send(c,first);uint32_t last=c.retransmissionDeadline(A,22);
    assert(last==first+10+15012 && last>33000);q=c.next(last);
    assert(q && q->bytes[14]==0 && q->attempts==2 && q->expires>last);
    tx=send(c,last);assert(!c.retransmitting() && c.routingStats.fallbacks==1 && c.nextHop(B,last)==0);
    assert(tx.attempts==3 && c.stats.sent==7);++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);directed(c,23,3000);send(c,3150);
    const uint32_t initial=c.retransmissionDeadline(A,23);c.noteBlockedAirtime(1200);
    assert(c.retransmissionDeadline(A,23)==initial+1200 && !c.next(initial));
    assert(c.next(initial+1200));c.clearPending();assert(!c.retransmitting() && !c.pending());++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);directed(c,24,3000);send(c,3150);
    auto m=data();packet(p,A,B,24,1,3,uint8_t(R),uint8_t(R));
    assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,3200,0));
    assert(!c.retransmitting() && c.routingStats.implicitAcks==1 && !c.pending());++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);directed(c,25,3000);send(c,3150);
    auto m=data();packet(p,A,B,25,1,3,uint8_t(R),uint8_t(R));p[20]^=1;
    assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,3200,0));
    assert(c.hasRetransmission(A,25) && !c.routingStats.implicitAcks);
    packet(p,A,B,25,1,3,uint8_t(SELF),uint8_t(R),0);
    assert(c.ingest(Protocol::Meshtastic,p,48,3300,0));send(c,3450,false);
    assert(c.hasRetransmission(A,25) && c.routingStats.implicitAcks==0);++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);directed(c,250,3000);send(c,3150);
    packet(p,A,B,250,2,3,0,uint8_t(R));
    // An opaque admission with precisely the same sender/id/channel/body must
    // not consume decoded history attempts or disable its later trusted retry.
    assert(c.ingest(Protocol::Meshtastic,p,48,3300,0));send(c,3450,true);
    assert(c.hasRetransmission(A,250));
    uint32_t at=c.retransmissionDeadline(A,250);auto *q=c.next(at);
    assert(q && q->attempts==1 && q->bytes[14]==uint8_t(R));send(c,at);
    at=c.retransmissionDeadline(A,250);q=c.next(at);
    assert(q && q->attempts==2 && q->bytes[14]==0);send(c,at);
    assert(!c.hasRetransmission(A,250));++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);directed(c,26,3000);send(c,3150);
    auto m=data(5);m.request_id=26;m.routingPresent=m.routingErrorPresent=true;
    packet(p,B,A,30,2,3,0,uint8_t(R));
    assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,3300,0));
    assert(!c.hasRetransmission(A,26) && c.routingStats.responseCancels==2);
    auto tx=send(c,3450);assert(le32(tx.bytes+4)==B && le32(tx.bytes+8)==30 && (tx.bytes[12]&8));++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);directed(c,27,3000);send(c,3150);
    auto m=data(5);m.request_id=27;m.routingPresent=m.routingErrorPresent=true;m.routingError=6;
    packet(p,R,A,31,0,0);assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,3200,0));
    assert(!c.hasRetransmission(A,27) && c.routingStats.nakCancels==1 && !c.pending());++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);directed(c,28,3000);send(c,3150);
    auto m=data(5);m.request_id=28;m.routingPresent=m.routingErrorPresent=true;
    packet(p,R,A,32,0,0);assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,3200,0));
    assert(!c.hasRetransmission(A,28) && c.routingStats.responseCancels==2);++cases;
  }
  {
    RoutingCore c;setup(c);known(c,B,0xb001,1000);directed(c,29,1300);
    auto m=data(5);m.request_id=29;m.routingPresent=m.routingErrorPresent=true;
    packet(p,B,A,33,0,0);assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,1350,0));
    assert(!c.pending() && !c.routeCount() && c.routingStats.responseCancels==1);++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);directed(c,30,3000);send(c,3150);
    auto m=data(5);m.request_id=30;m.routingPresent=m.routingErrorPresent=true;
    const uint32_t learned=c.routingStats.routesLearned;
    packet(p,0x444444d4,A,34,0,0);assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,3200,0));
    assert(!c.hasRetransmission(A,30)&&c.routingStats.responseCancels==2&&c.routingStats.routesLearned==learned);
    directed(c,31,3210);send(c,3360);m.request_id=999;
    packet(p,B,A,35,0,0);assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,3400,0));
    assert(c.hasRetransmission(A,31)&&c.routingStats.responseRejected==1&&c.routingStats.routesLearned==learned);++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);directed(c,300,3000);send(c,3150);
    auto m=data();m.reply_id=300;packet(p,B,A,350,2,3,0,uint8_t(R));
    const uint32_t learned=c.routingStats.routesLearned,cancelled=c.routingStats.responseCancels;
    assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,3300,0));send(c,3450);
    assert(c.hasRetransmission(A,300) && c.routingStats.routesLearned==learned &&
           c.routingStats.responseCancels==cancelled);++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);known(c,0x555555c3,0xc002,3000);
    assert(c.nextHop(B,3300)==0 && c.routingStats.collisions);
    directed(c,40,3400);auto tx=send(c,3550);assert(!tx.bytes[14] && !c.retransmitting());++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);assert(c.nextHop(B,2000)==uint8_t(R));
    assert(c.nextHop(B,1700+RoutingCore::ROUTE_TTL)==0);c.expire(2000+RoutingCore::ROUTE_TTL);
    assert(!c.knownCount() && !c.routeCount());++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);directed(c,41,3000);send(c,3150);
    c.noteBlockedAirtime(60000);c.noteBlockedAirtime(60000);c.expire(93000);
    assert(!c.pending() && !c.retransmitting() && c.routingStats.pendingExpired);++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);c.configureAirtime(nullptr);directed(c,42,3000);send(c,3150);
    assert(!c.retransmitting() && c.routingStats.noTiming==1);Frame empty;c.complete(empty,4000,false);
    assert(c.stats.failures==1);++cases;
  }
  {
    RoutingCore c;setup(c);packet(p,A,B,43,3,3,0,0,0);
    assert(c.ingest(Protocol::Meshtastic,p,48,100,0));Frame active=take(c,250);
    assert(c.retry(active,260));active=take(c,760);assert(active.attempts==2);assert(c.retry(active,770));
    active=take(c,1270);assert(active.attempts==3);assert(!c.retry(active,1280));c.retireRetryRefusal(active);
    assert(!c.pending()&&!c.hasActiveTransmission());
    // Native retry refusal ends that burst, not later originator transmissions.
    assert(c.ingest(Protocol::Meshtastic,p,48,5000,0));
    assert(!c.ingest(Protocol::Meshtastic,p,48,5001,0)); // bounded queued-copy coalescing
    assert(!c.next(5149));const auto repeated=send(c,5150);
    assert(!memcmp(repeated.bytes+16,p+16,32));
    p[12]=uint8_t((3<<5)|2|8);assert(!c.ingest(Protocol::Meshtastic,p,48,5500,0));
    assert(!c.pending()&&!c.historyCount()&&!c.retransmitting()&&!c.routeCount()&&!c.knownCount());++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);for(uint32_t id=50;id<58;++id) {directed(c,id,3000+(id-50)*200);send(c,3150+(id-50)*200);}
    assert(c.retransmitting()==8);directed(c,58,4700);send(c,4850);
    assert(c.retransmitting()==8 && c.routingStats.pendingFull==1);c.clearRoutingState();
    assert(!c.retransmitting() && !c.pending() && !c.routeCount() && !c.knownCount() && !c.historyCount());++cases;
  }
  {
    RoutingCore c;setup(c);for(uint32_t i=1;i<=32;++i) known(c,0x11000000+i,i,100+i*200);
    assert(c.knownCount()==32);known(c,0x11000040,40,7000);assert(c.knownCount()==32 && c.routingStats.knownFull==1);++cases;
  }
  {
    RoutingCore c;setup(c);auto m=data();for(uint32_t i=1;i<=64;++i) {
      packet(p,A,B,i,3,3,0xff);assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,100+i,0));
    }
    assert(c.historyCount()==64);packet(p,A,B,65);assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,200,0));
    assert(!c.pending() && c.routingStats.historyFull==1);++cases;
  }
  {
    RoutingCore c;setup(c);c.setRxSnr(-20);packet(p,A,B,1,3,3,0,0,0);
    assert(c.ingest(Protocol::Meshtastic,p,48,100,5));auto *q=c.next(100+5*28);assert(q && q->dueAt==240);c.clearPending();
    c.clearRoutingState();c.setAlwaysRelay(false);assert(c.ingest(Protocol::Meshtastic,p,48,100,5));
    assert(!c.next(100+20*28));q=c.next(100+21*28);assert(q && q->dueAt==688);++cases;
  }
  {
    RoutingCore c;setup(c);packet(p,A,B,1,2,0,0,0,0);
    assert(c.ingest(Protocol::Meshtastic,p,48,0xffffff00u,0));send(c,0xffffff96u);
    assert(!c.ingest(Protocol::Meshtastic,p,48,100,0));
    assert(c.ingest(Protocol::Meshtastic,p,48,61000,0));++cases;
  }
  {
    RoutingCore c;setup(c);learn(c,0xfffff000u);directed(c,70,0xfffffc00u);send(c,0xfffffc96u);
    uint32_t deadline=c.retransmissionDeadline(A,70);assert(deadline<20000 && !c.next(deadline-1));
    assert(c.next(deadline));send(c,deadline);assert(c.retransmitting()==1);++cases;
  }
  {
    RoutingCore c;setup(c);unsigned rejected=0;
    for(uint32_t i=0;i<100000;++i) {
      packet(p,A,B,i+1);p[12]=(1<<5)|7;
      assert(!c.ingest(Protocol::Meshtastic,p,17+i%239,i,0));++rejected;
    }
    assert(!c.pending() && !c.opaqueCount() && !c.historyCount());assert(rejected==100000);++cases;
  }
  {
    for(unsigned signature=1;signature<64;++signature) {
      RoutingCore c;setup(c);auto m=data(4);m.nodeInfoValid=true;m.signatureLength=uint8_t(signature);
      packet(p,B,0xffffffffu,signature);
      assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,100,0));
      assert(c.routingStats.signatureRejected==1&&!c.historyCount()&&!c.opaqueCount()&&!c.knownCount()&&!c.pending());
      m.signatureLength=0;assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,101,0));
      assert(c.knownCount()==1); // rejected first contact cannot poison this ID
    }
    ++cases;
  }
  {
    RoutingCore c;setup(c);auto m=data(4);m.nodeInfoValid=true;m.signatureLength=64;
    packet(p,B,0xffffffffu,80);assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,100,0));
    auto tx=send(c,250);assert(!memcmp(tx.bytes+16,p+16,32));
    assert(c.routingStats.signatureOpaque==1&&!c.historyCount()&&!c.knownCount()&&!c.retransmitting());++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);directed(c,81,3000);send(c,3150);
    assert(c.retransmitting()==1);const uint32_t deadline=c.retransmissionDeadline(A,81);
    const uint32_t cancelled=c.routingStats.responseCancels;
    auto m=data(5);m.request_id=81;m.routingPresent=m.routingErrorPresent=true;m.signatureLength=64;
    packet(p,B,A,82,3,3,0,uint8_t(B));
    assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,3200,0));send(c,3350);
    assert(c.retransmitting()==1&&c.retransmissionDeadline(A,81)==deadline);
    assert(c.routingStats.responseCancels==cancelled&&!c.routingStats.nakCancels&&c.routingStats.signatureOpaque==1);++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);directed(c,83,3000);send(c,3150);
    auto m=data();m.signatureLength=64;packet(p,A,B,83,2,3,uint8_t(SELF),uint8_t(R));
    assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,3200,0));send(c,3350);
    assert(c.retransmitting()==1&&!c.routingStats.implicitAcks);++cases;
  }
  {
    RoutingCore c;setup(c);learn(c);directed(c,84,3000);send(c,3150);
    const uint32_t cancelled=c.routingStats.responseCancels;
    auto m=data(5);m.request_id=84;m.decodeRejected=true;packet(p,B,A,85);
    assert(c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,3200,0));send(c,3350);
    assert(c.retransmitting()==1&&c.routingStats.responseCancels==cancelled&&!c.routingStats.implicitAcks);++cases;
  }
  {
    RoutingCore c;setup(c);RouteMetadata m;m.decodeRejected=m.backendFailed=true;packet(p,A,B,86);
    assert(!c.ingestWithMetadata(Protocol::Meshtastic,p,48,m,100,0));
    assert(c.routingStats.cryptoFailure==1&&!c.pending()&&!c.opaqueCount()&&!c.historyCount());++cases;
  }
  std::printf("PASS %u routing core cases;100000 malformed packets;fixed memory=%zu bytes;opaque private transit,decoded provenance/routes,ACK/NAK cancellation,implicit echo isolation,3-attempt ARQ/fallback,half-duplex timing,bounds,stop/key reset,wrap,SNR,channel-collision fallback,signature quarantine,crypto backend failure\n",cases,sizeof(RoutingCore));
}
