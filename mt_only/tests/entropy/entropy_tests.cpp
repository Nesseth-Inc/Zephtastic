// SPDX-License-Identifier: GPL-3.0-only
#include "BoundedEntropy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <functional>
using namespace mt::pki;
static unsigned wipes=0;
extern "C" void mt_entropy_test_wipe(void *p,size_t n){volatile uint8_t *b=(volatile uint8_t *)p;for(size_t i=0;i<n;++i)b[i]=0;++wipes;}
struct Port:EntropyPort {
  uint32_t clock=0,increment=250;unsigned reads=0,pauses=0,acquires=0,releases=0;
  int chunk=4;bool acquireOk=true,readyOk=true,releaseOk=true;std::function<void()> hook;
  bool acquire()override{++acquires;return acquireOk;}
  bool ready()const override{return readyOk;}
  int read(uint8_t *p,size_t n)override{++reads;if(hook)hook();if(chunk<0||chunk>32)return chunk;size_t c=size_t(chunk)<n?size_t(chunk):n;memset(p,0x71,c);return int(c);}
  void pause()override{++pauses;clock+=increment;}
  bool release()override{++releases;return releaseOk;}
  uint32_t micros()const override{return clock;}
};
static void group(const char *text){printf("PASS %s\n",text);}
int main(){
  {Port p;BoundedEntropy e(p);uint8_t out[34];memset(out,0xa5,34);assert(!e.fill(nullptr,1)&&!e.fill(out,0)&&!e.fill(out,33)&&!p.acquires);for(size_t n:{size_t(1),size_t(4),size_t(32)}){assert(e.fill(out,n)&&out[n]==0xa5);memset(out,0xa5,34);}group("bounded length and staged exact outputs");}
  {Port p;BoundedEntropy e(p);uint8_t out[32];memset(out,0xa5,32);p.chunk=0;p.increment=0;assert(!e.hedge(out)&&p.reads==64&&p.pauses==63&&p.releases==1&&out[0]==0xa5);group("stuck clock and empty source stop at64 reads63 pauses");}
  {Port p;BoundedEntropy e(p);uint8_t out[32];memset(out,0xa5,32);p.chunk=1;p.increment=10000;assert(!e.hedge(out)&&p.reads==2&&p.releases==1&&out[0]==0xa5);group("deadline refuses partial bytes before publication");}
  {for(int chunk:{-5,33}){Port p;BoundedEntropy e(p);uint8_t out[4];memset(out,0xa5,4);p.chunk=chunk;assert(!e.fill(out,4)&&p.releases==1&&out[0]==0xa5);}group("port errors and excessive counts fail closed");}
  {Port p;BoundedEntropy e(p);uint8_t out[4];memset(out,0xa5,4);p.acquireOk=false;assert(!e.fill(out,4)&&!p.reads&&!p.releases);p.acquireOk=true;p.readyOk=false;assert(!e.fill(out,4)&&!p.reads&&p.releases==1&&out[0]==0xa5);group("unavailable source never sampled");}
  {Port p;BoundedEntropy e(p);uint8_t out[4];memset(out,0xa5,4);p.hook=[&]{p.readyOk=false;};assert(!e.fill(out,4)&&p.releases==1&&out[0]==0xa5);group("source loss after final sample withholds all output");}
  {Port p;BoundedEntropy e(p);uint8_t out[4];memset(out,0xa5,4);p.releaseOk=false;assert(!e.fill(out,4)&&out[0]==0xa5);p.releaseOk=true;assert(!e.fill(out,4)&&p.releases==1&&p.acquires==1);group("uncertain cleanup is terminal with no delayed retry");}
  {Port p;BoundedEntropy e(p);uint8_t out[4],nested[4];memset(nested,0xa5,4);p.hook=[&]{assert(!e.fill(nested,4)&&nested[0]==0xa5);};assert(e.fill(out,4)&&p.reads==1);group("same-object reentry rejected");}
  {Port p;p.clock=UINT32_MAX-500;BoundedEntropy e(p);uint8_t out[32];assert(e.hedge(out));group("elapsed unsigned deadline survives clock wrap");}
  assert(wipes==13);group("every started operation wipes local entropy scratch");
  {Port p;BoundedEntropy e(p);assert(e.ready()&&!p.reads&&p.releases==1);p.readyOk=false;assert(!e.ready()&&!p.reads&&p.releases==2);p.readyOk=true;p.releaseOk=false;assert(!e.ready());auto calls=p.acquires;p.releaseOk=true;assert(!e.ready()&&p.acquires==calls);group("non-consuming readiness and cleanup failure revoke availability");}
}
