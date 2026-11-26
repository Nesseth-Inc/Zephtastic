// SPDX-License-Identifier: GPL-3.0-only
// Synthetic NOR engine tests; approved digest is a deterministic test policy,
// not the production whitelist. No target timing/physical erase proof.
#include "pki/FormerCodePreparation.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
using namespace mt::pki;
struct Nor final:AppendStore,HistoryPageEraser {
  std::vector<uint8_t> bytes=std::vector<uint8_t>(LegacyTailProbe::Bytes+8192,0x55);
  unsigned reads=0,programs=0,calls=0,afterReads=0;int geometry=0;
  int cutPage=-1,cutKind=0,readFaultPage=-1,corruptPage=-1;bool finalReadFault=false;
  std::vector<unsigned> trace;
  Nor(){std::fill(bytes.begin()+4096,bytes.end()-4096,0);}
  size_t capacity()const override{return bytes.size()-8192;}
  int read(size_t at,uint8_t *out,size_t n)override{
    ++reads;assert(out&&n==64&&at<=capacity()&&n<=capacity()-at);
    if(calls&&int(calls-1)==readFaultPage)return -EIO;
    if(finalReadFault&&calls==42&&afterReads==64)return -EIO;
    memcpy(out,bytes.data()+4096+at,n);if(calls)++afterReads;
    if(calls&&int(calls-1)==corruptPage&&at==(calls-1)*4096)out[0]=0;
    return 0;
  }
  int program(size_t,const uint8_t *,size_t)override{++programs;return -EACCES;}
  int validate()const override{return geometry;}
  int erasePage(unsigned page)override{
    assert(page<42&&page==calls);trace.push_back(page);++calls;afterReads=0;
    if(int(page)==cutPage&&cutKind==0)return -EIO;
    const unsigned n=int(page)==cutPage&&cutKind==1?2048:4096;
    std::fill(bytes.begin()+4096+page*4096,bytes.begin()+4096+page*4096+n,0xff);
    return int(page)==cutPage?-EIO:0;
  }
  void hash(uint8_t out[32]){crypto_blake2b(out,32,bytes.data()+4096,capacity());}
  void guards()const{
    assert(!programs);
    for(size_t i=0;i<4096;++i)assert(bytes[i]==0x55&&bytes[bytes.size()-1-i]==0x55);
  }
};
struct Progress final:PreparationProgress {
  unsigned calls=0,failAt=0;int error=-ETIMEDOUT;
  int checkpoint()override{return ++calls==failAt?error:0;}
};
int main(){
  uint8_t digest[32]{};unsigned checks=0;
  {
    Nor n;n.hash(digest);Progress p;bool latch=false;TailPreparationResult r;
    assert(!FormerCodePreparation::run(n,n,p,digest,latch,r));
    assert(latch&&r.complete&&r.blank&&r.attempted==42&&r.verified==42&&n.calls==42);
    for(unsigned i=0;i<42;++i)assert(n.trace[i]==i);
    checks=p.calls;n.guards();const auto calls=n.calls;
    assert(FormerCodePreparation::run(n,n,p,digest,latch,r)==-EALREADY&&n.calls==calls);
  }
  std::puts("PASS exactly42 fixed pages, complete FF proof, adjacent guards and no retry after success");
  for(unsigned page=0;page<42;++page)for(int kind=0;kind<3;++kind){
    Nor n;n.cutPage=int(page);n.cutKind=kind;Progress p;bool latch=false;TailPreparationResult r;
    assert(FormerCodePreparation::run(n,n,p,digest,latch,r)==-EIO);
    assert(latch&&r.attempted==page+1&&r.verified==page&&!r.complete&&!r.blank&&n.calls==page+1);
    const auto calls=n.calls;assert(FormerCodePreparation::run(n,n,p,digest,latch,r)==-EALREADY&&n.calls==calls);n.guards();
  }
  std::puts("PASS before/partial/after erase failures at every page stop immediately with truthful counters");
  for(unsigned fail=1;fail<=checks;++fail){
    Nor n;Progress p;p.failAt=fail;bool latch=false;TailPreparationResult r;
    assert(FormerCodePreparation::run(n,n,p,digest,latch,r)==-ETIMEDOUT);
    assert(!r.complete&&!r.blank&&r.attempted==n.calls&&r.verified<=r.attempted);
    assert(latch==(n.calls!=0));const auto calls=n.calls;
    if(latch)assert(FormerCodePreparation::run(n,n,p,digest,latch,r)==-EALREADY&&n.calls==calls);
    n.guards();
  }
  std::puts("PASS every progress checkpoint refuses deadline/epoch/health loss without a later erase call");
  for(int page=0;page<42;++page)for(bool corrupt:{false,true}){
    Nor n;if(corrupt)n.corruptPage=page;else n.readFaultPage=page;
    Progress p;bool latch=false;TailPreparationResult r;
    assert(FormerCodePreparation::run(n,n,p,digest,latch,r)==-EIO);
    assert(r.attempted==page+1&&r.verified==page&&!r.complete&&n.calls==unsigned(page+1));n.guards();
  }
  {Nor n;n.finalReadFault=true;Progress p;bool latch=false;TailPreparationResult r;
    assert(FormerCodePreparation::run(n,n,p,digest,latch,r)==-EIO&&r.verified==42&&!r.complete);n.guards();}
  std::puts("PASS every page readback error/corruption and final rescan failure preserve guards and stop");
  for(unsigned mode=0;mode<6;++mode){
    Nor n;Progress p;bool latch=false;TailPreparationResult r;uint8_t expected[32];memcpy(expected,digest,32);
    if(mode==0)expected[0]^=1;
    if(mode==1)memcpy(n.bytes.data()+4096,"ZTAH",4);
    if(mode==2)memcpy(n.bytes.data()+4096+4092,"CMIT",4);
    if(mode==3)n.bytes.resize(n.bytes.size()-1);
    if(mode==4)n.geometry=-ENOTSUP;
    if(mode==5)n.geometry=1;
    assert(FormerCodePreparation::run(n,n,p,expected,latch,r)<0&&!n.calls&&!latch&&!r.complete);
    assert(!n.programs);
  }
  {Nor n;std::fill(n.bytes.begin()+4096,n.bytes.end()-4096,0xff);Progress p;bool latch=false;TailPreparationResult r;
    assert(!FormerCodePreparation::run(n,n,p,digest,latch,r)&&r.complete&&r.blank&&!n.calls&&!latch);n.guards();}
  std::puts("PASS nonblank wrong digest, possible journal, geometry and capacity refuse; blank performs zero erases");
  using L=std::numeric_limits<int64_t>;
  assert(preparationDeadline(5,5,2)&&preparationDeadline(5,6,2)&&!preparationDeadline(5,7,2));
  assert(!preparationDeadline(-1,0,1)&&!preparationDeadline(0,-1,1)&&!preparationDeadline(5,4,2));
  assert(!preparationDeadline(0,0,0)&&!preparationDeadline(0,0,-1)&&!preparationDeadline(L::max(),L::max(),1));
  assert(preparationDeadline(L::max()-1,L::max()-1,1)&&!preparationDeadline(L::max()-1,L::max(),1));
  std::printf("PASS absolute monotonic boundary/overflow checks; %u progress failure boundaries\n",checks);
}
