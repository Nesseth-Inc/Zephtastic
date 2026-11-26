// SPDX-License-Identifier: GPL-3.0-only
// Proposal only: actual Endpoint public API, synthetic already-supplied frames.
// No Service, native driver, nonce durability, PKI authentication or RF claim.
#ifdef NDEBUG
#error "Endpoint lifetime assertions require assertions enabled"
#endif
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <array>
#include <iostream>
#include <memory>
#include "Endpoint.h"
#include "RadioTiming.h"
#include "PacketLifetime.h"

namespace {
using Endpoint=mt::Endpoint;
constexpr uint32_t Ours=0x10203040u,Peer=0x11223344u,LongAir=120000u;
unsigned groups=0;
unsigned resultCalls=0;
void pass(const char *name){++groups;std::cout<<"PASS "<<name<<std::endl;}
void put(uint8_t *p,uint32_t value){for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));}
bench::Frame frame(uint32_t id,bool reliable=false,size_t length=29){
  assert(length>=17&&length<=255);
  bench::Frame f{};f.len=length;put(f.bytes,Peer);put(f.bytes+4,Ours);put(f.bytes+8,id);
  f.bytes[12]=reliable?8:0;f.bytes[15]=uint8_t(Ours);
  for(size_t i=16;i<length;++i)f.bytes[i]=uint8_t(i^id);
  return f;
}
struct Provider {
  Endpoint *endpoint=nullptr;
  uint32_t air=LongAir;
  unsigned calls=0,freeAtCall=0,controlsAtCall=0;
  size_t lastLength=0;
  static uint32_t get(size_t n,void *context){
    assert(context);auto &p=*static_cast<Provider *>(context);
    ++p.calls;p.lastLength=n;
    if(p.endpoint){p.freeAtCall=p.endpoint->free();p.controlsAtCall=p.endpoint->pendingControls();}
    return p.air;
  }
};
mt::PacketLifetime::Bounds expected(uint32_t air){
  mt::RadioTiming::Deadline timing;assert(mt::RadioTiming::resolve(air,timing));
  const uint64_t rto=16000u+2*uint64_t(air);assert(rto<=UINT32_MAX);
  mt::PacketLifetime::Bounds out;assert(mt::PacketLifetime::resolve(timing.hostMs,uint32_t(rto),out));
  // Independent literal check of a deliberately long-airtime vector.
  if(air==LongAir){
    assert(timing.driverMs==150500u&&timing.hostMs==151500u);
    assert(out.queueMs==161500u&&out.pendingMs==1225500u&&out.historyMs==1388000u);
  }
  return out;
}
void bounds(const Endpoint::Job &job,uint32_t queue,uint32_t pending,uint32_t born){
  assert(job.lifetimeCaptured&&job.queueLifetimeMs==queue&&job.pendingLifetimeMs==pending&&job.born==born);
}
void bounds(const Endpoint::Job &job,const mt::PacketLifetime::Bounds &value,uint32_t born){
  bounds(job,value.queueMs,value.pendingMs,born);
}
int submit(Endpoint &ep,const bench::Frame &f,bool control,uint32_t now,
           bool reliable=false,bool privateUser=false,bool *created=nullptr,bool position=false){
  if(control)return ep.submitPrivateControl(f,7001u,now,position);
  return ep.submit(0,f.bytes,f.len,reliable,now,false,true,false,created,privateUser);
}
Endpoint::Job &next(Endpoint &ep,uint32_t now){auto *job=ep.next(now);assert(job);return *job;}
void noResult(Endpoint &ep){Endpoint::Result r;assert(!ep.take(0,r)&&!ep.take(1,r));}
void result(Endpoint &ep,uint32_t id,Endpoint::Kind kind,uint8_t code,uint8_t attempts){
  const unsigned resultCall=++resultCalls;
  Endpoint::Result r;assert(ep.take(0,r));
  if(!(r.id==id&&r.kind==kind&&r.code==code&&r.attempts==attempts)){
    std::cerr<<"FAIL endpoint-lifetime-result call="<<resultCall<<" after_group="<<groups
      <<" expected={id:"<<id<<",kind:"<<unsigned(kind)<<",code:"<<unsigned(code)
      <<",attempts:"<<unsigned(attempts)<<"} actual={id:"<<r.id<<",kind:"
      <<unsigned(r.kind)<<",code:"<<unsigned(r.code)<<",attempts:"
      <<unsigned(r.attempts)<<"}"<<std::endl;
  }
  assert(r.id==id&&r.kind==kind&&r.code==code&&r.attempts==attempts);
  noResult(ep);
}
struct Borrowed {
  Endpoint::Job *job;
  uint8_t *data;
  size_t length;
  std::array<uint8_t,255> bytes{};
  explicit Borrowed(Endpoint::Job &j):job(&j),data(j.frame.bytes),length(j.frame.len){
    assert(length<=bytes.size());std::memcpy(bytes.data(),data,length);
  }
  void retained()const{
    assert(job->state==Endpoint::State::Active&&job->frame.bytes==data&&job->frame.len==length);
    assert(!std::memcmp(data,bytes.data(),length));
  }
};
void assertOccupied(Endpoint &ep,bool control){
  assert(!ep.idle()&&ep.free()==(control?4u:3u)&&ep.pendingControls()==(control?1u:0u));
}
void drainQueuedOrWaiting(Endpoint &ep){ep.cancel();assert(ep.idle());}
}

int main(){
  {
    auto storage=std::make_unique<Endpoint>();auto &ep=*storage;
    Provider a{&ep},b{&ep};b.air=100;
    assert(!ep.setLifetimeAirtimeProvider(nullptr,&b));
    assert(ep.idle()&&ep.free()==4&&!ep.pendingControls());
    assert(ep.setLifetimeAirtimeProvider(Provider::get,&a));
    assert(!ep.setLifetimeAirtimeProvider(nullptr,&b));
    assert(!ep.setLifetimeAirtimeProvider(Provider::get,&b));
    auto f=frame(101);assert(!submit(ep,f,false,100));
    bounds(next(ep,100),expected(LongAir),100);
    assert(a.calls==1&&!b.calls&&a.lastLength==f.len&&a.freeAtCall==4&&!a.controlsAtCall);
    ep.cancel();assert(!ep.setLifetimeAirtimeProvider(Provider::get,&b));
    f=frame(102);assert(!submit(ep,f,false,200));
    bounds(next(ep,200),expected(LongAir),200);assert(a.calls==2&&!b.calls);
    pass("null and rebind refuse atomically; cancellation cannot replace the installed callback/context");
  }
  for(bool control:{false,true})for(unsigned phase=0;phase<3;++phase){
    auto storage=std::make_unique<Endpoint>();auto &ep=*storage;
    Provider rejected{&ep},accepted{&ep};accepted.air=100;
    const auto f=frame(200+phase,true);assert(!submit(ep,f,control,100,true,true));
    auto &job=next(ep,400);bounds(job,10000,90000,100);
    if(phase){ep.started(job,0,400,100,0);if(phase==2)ep.complete(job,401,0);}
    assertOccupied(ep,control);
    assert(!ep.setLifetimeAirtimeProvider(Provider::get,&rejected));
    assert(!ep.setLifetimeAirtimeProvider(nullptr,&rejected));
    if(phase==1){Borrowed held(job);ep.cancelPrivate();held.retained();ep.complete(job,402,0);}
    else drainQueuedOrWaiting(ep);
    assert(ep.idle()&&!rejected.calls&&!accepted.calls);
    assert(ep.setLifetimeAirtimeProvider(Provider::get,&accepted));
    const auto later=frame(300+phase);assert(!submit(ep,later,control,500));
    bounds(next(ep,800),expected(100),500);assert(!rejected.calls&&accepted.calls==1);
  }
  pass("first bind refuses queued active and waiting user/control work without poisoning a later idle bind");
  for(bool control:{false,true}){
    auto storage=std::make_unique<Endpoint>();auto &ep=*storage;Provider p{&ep};
    assert(ep.setLifetimeAirtimeProvider(Provider::get,&p));
    auto f=frame(401);
    for(size_t bad:{size_t(16),size_t(256)}){
      f.len=bad;
      assert(submit(ep,f,control,100)==-EINVAL);
      assert(ep.idle()&&ep.free()==4&&!ep.pendingControls()&&!p.calls);noResult(ep);
    }
    f.len=29;
    for(uint32_t bad:{0u,262143u,UINT32_MAX}){
      p.air=bad;bool created=true;
      assert(submit(ep,f,control,100,false,false,&created)==-EMSGSIZE);
      if(!control)assert(!created);
      assert(ep.idle()&&ep.free()==4&&!ep.pendingControls());noResult(ep);
      assert(p.lastLength==f.len&&p.freeAtCall==4&&!p.controlsAtCall);
      assert(ep.generatedId(401)==401); // Refusal did not reserve its ID.
    }
    p.air=LongAir;bool created=false;
    assert(!submit(ep,f,control,100,false,false,&created));if(!control)assert(created);
    assert(p.calls==4);bounds(next(ep,400),expected(LongAir),100);
  }
  pass("bad lengths and unusable airtime refuse before slot/nonce publication; same ID later admits");
  for(bool control:{false,true})for(uint32_t air:{1u,100u,LongAir,262142u})for(size_t length:{size_t(17),size_t(29),size_t(255)}){
    auto storage=std::make_unique<Endpoint>();auto &ep=*storage;Provider p{&ep};p.air=air;
    assert(ep.setLifetimeAirtimeProvider(Provider::get,&p));const auto f=frame(501,true,length);
    assert(!submit(ep,f,control,100,true));auto &job=next(ep,400);const auto want=expected(air);
    bounds(job,want,100);assert(p.calls==1&&p.lastLength==length&&p.freeAtCall==4&&!p.controlsAtCall);
    p.air=0;
    if(!control)assert(!ep.submit(1,f.bytes,f.len,true,401)); // Join, not recapture.
    mt::Config cfg;assert(cfg.setDisplay(29,false,0));ep.positionConfig(cfg);
    assert(!ep.setLifetimeAirtimeProvider(Provider::get,&p));
    ep.blocked(2000);ep.tick(500,0);assert(&next(ep,500)==&job);
    bounds(job,want,100);assert(p.calls==1);
  }
  pass("final 17/29/255-byte lengths capture exact bounds once; owner join/config/blocked/tick cannot recapture");
  for(bool control:{false,true})for(bool busy:{false,true}){
    auto storage=std::make_unique<Endpoint>();auto &ep=*storage;Provider p{&ep};
    assert(ep.setLifetimeAirtimeProvider(Provider::get,&p));const auto f=frame(601,true);
    assert(!submit(ep,f,control,100,true));auto *job=&next(ep,400);const auto want=expected(LongAir);
    p.air=0;uint32_t now=400;
    // Pinned stock permits five total attempts for acknowledged unicast.
    constexpr unsigned totalAttempts=5;
    for(unsigned attempt=1;attempt<=totalAttempts;++attempt){
      assert(job->frame.len==f.len&&!std::memcmp(job->frame.bytes,f.bytes,f.len));
      bounds(*job,want,100);ep.started(*job,busy?-EBUSY:0,now,LongAir,0);
      if(!busy){assert(job->attempts==attempt);ep.complete(*job,now+1,0);}
      if(attempt<totalAttempts){
        assert(job->attempts==attempt);
        if(!busy)ep.tick(job->due,0);
        now=job->due;auto *again=ep.next(now);assert(again==job);bounds(*again,want,100);
      }else if(!busy)ep.tick(job->due,0);
      assert(p.calls==1);
    }
    assert(ep.idle()&&ep.free()==4&&!ep.pendingControls()&&!ep.next(now+1000000u));
    // Five reliable-unicast transmissions occur, but the next 255-second
    // retry deadline is beyond the fixed absolute pending lifetime. tick()
    // resolves lifetime expiry (code 3) before retry exhaustion (code 5).
    // CAD-busy failures use 250ms backoff and exhaust before that lifetime.
    if(control)noResult(ep);else result(ep,601,Endpoint::Kind::Failed,busy?5:3,totalAttempts);
  }
  pass("native completion and CAD-busy unicast retries retain frame/born/bounds and stop at exactly five attempts");
  for(bool control:{false,true})for(bool legacy:{false,true})for(uint32_t born:{100u,UINT32_MAX-1000u}){
    auto storage=std::make_unique<Endpoint>();auto &ep=*storage;Provider p{&ep};
    if(!legacy)assert(ep.setLifetimeAirtimeProvider(Provider::get,&p));
    const auto f=frame(701);assert(!submit(ep,f,control,born));auto &job=next(ep,born+300u);
    const uint32_t queue=legacy?10000u:expected(LongAir).queueMs;
    const uint32_t pending=legacy?90000u:expected(LongAir).pendingMs;
    bounds(job,queue,pending,born);p.air=0;
    ep.tick(born+queue-1u,0);assertOccupied(ep,control);assert(&next(ep,born+queue-1u)==&job);
    ep.tick(born+queue,0);assert(ep.idle()&&!ep.next(born+queue)&&ep.free()==4&&!ep.pendingControls());
    assert(p.calls==(legacy?0u:1u));
    if(control)noResult(ep);else result(ep,701,Endpoint::Kind::Failed,3,0);
  }
  pass("queued expiry before/at captured or explicit legacy10s bound, including uint32 wrap");
  for(bool control:{false,true})for(bool legacy:{false,true})for(uint32_t born:{100u,UINT32_MAX-1000u}){
    auto storage=std::make_unique<Endpoint>();auto &ep=*storage;Provider p{&ep};
    if(!legacy)assert(ep.setLifetimeAirtimeProvider(Provider::get,&p));
    const auto f=frame(801,true);assert(!submit(ep,f,control,born,true));auto &job=next(ep,born+300u);
    const auto pending=job.pendingLifetimeMs,queue=job.queueLifetimeMs;
    bounds(job,legacy?10000u:expected(LongAir).queueMs,legacy?90000u:expected(LongAir).pendingMs,born);
    ep.started(job,0,born+300u,LongAir,0);ep.complete(job,born+301u,0);p.air=0;
    assert(job.state==Endpoint::State::Waiting);ep.blocked(pending+1000000u);
    ep.tick(born+pending-1u,0);assertOccupied(ep,control);assert(job.state==Endpoint::State::Waiting);
    bounds(job,queue,pending,born);ep.tick(born+pending,0);assert(ep.idle()&&!ep.next(born+pending));
    assert(p.calls==(legacy?0u:1u));
    if(control)noResult(ep);else result(ep,801,Endpoint::Kind::Failed,3,1);
  }
  pass("blocked wait cannot extend whole-job capture or explicit legacy90s pending lifetime across wrap");
  for(bool control:{false,true})for(bool reliable:{false,true})for(bool tickFirst:{false,true}){
    auto storage=std::make_unique<Endpoint>();auto &ep=*storage;Provider p{&ep};
    assert(ep.setLifetimeAirtimeProvider(Provider::get,&p));const auto f=frame(901,reliable);
    assert(!submit(ep,f,control,100,reliable,true));auto &job=next(ep,400);
    const auto expiry=job.born+job.pendingLifetimeMs;ep.started(job,0,400,LongAir,0);Borrowed held(job);p.air=0;
    ep.tick(expiry-1u,0);held.retained();assert(!job.terminal);noResult(ep);
    if(tickFirst){
      ep.tick(expiry,0);held.retained();assertOccupied(ep,control);
      assert(job.terminal&&job.result.kind==Endpoint::Kind::Failed&&job.result.code==3);
      noResult(ep);assert(!ep.next(expiry));
    }
    assert(p.calls==1);ep.complete(job,expiry,0);
    assert(ep.idle()&&ep.free()==4&&!ep.pendingControls()&&!ep.next(expiry+100000u));
    if(control)noResult(ep);else result(ep,901,Endpoint::Kind::Failed,3,1);
  }
  pass("active user/control buffers survive expiry; tick-first and late-completion paths fail3 without retry");
  for(bool control:{false,true}){
    auto storage=std::make_unique<Endpoint>();auto &ep=*storage;Provider p{&ep};
    assert(ep.setLifetimeAirtimeProvider(Provider::get,&p));const auto f=frame(1001,true);
    assert(!submit(ep,f,control,100,true,true));auto &job=next(ep,400);
    const auto expiry=job.born+job.pendingLifetimeMs;ep.started(job,0,400,LongAir,0);Borrowed held(job);
    ep.tick(expiry,0);held.retained();assert(job.terminal&&job.result.code==3);
    const auto other=frame(1002);assert(!submit(ep,other,control,expiry));
    assert(p.calls==2);held.retained();assert(&next(ep,expiry+300u)!=&job);
    ep.complete(job,expiry+301u,0);assertOccupied(ep,control);
    if(control)noResult(ep);else result(ep,1001,Endpoint::Kind::Failed,3,1);
    drainQueuedOrWaiting(ep);
  }
  pass("expired active slot remains borrowed while another slot admits and cannot be selected/reused");
  for(bool control:{false,true})for(bool revoke:{false,true}){
    auto storage=std::make_unique<Endpoint>();auto &ep=*storage;Provider p{&ep};
    assert(ep.setLifetimeAirtimeProvider(Provider::get,&p));const auto f=frame(1101,true);
    assert(!submit(ep,f,control,100,true,true));auto &job=next(ep,400);
    const auto expiry=job.born+job.pendingLifetimeMs;ep.started(job,0,400,LongAir,0);Borrowed held(job);
    // Existing user revocation force-overrides proof; controls retain the
    // first terminal outcome. Do not introduce a new control priority policy.
    if(!revoke||!control){
      ep.authenticated(1101,Peer,0);held.retained();
      assert(job.terminal&&job.result.kind==Endpoint::Kind::Authenticated&&job.result.code==0);
    }
    if(revoke){ep.cancelPrivate(7);held.retained();assert(job.result.kind==Endpoint::Kind::Failed&&job.result.code==7);}
    ep.tick(expiry,0);held.retained();noResult(ep);
    assert(job.result.kind==(revoke?Endpoint::Kind::Failed:Endpoint::Kind::Authenticated)&&job.result.code==(revoke?7:0));
    p.air=0;ep.complete(job,expiry+1u,0);assert(ep.idle()&&p.calls==1);
    if(control)noResult(ep);else result(ep,1101,revoke?Endpoint::Kind::Failed:Endpoint::Kind::Authenticated,revoke?7:0,1);
  }
  pass("earlier proof/cancellation survives expiry; active user revocation still overrides proof");
  {
    auto storage=std::make_unique<Endpoint>();auto &ep=*storage;Provider p{&ep};
    mt::Config cfg;assert(cfg.setFixedPosition(100,200,false,0));cfg.setPositionSharing(true);ep.positionConfig(cfg);
    assert(ep.setLifetimeAirtimeProvider(Provider::get,&p));const auto f=frame(1201,true);
    assert(!submit(ep,f,true,100,true,false,nullptr,true));auto &job=next(ep,400);
    const auto expiry=job.born+job.pendingLifetimeMs;ep.started(job,0,400,LongAir,0);Borrowed held(job);
    cfg.setPositionSharing(false);ep.positionConfig(cfg);held.retained();
    assert(job.terminal&&job.result.kind==Endpoint::Kind::Failed&&job.result.code==4);
    ep.tick(expiry,0);held.retained();assert(job.result.code==4);
    ep.complete(job,expiry+1u,0);assert(ep.idle()&&p.calls==1);noResult(ep);
    pass("position consent revocation holds active control ciphertext and failure4 priority through expiry/completion");
  }
  {
    auto storage=std::make_unique<Endpoint>();auto &ep=*storage;Provider p{&ep};
    assert(ep.setLifetimeAirtimeProvider(Provider::get,&p));const auto f=frame(1301,true);
    assert(!submit(ep,f,false,100,true,true));auto &job=next(ep,400);
    const auto expiry=job.born+job.pendingLifetimeMs;ep.started(job,0,400,LongAir,0);Borrowed held(job);
    ep.disconnect(0);held.retained();assert(!job.owners&&job.terminal&&job.result.code==4);
    ep.tick(expiry,0);held.retained();assert(job.result.code==4);ep.complete(job,expiry+1u,0);
    assert(ep.idle()&&p.calls==1);noResult(ep);
    pass("owner disconnect cancels retry without freeing an active frame or leaking a result to a new owner");
  }
  for(bool module:{false,true}){
    auto storage=std::make_unique<Endpoint>();auto &ep=*storage;Provider p{&ep};p.air=0;
    assert(ep.setLifetimeAirtimeProvider(Provider::get,&p));const auto f=frame(1401);
    const auto add=[&](){return module?ep.submitModuleControl(f,100,false):ep.submitNodeInfoControl(f,false,100);};
    assert(add()!=0&&ep.idle()&&ep.free()==4&&!ep.pendingControls());
    assert(ep.generatedId(1401)==1401&&p.calls==1);
    p.air=LongAir;assert(add()==0);bounds(next(ep,400),expected(LongAir),100);
    assert(p.calls==2&&p.lastLength==f.len&&p.freeAtCall==4&&!p.controlsAtCall);
  }
  pass("NodeInfo/module supplied-frame public admission cannot bypass capture or burn refused IDs");
  std::cout<<"{\"PASS\":true,\"groups\":"<<groups<<",\"Endpoint_boundary_only\":true,\"hardware\":false,\"crypto_authentication_proof\":false}\n";
}
