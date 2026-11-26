// SPDX-License-Identifier: GPL-3.0-only
// Actual Endpoint boundary tests, synthetic frames only. No radio or crypto
// authentication claim: callers supply already admitted proof/telemetry here.
#include "pki_runtime_fixture.h"
#include <array>

namespace {
using Endpoint=mt::Endpoint;
constexpr uint32_t Ours=0x10203040,PeerA=0x11223344,PeerB=0x55667788;
unsigned groups=0;
void pass(const char *name){++groups;std::cout<<"PASS "<<name<<'\n';}
bench::Frame frame(uint32_t to,uint32_t id,bool reliable=false){
  bench::Frame f{};f.len=17;put(f.bytes,to);put(f.bytes+4,Ours);put(f.bytes+8,id);
  f.bytes[12]=reliable?8:0;f.bytes[15]=uint8_t(Ours);f.bytes[16]=1;return f;
}
void submit(Endpoint &ep,const bench::Frame &f,bool reliable,bool privateMessage=true,
            bool signedPublic=false,bool telemetry=false){
  for(uint8_t owner:{0,1})assert(!ep.submit(owner,f.bytes,f.len,reliable,100,
    false,true,telemetry,nullptr,privateMessage,false,false,telemetry,signedPublic));
}
Endpoint::Job *transmit(Endpoint &ep,uint32_t now=110){
  auto *j=ep.next(now);assert(j);ep.started(*j,0,now,10,0);ep.complete(*j,now+1,0);return j;
}
void authenticated(Endpoint &ep,uint32_t peer,uint32_t id){
  submit(ep,frame(peer,id,true),true);transmit(ep);ep.authenticated(id,peer,0);assert(ep.idle());
}
void local(Endpoint &ep,const bench::Frame &f,bool privateMessage,bool signedPublic){
  submit(ep,f,false,privateMessage,signedPublic);transmit(ep);assert(ep.idle());
}
Endpoint::Result take(Endpoint &ep,uint8_t owner,uint32_t id,Endpoint::Kind kind,
                      uint32_t peer,uint8_t code=0){
  Endpoint::Result r;assert(ep.take(owner,r));
  assert(r.id==id&&r.kind==kind&&r.peer==peer&&r.code==code&&r.attempts==1);return r;
}
void empty(Endpoint &ep){Endpoint::Result r;assert(!ep.take(0,r)&&!ep.take(1,r));}
std::array<uint8_t,sizeof(Endpoint)> snapshot(const Endpoint &ep){
  std::array<uint8_t,sizeof(Endpoint)> bytes{};memcpy(bytes.data(),&ep,sizeof(ep));return bytes;
}
void unchanged(const Endpoint &ep,const std::array<uint8_t,sizeof(Endpoint)> &bytes){
  assert(!memcmp(&ep,bytes.data(),sizeof(ep)));
}
void response(Endpoint &ep,uint32_t peer,uint32_t query,uint32_t packet){
  submit(ep,frame(peer,query),false,true,false,true);transmit(ep);
  uint8_t data[80]{};size_t n=0;assert(mt::TelemetryQuery::uptimeResponse(1234,query,data,sizeof(data),n));
  bench::RouteMetadata metadata;assert(bench::MtCodec::decodeData(data,n,metadata));
  assert(ep.privateTelemetry(Ours,peer,Ours,packet,metadata,120)&&ep.idle());
}
bool approval(Endpoint &ep,uint8_t owner,uint32_t peer,uint32_t query,uint32_t packet){
  uint8_t data[80]{};size_t n=0;
  return ep.takeTraceRoute(owner,peer,packet,query,130,data,sizeof(data),n,false,true,true);
}
}

int main(){
  {
    Endpoint ep;authenticated(ep,PeerA,101);authenticated(ep,PeerB,102);
    assert(ep.invalidatePrivatePeer(PeerA));
    for(uint8_t owner:{0,1}){
      take(ep,owner,101,Endpoint::Kind::Failed,0,4);
      take(ep,owner,102,Endpoint::Kind::Authenticated,PeerB);
    }
    empty(ep);const auto old=frame(PeerA,101,true);
    assert(ep.submit(0,old.bytes,old.len,true,140,false,true,false,nullptr,true)==-EALREADY);
    auto changed=old;changed.bytes[16]^=1;
    assert(ep.submit(0,changed.bytes,changed.len,true,140,false,true,false,nullptr,true)==-EALREADY);
    const auto other=frame(PeerB,102,true);
    assert(!ep.submit(1,other.bytes,other.len,true,140,false,true,false,nullptr,true)&&ep.idle());
    take(ep,1,102,Endpoint::Kind::Authenticated,PeerB);empty(ep);
    assert(ep.invalidatePrivatePeer(PeerA)&&ep.idle()&&ep.free()==4);empty(ep);
    pass("selected completions fail, other completion/history survives, nonce stays burned and repeat is no-op");
  }
  {
    Endpoint ep;
    for(uint32_t id=200;id<212;++id)authenticated(ep,(id&1)?PeerB:PeerA,id);
    // The eight-entry rings have wrapped: retain exactly the newest eight IDs.
    assert(ep.invalidatePrivatePeer(PeerA,9));
    for(uint8_t owner:{0,1})for(uint32_t id=204;id<212;++id){
      if(id&1)take(ep,owner,id,Endpoint::Kind::Authenticated,PeerB);
      else take(ep,owner,id,Endpoint::Kind::Failed,0,9);
    }
    empty(ep);pass("both wrapped result rings retain order, cardinality, IDs, attempts and unrelated result values");
  }
  {
    Endpoint ep;const auto broadcast=frame(0xffffffffu,301),directed=frame(PeerA,302),plain=frame(PeerA,303);
    local(ep,broadcast,false,true);local(ep,directed,false,true);local(ep,plain,false,false);
    assert(ep.invalidatePrivatePeer(PeerA));
    for(uint8_t owner:{0,1})for(uint32_t id:{301u,302u,303u})take(ep,owner,id,Endpoint::Kind::Local,0);
    empty(ep);
    assert(!ep.submit(0,broadcast.bytes,broadcast.len,false,140,false,true,false,nullptr,false,false,false,false,true));
    assert(!ep.submit(0,plain.bytes,plain.len,false,140,false,true,false,nullptr,false));
    assert(ep.submit(0,directed.bytes,directed.len,false,140,false,true,false,nullptr,false,false,false,false,true)==-EALREADY);
    take(ep,0,301,Endpoint::Kind::Local,0);take(ep,0,303,Endpoint::Kind::Local,0);empty(ep);
    assert(ep.idle());pass("signed-directed history invalidates; signed Public broadcast and unsigned directed history survive");
  }
  {
    Endpoint ep;response(ep,PeerA,401,1401);response(ep,PeerB,402,1402);
    assert(ep.invalidatePrivatePeer(PeerA,7));
    for(uint8_t owner:{0,1}){
      take(ep,owner,401,Endpoint::Kind::Failed,0,7);
      take(ep,owner,402,Endpoint::Kind::PrivateResponse,PeerB);
      assert(!approval(ep,owner,PeerA,401,1401));
      assert(approval(ep,owner,PeerB,402,1402));
      assert(!approval(ep,owner,PeerB,402,1402));
    }
    empty(ep);pass("selected private response approvals disappear; other peer approval retains one fanout per owner");
  }
  {
    Endpoint ep;authenticated(ep,PeerA,501);const auto before=snapshot(ep);
    for(uint32_t peer:{0u,1u,0xfffffffeu,0xffffffffu}){
      assert(!ep.invalidatePrivatePeer(peer));unchanged(ep,before);
    }
    assert(ep.invalidatePrivatePeer(0xfffffffdu));unchanged(ep,before);
    for(uint8_t owner:{0,1})take(ep,owner,501,Endpoint::Kind::Authenticated,PeerA);
    empty(ep);pass("reserved peers refuse before any mutation; valid absent peer is byte-identical no-op");
  }
  for(unsigned mode=0;mode<6;++mode){
    Endpoint ep;authenticated(ep,PeerA,601);const auto pending=frame(PeerB,602,true);
    if(mode<3)submit(ep,pending,true);
    else assert(!ep.submitNodeInfoControl(pending,true,100,true));
    auto *j=ep.next(400);assert(j);
    if(mode%3){ep.started(*j,0,400,10,0);if(mode%3==2)ep.complete(*j,401,0);}
    assert(!ep.idle());const auto before=snapshot(ep);
    assert(!ep.invalidatePrivatePeer(PeerA,8));unchanged(ep,before);
    for(uint8_t owner:{0,1})take(ep,owner,601,Endpoint::Kind::Authenticated,PeerA);
    assert(ep.pendingPrivate(602,PeerB)==(mode<3));
  }
  pass("queued active waiting user and control jobs refuse byte-identically before completed state changes");
  {
    Endpoint ep;ep.measuredUptime(100);uint8_t data[80]{};size_t n=0;
    assert(ep.privateTelemetryReply(PeerA,701,1000,data,sizeof(data),n));
    assert(ep.invalidatePrivatePeer(PeerA));
    assert(!ep.privateTelemetryReply(PeerA,702,1001,data,sizeof(data),n));
    assert(!ep.privateTelemetryReply(PeerA,701,7000,data,sizeof(data),n));
    assert(ep.privateTelemetryReply(PeerB,701,1001,data,sizeof(data),n));
    assert(ep.idle());empty(ep);pass("private telemetry rate and replay records survive selected-peer invalidation");
  }
  std::cout<<"{\"PASS\":true,\"groups\":"<<groups<<",\"hardware\":false,\"Endpoint_boundary_only\":true}\n";
}
