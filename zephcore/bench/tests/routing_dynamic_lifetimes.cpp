// SPDX-License-Identifier: GPL-3.0-only
// Actual RoutingCore with synthetic admitted metadata; no crypto/radio/target proof.
#ifdef NDEBUG
#error "Routing lifetime checks require enabled assertions"
#endif
#include "RoutingCore.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <array>
#include <memory>
using bench::RoutingCore;using bench::Frame;using bench::RouteMetadata;using bench::Protocol;
namespace {
constexpr uint32_t Self=0x1234567c,A=0x111111a1,B=0x222222b2,R=0x333333c3,Air=120000;
constexpr uint32_t Slot=1320,Processing=4500;
unsigned groups=0;
void pass(const char *name){++groups;std::printf("PASS %s\n",name);}
void put(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
std::array<uint8_t,255> packet(uint32_t id,uint8_t hop=2,uint8_t start=3,uint8_t next=0,
                               uint32_t from=A,uint32_t to=B,uint8_t channel=8){
 std::array<uint8_t,255> p{};put(p.data(),to);put(p.data()+4,from);put(p.data()+8,id);
 p[12]=uint8_t((start<<5)|hop|8);p[13]=channel;p[14]=next;p[15]=uint8_t(from);
 for(size_t i=16;i<p.size();++i)p[i]=uint8_t(i*7+id);
 return p;
}
RouteMetadata data(unsigned port=1,size_t encoded=32){RouteMetadata m;m.channelDecoded=true;m.portnum=port;m.encodedDataSize=uint16_t(encoded);return m;}
struct Provider {
 uint32_t air=Air;unsigned calls=0;size_t length=0;
 static uint32_t get(size_t n,void *ctx){assert(ctx);auto &p=*static_cast<Provider *>(ctx);++p.calls;p.length=n;return p.air;}
};
struct Harness {
 Provider provider;RoutingCore core;
 explicit Harness(bool dynamic=true,uint32_t slot=Slot){
  core.configureContinuous(uint8_t(Self),8);core.configureIdentity(Self);
  core.configureAirtime(Provider::get,&provider);core.configureTiming(28,100,Processing);
  if(dynamic){assert(core.enableDynamicLifetimes());assert(core.configureSlotTime(slot));}
 }
};
bool ingest(RoutingCore &c,const std::array<uint8_t,255> &p,uint32_t now,const RouteMetadata &m=data(),size_t n=48){
 return c.ingestWithMetadata(Protocol::Meshtastic,p.data(),n,m,now,0);
}
Frame take(RoutingCore &c,uint32_t now){
 auto *q=c.next(now);assert(q);c.noteTransmitStart(*q);Frame active=*q;q->used=false;++active.attempts;return active;
}
Frame send(RoutingCore &c,uint32_t now,bool success=true){auto q=take(c,now);c.complete(q,now+10,success);return q;}
void known(RoutingCore &c,uint32_t node,uint32_t id,uint32_t now){
 auto p=packet(id,3,3,0,node,UINT32_MAX);auto m=data(4);m.nodeInfoValid=true;
 assert(ingest(c,p,now,m));send(c,now+150);
}
void learn(RoutingCore &c,uint32_t now=1000){
 known(c,B,0xb001,now);known(c,R,0xc001,now+200);
 auto p=packet(0xa001,3);assert(ingest(c,p,now+400));send(c,now+550);
 p=packet(0xa001,1);p[15]=uint8_t(R);assert(!ingest(c,p,now+600));
 p=packet(0xb002,2,3,0,B,A);p[15]=uint8_t(R);auto m=data(5);m.request_id=0xa001;m.routingPresent=m.routingErrorPresent=true;
 assert(ingest(c,p,now+700,m));assert(c.nextHop(B,now+701)==uint8_t(R));send(c,now+850);
 assert(!c.pending()&&!c.retransmitting());
}
mt::PacketLifetime::Bounds bounds(uint32_t air=Air,uint32_t slot=Slot){
 mt::RadioTiming::Deadline deadline;assert(mt::RadioTiming::resolve(air,deadline));
 const uint64_t rto=2*uint64_t(air)+304*uint64_t(slot)+Processing;assert(rto<=UINT32_MAX);
 mt::PacketLifetime::Bounds b;assert(mt::PacketLifetime::resolve(deadline.hostMs,uint32_t(rto),b));
 if(air==Air&&slot==Slot)assert(b.queueMs==161500&&b.pendingMs==2394840&&b.historyMs==2557340);
 return b;
}
void countsEmpty(const RoutingCore &c){assert(!c.pending()&&!c.retransmitting()&&!c.historyCount()&&!c.opaqueCount());}
void historyBoundary(RoutingCore &c,uint32_t id,uint32_t born,uint32_t ttl){
 c.clearPending();c.expire(born+ttl-1);assert(c.hasHistory(A,id));c.expire(born+ttl);assert(!c.hasHistory(A,id));
}
}
int main(){
 {
  struct Vector{uint32_t bw;unsigned sf;uint32_t slot;};
  for(const auto v:{Vector{250000,11,44},Vector{125000,12,155},Vector{500000,5,7},Vector{7800,12,2370}}){
   uint32_t out=0xfeedface;assert(mt::RadioSlot::resolve(v.bw,v.sf,out)&&out==v.slot);
  }
  for(uint32_t bw:{7800u,10400u,15600u,20800u,31250u,41700u,62500u,125000u,250000u,500000u})
   for(unsigned sf=5;sf<=12;++sf){uint32_t out=0;assert(mt::RadioSlot::resolve(bw,sf,out)&&out==uint32_t(4.5*(float(1u<<sf)/(float(bw)/1000))+7.6f));}
  for(const auto v:{Vector{0,12,0},Vector{125000,4,0},Vector{125000,13,0},Vector{1,12,0},Vector{5000,12,0}}){
   uint32_t out=0xfeedface;assert(!mt::RadioSlot::resolve(v.bw,v.sf,out)&&out==0xfeedface);
  }
  pass("RadioSlot literal44/155/7/2370ms vectors,80 US width/SF combinations and atomic invalid outputs");
 }
 {
  for(uint32_t air:{0u,262143u,UINT32_MAX}){mt::RadioTiming::Deadline d{91,92};assert(!mt::RadioTiming::resolve(air,d)&&d.driverMs==91&&d.hostMs==92);}
  mt::RadioTiming::Deadline d;assert(mt::RadioTiming::resolve(262142,d)&&d.driverMs==262143&&d.hostMs==263143);
  struct Bad{uint32_t host,rto;};
  for(auto v:{Bad{0,1},Bad{1,0},Bad{UINT32_MAX,UINT32_MAX},Bad{0x80000000u,1},Bad{1,0x80000000u}}){
   mt::PacketLifetime::Bounds b{11,22,33};assert(!mt::PacketLifetime::resolve(v.host,v.rto,b)&&b.queueMs==11&&b.pendingMs==22&&b.historyMs==33);
  }
  (void)bounds();pass("deadline and lifetime zero/overflow refusals retain caller output and long literal bounds");
 }
 {
  auto c=std::make_unique<RoutingCore>();Provider p;
  c->configureContinuous(uint8_t(Self),8);c->configureIdentity(Self);
  assert(!c->enableDynamicLifetimes()&&!c->configureSlotTime(Slot));countsEmpty(*c);
  c->setAirtimeProvider(Provider::get,&p);assert(c->enableDynamicLifetimes());
  assert(!c->enableDynamicLifetimes());
  for(uint32_t v:{0u,mt::RadioSlot::MaxSlotMs+1,UINT32_MAX})assert(!c->configureSlotTime(v));
  assert(c->configureSlotTime(Slot));auto packet1=packet(101);assert(ingest(*c,packet1,100));
  auto *q=c->next(250);assert(q&&q->expires==100+bounds().queueMs&&p.calls==1);
  historyBoundary(*c,101,100,bounds().historyMs);
  pass("mode requires provider, enables once, invalid explicit slots refuse atomically and1320 is not legacy-clamped");
 }
 for(unsigned state=0;state<4;++state){
  auto h=std::make_unique<Harness>(false);auto &c=h->core;auto p=packet(200+state);
  if(state==0){assert(ingest(c,p,100));c.clearPending();assert(c.historyCount());}
  if(state==1){assert(c.ingest(Protocol::Meshtastic,p.data(),48,100,0));c.clearPending();assert(c.opaqueCount());}
  if(state==2){Frame f{};f.len=17;f.expires=1000;put(f.bytes+8,200);assert(c.retry(f,100));assert(c.pending()&&!c.historyCount()&&!c.opaqueCount());}
  if(state==3){Frame f{};f.len=17;put(f.bytes+8,200);c.noteTransmitStart(f);assert(c.hasActiveTransmission()&&!c.pending());}
  const auto queued=c.pending(),history=c.historyCount(),opaque=c.opaqueCount();assert(!c.enableDynamicLifetimes());
  assert(c.pending()==queued&&c.historyCount()==history&&c.opaqueCount()==opaque&&!h->provider.calls);
  assert(c.hasActiveTransmission()==(state==3));
  c.clearRoutingState();assert(c.enableDynamicLifetimes()&&c.configureSlotTime(Slot));
 }
 pass("mode-enable refuses retained history, opaque history, queue-only and active-only state without clearing it");
 for(unsigned state=0;state<4;++state){
  auto h=std::make_unique<Harness>();auto &c=h->core;auto p=packet(250+state);
  if(state==0){assert(ingest(c,p,100));c.clearPending();}
  if(state==1){assert(c.ingest(Protocol::Meshtastic,p.data(),48,100,0));c.clearPending();}
  if(state==2){Frame f{};f.len=17;f.expires=1000;put(f.bytes+8,250);assert(c.retry(f,100));}
  if(state==3){Frame f{};f.len=17;put(f.bytes+8,250);c.noteTransmitStart(f);}
  const auto queued=c.pending(),history=c.historyCount(),opaque=c.opaqueCount();
  assert(!c.configureSlotTime(28));
  assert(c.pending()==queued&&c.historyCount()==history&&c.opaqueCount()==opaque&&c.hasActiveTransmission()==(state==3));
  c.clearRoutingState();p=packet(260+state);assert(ingest(c,p,2000));historyBoundary(c,260+state,2000,bounds().historyMs);
 }
 pass("slot reconfiguration refuses history/opaque/queue/active state atomically without silently changing1320ms");
 {
  auto h=std::make_unique<Harness>();auto &c=h->core;auto p=packet(301);
  assert(ingest(c,p,100));auto *q=c.next(250);assert(q);const auto expiry=q->expires,due=q->dueAt;
  assert(!c.configureSlotTime(28)&&!c.configureSlotTime(0));assert(q->expires==expiry&&q->dueAt==due);
  c.clearPending();assert(!c.configureSlotTime(28)&&c.hasHistory(A,301));
  c.clearRoutingState();p=packet(302);assert(ingest(c,p,500));historyBoundary(c,302,500,bounds().historyMs);
  assert(c.configureSlotTime(28));p=packet(303);assert(ingest(c,p,500+bounds().historyMs));
  historyBoundary(c,303,500+bounds().historyMs,bounds(Air,28).historyMs);
  pass("explicit slot changes require quiescence; refused writes preserve old captured policy until clear");
 }
 for(uint32_t invalid:{0u,262143u,UINT32_MAX}){
  auto h=std::make_unique<Harness>();auto &c=h->core;h->provider.air=invalid;auto p=packet(401);
  assert(!ingest(c,p,100));countsEmpty(c);assert(c.routingStats.noTiming==1&&h->provider.calls==1);
  h->provider.air=Air;assert(ingest(c,p,101));assert(c.next(251)->expires==101+bounds().queueMs);
 }
 pass("zero/unrepresentable provider timing refuses before history/queue admission and same identity can later admit");
 {
  auto h=std::make_unique<Harness>();auto &c=h->core;auto p=packet(501);
  for(size_t n:{size_t(16),size_t(256)})assert(!ingest(c,p,100,data(),n));
  assert(!ingest(c,p,100,data(1,240)));assert(!h->provider.calls);countsEmpty(c);
  c.setAirtimeProvider(nullptr);assert(!ingest(c,p,100));countsEmpty(c);assert(!h->provider.calls);
  c.setAirtimeProvider(Provider::get,&h->provider);
  auto m=data(1,239);assert(ingest(c,p,100,m,255)&&h->provider.length==255);
  c.clearRoutingState();m=data(1,0);assert(ingest(c,p,200,m,17)&&h->provider.length==17);
  pass("invalid lengths/encoded sizes and removed provider never fall back; timing receives exact admitted wire bound");
 }
 for(bool dynamic:{false,true}){
  auto h=std::make_unique<Harness>(dynamic,28);auto &c=h->core;learn(c);h->provider.air=5000;
  auto p=packet(601,3,3,uint8_t(Self));assert(ingest(c,p,3000));auto sent=send(c,3150);
  assert(sent.bytes[14]==uint8_t(R));
  assert(c.hasRetransmission(A,601)==dynamic);
  if(dynamic)assert(c.retransmissionDeadline(A,601)==3160+23012);
  else assert(c.routingStats.noTiming==1);
 }
 pass("5000ms decoded relay remains accepted; dynamic ARQ admits timing rejected by legacy4s retransmission guard");
 for(uint32_t born:{100u,UINT32_MAX-1000u}){
  auto h=std::make_unique<Harness>();auto &c=h->core;auto p=packet(701,1);
  assert(ingest(c,p,born));auto *q=c.next(born+150);assert(q);
  const auto due=q->dueAt,expiry=q->expires;assert(expiry==born+bounds().queueMs);
  h->provider.air=0;
  assert(!ingest(c,p,born+151));assert(q->dueAt==due&&q->expires==expiry);
  p[12]=uint8_t((3<<5)|2|8);assert(ingest(c,p,born+152));assert(q->dueAt==due&&q->expires==expiry);
  c.noteBlockedAirtime(UINT32_MAX);c.defer(*q,born+153,0);assert(q->expires==expiry&&h->provider.calls==1);
  c.expire(expiry-1);assert(c.hasQueued(A,701));c.expire(expiry);assert(!c.hasQueued(A,701));
  historyBoundary(c,701,born,bounds().historyMs);
 }
 pass("duplicate/hop upgrade/defer/blocked preserve captured queue and absolute history boundaries across wrap");
 {
  auto h=std::make_unique<Harness>();auto &c=h->core;auto p=packet(801,1);auto m=data(70);m.traceRouteValid=m.hasBitfield=true;
  assert(ingest(c,p,100,m));auto *q=c.next(250);assert(q);const auto expiry=q->expires,due=q->dueAt;
  p[12]=uint8_t((3<<5)|2);p[30]^=1;m.encodedDataSize=239;
  assert(c.upgradeTraceVariant(p.data(),255,m,300));assert(q->expires==expiry&&q->dueAt==due&&q->len==255);
  assert(h->provider.calls==1);historyBoundary(c,801,100,bounds().historyMs);
  pass("typed trace ciphertext/length upgrade preserves first-admission deadlines without recapture");
 }
 for(uint32_t born:{3000u,UINT32_MAX-1000u}){
  auto h=std::make_unique<Harness>();auto &c=h->core;
  learn(c,born-2000u); // Fresh route evidence in the same wrapped epoch.
  auto p=packet(901,3,3,uint8_t(Self));assert(ingest(c,p,born));auto active=send(c,born+150u);
  assert(c.hasRetransmission(A,901));const uint32_t lifecycle=born+bounds().pendingMs;
  assert(active.expires==born+bounds().queueMs);
  for(unsigned i=0;i<20;++i)c.noteBlockedAirtime(UINT32_MAX);
  assert(c.retransmissionDeadline(A,901)==lifecycle);
  c.expire(lifecycle-1);assert(c.hasRetransmission(A,901));c.expire(lifecycle);assert(!c.hasRetransmission(A,901)&&!c.hasQueued(A,901));
  historyBoundary(c,901,born,bounds().historyMs);
 }
 pass("blocked retry deadline clamps to first lifecycle, never extends pending/history, including wrap");
 {
  auto h=std::make_unique<Harness>();auto &c=h->core;learn(c);auto p=packet(1001,3,3,uint8_t(Self));assert(ingest(c,p,3000));
  auto active=take(c,3150);const auto expiry=active.expires;
  assert(c.retry(active,3160));auto *q=c.next(3660);assert(q&&q->expires==expiry&&q->attempts==1);
  auto second=take(c,3660);assert(c.retry(second,3670));q=c.next(4170);assert(q&&q->expires==expiry&&q->attempts==2);
  auto third=take(c,4170);assert(!c.retry(third,4180)&&c.historyAttempts(A,1001)==3);
  c.retireRetryRefusal(third);assert(!c.pending()&&!c.retransmitting()&&!c.hasActiveTransmission());
  historyBoundary(c,1001,3000,bounds().historyMs);
  pass("native busy retry preserves first queue expiry and three-attempt cap; refusal retires only active packet");
 }
 {
  auto h=std::make_unique<Harness>();auto &c=h->core;learn(c);auto p=packet(1101,3,3,uint8_t(Self));assert(ingest(c,p,3000));
  send(c,3150);const auto first=c.retransmissionDeadline(A,1101);assert(first==3160+645780);
  auto *q=c.next(first);assert(q&&q->expires==first+bounds().queueMs);
  send(c,first);const auto last=c.retransmissionDeadline(A,1101);assert(last==first+10+645780);
  q=c.next(last);assert(q&&q->expires==last+bounds().queueMs&&q->bytes[14]==0);
  const auto third=send(c,last);assert(third.attempts==3&&!c.hasRetransmission(A,1101));
  historyBoundary(c,1101,3000,bounds().historyMs);
  pass("ARQ queue attempts get finite queue windows under immutable captured lifecycle/history and stop at3");
 }
 {
  auto h=std::make_unique<Harness>(false);auto &c=h->core;auto p=packet(1201,2);assert(ingest(c,p,100));
  assert(c.next(250)->expires==30100&&!h->provider.calls);c.clearPending();
  assert(!ingest(c,p,500000));c.expire(600100);assert(c.hasHistory(A,1201));
  c.expire(1100000);assert(!c.hasHistory(A,1201));
  pass("mode-off decoded queue30s and sliding history600s remain unchanged");
 }
 for(uint32_t born:{100u,UINT32_MAX-1000u}){
  auto h=std::make_unique<Harness>();auto &c=h->core;auto p=packet(1301,2,0,0xfe,A,B,0);
  assert(c.ingest(Protocol::Meshtastic,p.data(),255,born,0));auto *q=c.next(born+150u);assert(q&&q->expires==born+30000u&&q->bytes[14]==0);
  assert(!h->provider.calls&&!c.historyCount()&&!c.knownCount()&&!c.routeCount()&&!c.retransmitting());
  c.expire(born+29999u);assert(c.hasQueued(A,1301));c.expire(born+30000u);assert(!c.hasQueued(A,1301));
  c.expire(born+59999u);assert(c.hasOpaqueHistory(A,1301));c.expire(born+60000u);assert(!c.hasOpaqueHistory(A,1301));
 }
 pass("dynamic mode leaves opaque/legacy-header queue30s/history60s and no route/ARQ side effects unchanged");
 std::printf("{\"PASS\":true,\"groups\":%u,\"hardware\":false,\"synthetic_metadata_only\":true}\n",groups);
}
