// SPDX-License-Identifier: GPL-3.0-only
// Independent caller-visible contracts; no target devices or entropy material.
#include "pki/BoundedEntropy.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
using mt::pki::BoundedEntropy;
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);std::abort();}}while(0)
struct Port:mt::pki::EntropyPort {
  unsigned acquires=0,reads=0,pauses=0,releases=0,readyCalls=0;
  uint32_t clock=0,acquireCost=0,readCost=0,pauseCost=0,releaseCost=0;
  int fragment=32,errorAt=-1,oversizeAt=-1,starveBefore=0;
  bool leased=false,acquireOK=true,releaseOK=true,isReady=true;
  unsigned unreadyAt=0;
  std::function<void()> callback;
  bool acquire()override{++acquires;clock+=acquireCost;leased=acquireOK;return acquireOK;}
  bool ready()const override {auto &self=*const_cast<Port*>(this);REQUIRE(leased);++self.readyCalls;return isReady&&(!unreadyAt||readyCalls<unreadyAt);}
  int read(uint8_t *out,size_t n)override {
    REQUIRE(leased);++reads;clock+=readCost;
    if(callback){auto saved=callback;callback=nullptr;saved();}
    if(int(reads)==errorAt)return -5;
    if(int(reads)==oversizeAt)return int(n+1);
    if(int(reads)<=starveBefore)return 0;
    const size_t count=n<size_t(fragment)?n:size_t(fragment);
    for(size_t i=0;i<count;++i)out[i]=uint8_t(0x40+reads);
    return int(count);
  }
  void pause()override {REQUIRE(leased);++pauses;clock+=pauseCost;}
  bool release()override{REQUIRE(leased);++releases;clock+=releaseCost;leased=false;return releaseOK;}
  uint32_t micros()const override{return clock;}
};
using Buffer=std::array<uint8_t,40>;
Buffer sentinel(){Buffer b;b.fill(0xa5);return b;}
void invalid(){Port p;BoundedEntropy e(p);auto b=sentinel();REQUIRE(!e.fill(nullptr,1));REQUIRE(!e.fill(b.data(),0));REQUIRE(!e.fill(b.data(),33));REQUIRE(p.acquires==0&&b==sentinel());std::puts("PASS invalid-length-no-provider-access");}
void fragments(){for(unsigned n=1;n<=32;++n)for(int chunk=1;chunk<=32;++chunk){Port p;p.fragment=chunk;BoundedEntropy e(p);auto b=sentinel();REQUIRE(e.fill(b.data(),n));REQUIRE(p.reads==(n+chunk-1)/unsigned(chunk));REQUIRE(p.pauses+1==p.reads&&p.releases==1&&!p.leased);for(unsigned i=n;i<b.size();++i)REQUIRE(b[i]==0xa5);}std::puts("PASS all-1024-length-fragment-combinations");}
void starvation(){for(int before:{0,1,31,32,62,63,64,65}){Port p;p.starveBefore=before;BoundedEntropy e(p);auto b=sentinel();const bool ok=e.fill(b.data(),32);REQUIRE(ok==(before<64));REQUIRE(p.reads<=BoundedEntropy::MaxReads&&p.pauses<=63&&p.releases==1);if(!ok)REQUIRE(b==sentinel());}Port p;p.fragment=0;BoundedEntropy e(p);auto b=sentinel();REQUIRE(!e.hedge(b.data()));REQUIRE(p.reads==64&&p.pauses==63&&b==sentinel());std::puts("PASS starvation-and-stuck-clock-attempt-bound");}
void partialFailure(){for(unsigned at=1;at<=32;++at)for(bool oversize:{false,true}){Port p;p.fragment=1;if(oversize)p.oversizeAt=at;else p.errorAt=at;BoundedEntropy e(p);auto b=sentinel();REQUIRE(!e.fill(b.data(),32));REQUIRE(p.reads==at&&p.releases==1&&b==sentinel());}std::puts("PASS partial-read-errors-and-overreported-counts");}
void readiness(){for(unsigned at=1;at<=33;++at){Port p;p.fragment=1;p.unreadyAt=at;BoundedEntropy e(p);auto b=sentinel();REQUIRE(!e.fill(b.data(),32));REQUIRE(p.releases==1&&b==sentinel());}std::puts("PASS readiness-loss-before-during-after-read");}
void deadlines(){for(unsigned which=0;which<4;++which){Port p;if(which==0)p.acquireCost=20000;if(which==1)p.readCost=20000;if(which==2){p.fragment=1;p.pauseCost=20000;}if(which==3)p.releaseCost=20000;BoundedEntropy e(p);auto b=sentinel();REQUIRE(!e.fill(b.data(),32));REQUIRE(p.releases==1&&b==sentinel());}for(uint32_t start:{0u,0xfffffff0u,0xffffffffu}){Port p;p.clock=start;p.readCost=19999;BoundedEntropy e(p);auto b=sentinel();REQUIRE(e.fill(b.data(),32));}std::puts("PASS exact-deadline-and-timer-wrap");}
void leaseFailure(){Port p;BoundedEntropy e(p);auto b=sentinel();p.acquireOK=false;REQUIRE(!e.fill(b.data(),4));REQUIRE(p.releases==0&&p.reads==0&&b==sentinel());p.acquireOK=true;REQUIRE(e.fill(b.data(),4));p.releaseOK=false;b=sentinel();REQUIRE(!e.fill(b.data(),4));const unsigned attempts=p.acquires;const unsigned releases=p.releases;p.releaseOK=true;for(unsigned i=0;i<10;++i)REQUIRE(!e.fill(b.data(),4));REQUIRE(p.acquires==attempts&&p.releases==releases&&b==sentinel());std::puts("PASS acquire-recovery-and-release-poison-no-cleanup-retry");}
void reentry(){Port p;BoundedEntropy e(p);auto outer=sentinel(),inner=sentinel();p.callback=[&]{REQUIRE(!e.ready());REQUIRE(!e.fill(inner.data(),32));REQUIRE(inner==sentinel());};REQUIRE(e.fill(outer.data(),32));REQUIRE(p.acquires==1&&p.releases==1);REQUIRE(e.fill(inner.data(),4));std::puts("PASS recursive-reentry-rejected-without-poisoning-owner");}
void readyProbe(){Port p;BoundedEntropy e(p);REQUIRE(e.ready());REQUIRE(p.acquires==1&&p.releases==1&&!p.reads&&!p.pauses);p.acquireOK=false;REQUIRE(!e.ready());p.acquireOK=true;REQUIRE(e.ready());p.fragment=0;auto b=sentinel();REQUIRE(!e.fill(b.data(),32)&&b==sentinel());REQUIRE(e.ready());p.isReady=false;REQUIRE(!e.ready());p.isReady=true;REQUIRE(e.ready());std::puts("PASS nonconsuming-ready-is-not-next-draw-or-authority-guarantee");}
void readyPoison(){Port p;BoundedEntropy e(p);p.releaseOK=false;REQUIRE(!e.ready());const auto calls=p.acquires;auto b=sentinel();p.releaseOK=true;REQUIRE(!e.ready()&&!e.fill(b.data(),4));REQUIRE(p.acquires==calls&&b==sentinel());std::puts("PASS nonconsuming-probe-release-failure-poisons-future-draws");}
int main(){readyProbe();readyPoison();invalid();fragments();starvation();partialFailure();readiness();deadlines();leaseFailure();reentry();}
