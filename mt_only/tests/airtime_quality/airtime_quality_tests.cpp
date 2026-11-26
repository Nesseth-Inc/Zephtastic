// SPDX-License-Identifier: GPL-3.0-only
// Quality wrapper tests, separate from the already-qualified window math.
#include "AirtimeMeter.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#ifdef NDEBUG
#error Acceptance requires enabled assertions
#endif
struct Clock {uint64_t ms=0;bool valid=true;};
static uint64_t readClock(void *p){auto &c=*static_cast<Clock *>(p);return c.valid?c.ms:UINT64_MAX;}
static void bind(mt::AirtimeMeter &m,Clock &c){assert(m.bind(mt::UptimeClock{readClock,&c}));m.ensureAnchored();}
static void expected(mt::AirtimeMeter &m,uint64_t ms){auto p=m.percent();assert(p&&std::fabs(double(*p)-double(ms)/36000.0)<0.00000003);}
static void group(const char *name){std::cout<<"PASS "<<name<<"\n";}
int main(){
 {zt::Observed<float> absent;assert(!absent&&!absent.has_value());
  zt::Observed<float> zero{0};assert(zero&&zero.has_value()&&*zero==0);
  auto copy=zero;zero={};assert(!zero&&copy&&*copy==0);
  zt::Observed<uint64_t> maximum{UINT64_MAX};assert(maximum&&*maximum==UINT64_MAX);}
 group("scalar availability preserves zero, copying and 64-bit values without libstdc++");
 {mt::AirtimeMeter m;assert(!m.percent());Clock c;bind(m,c);expected(m,0);
  m.noteAcceptedStart(600);if(auto p=m.percent())assert(*p==0);m.creditPending();expected(m,600);m.creditPending();expected(m,600);
  assert(!m.pending()&&!m.lost()&&!m.uncertain());}
 group("unobserved differs from clean idle; accepted sample credited once");
 {mt::AirtimeMeter m;Clock c{1000,true};bind(m,c);m.noteAcceptedStart(600);m.noteUncertain();assert(m.uncertain()==1&&!m.percent());
  for(uint64_t now:{1001ull,60000ull,3600999ull}){c.ms=now;assert(!m.percent());}
  c.ms=3601000;expected(m,0);assert(m.uncertain()==1);}
 group("uncertain event suppresses reporting through a full conservative hour");
 {mt::AirtimeMeter m;Clock c;bind(m,c);m.noteAcceptedStart(600);c.valid=false;m.creditPending();assert(m.lost()==1&&!m.percent());
  c.valid=true;c.ms=1000;assert(!m.percent());c.ms=3600999;assert(!m.percent());c.ms=3601000;expected(m,0);}
 group("lost completion clock starts recovery at first trustworthy later observation");
 {mt::AirtimeMeter m;m.noteAcceptedStart(600);m.creditPending();assert(m.lost()==1);Clock c{1000,true};bind(m,c);
  assert(!m.percent());c.ms=3600999;assert(!m.percent());c.ms=3601000;expected(m,0);}
 group("events before first binding are retained as unknown quality");
 {mt::AirtimeMeter m;Clock c{UINT64_MAX-2,true};bind(m,c);m.noteAcceptedStart(600);m.noteUncertain();assert(!m.percent());
  c.ms=UINT64_MAX-1;assert(!m.percent());c.ms=UINT64_MAX;assert(!m.percent());c.ms=0;assert(!m.percent());}
 group("near-maximum clock cannot wrap a quality deadline into available zero");
 {mt::AirtimeMeter m;Clock c{1000,true};bind(m,c);m.noteAcceptedStart(600);m.noteAcceptedStart(700);
  m.creditPending();assert(m.uncertain()>=1&&!m.percent());}
 group("a second accepted start cannot silently overwrite unresolved history");
 {mt::AirtimeMeter m;Clock c{1000,true};bind(m,c);m.noteAcceptedStart(600);m.creditPending();expected(m,600);
  c.ms=999;m.noteAcceptedStart(700);m.creditPending();assert(m.lost()>=1&&!m.percent());
  c.ms=1000;assert(!m.percent());c.ms=3600999;assert(!m.percent());c.ms=3601000;expected(m,0);}
 group("rejected backward credit remains unknown when the clock moves forward again");
 {mt::AirtimeMeter m;Clock c{1000,true};bind(m,c);m.noteAcceptedStart(UINT32_MAX);m.creditPending();
  m.noteAcceptedStart(1);m.creditPending();assert(m.lost()>=1&&!m.percent());
  c.ms=2000;assert(!m.percent());c.ms=3600999;assert(!m.percent());c.ms=3601000;expected(m,0);}
 group("overflow or rejected estimate cannot silently expose a surviving partial total");
 {mt::AirtimeMeter m;Clock c{1000,true};bind(m,c);m.noteAcceptedStart(600);m.noteUncertain();
  c.ms=2000;m.noteAcceptedStart(700);m.noteUncertain();assert(m.uncertain()==2);
  c.ms=3601000;assert(!m.percent());c.ms=3601999;assert(!m.percent());c.ms=3602000;expected(m,0);}
 group("each later uncertain terminal event extends quality quarantine by a whole hour");
}
