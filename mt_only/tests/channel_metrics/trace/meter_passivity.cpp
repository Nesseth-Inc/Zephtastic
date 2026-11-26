// SPDX-License-Identifier: GPL-3.0-only
// Compile unchanged against frozen legacy and current production ChannelMeter.
#include "ChannelMeter.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#ifdef NDEBUG
#error Assertions are required
#endif
struct Clock {
  uint64_t time=0,step=0;unsigned calls=0;std::string log;bool bound=false,supported=false;
  static uint64_t read(void *ctx){auto &c=*static_cast<Clock *>(ctx);++c.calls;c.log+='C';auto now=c.time;if(now!=UINT64_MAX)c.time+=c.step;return now;}
  mt::UptimeClock provider(){return {read,this};}
};
static void support(mt::ChannelMeter &m,Clock &c,bool value){m.supported(value);c.supported=value;}
struct Drain {
  Clock &clock;unsigned calls=0;uint32_t amount=0;bool unknown=false,unsupported=false;
  static void call(mt::ChannelMeter &m,void *ctx) {
    auto &d=*static_cast<Drain *>(ctx);++d.calls;d.clock.log+='D';
    if(d.unsupported){d.unsupported=false;support(m,d.clock,false);}
    if(d.unknown){d.unknown=false;m.observeRx(123,false);}
    if(d.amount){const auto amount=d.amount;d.amount=0;m.observeRx(amount,true);}
  }
};
static void diagnosticPure(mt::ChannelMeter &m,Clock &c,Drain &d) {
#ifdef CHANNEL_DIAGNOSTICS
  const auto clocks=c.calls,drains=d.calls;const auto log=c.log;
  for(unsigned i=0;i<20;++i){(void)m.diagnosticTrace();(void)m.diagnosticSupported();
    (void)m.diagnosticPending();(void)m.diagnosticBound();(void)m.diagnosticLastAvailable();}
  assert(c.calls==clocks&&d.calls==drains&&c.log==log);
#else
  (void)m;(void)c;(void)d;
#endif
}
static bool countersOnly=false;
static void output(const char *label,unsigned index,mt::ChannelMeter &m,Clock &c,Drain &d) {
  auto value=m.percent();uint32_t bits=0;if(value.has_value())std::memcpy(&bits,&*value,sizeof(bits));
  diagnosticPure(m,c,d);
#ifdef CHANNEL_DIAGNOSTICS
  assert(m.diagnosticBound()==c.bound&&m.diagnosticSupported()==c.supported);
  if(countersOnly){const auto &t=m.diagnosticTrace();
    std::cout<<label<<' '<<index<<' '<<d.calls<<' '<<m.diagnosticBound()<<' '<<m.diagnosticSupported()<<' '<<m.diagnosticPending()
      <<' '<<t.rxKnown<<' '<<t.rxUnknown<<' '<<t.uncertain<<' '<<t.loss<<' '<<t.limit<<' '<<t.support<<' '<<t.tx<<' '<<t.overlap<<' '<<t.other
      <<' '<<t.lastMs<<' '<<t.irq<<' '<<unsigned(t.len)<<' '<<unsigned(t.reason)<<' '<<t.known<<'\n';
  }else{
#endif
  std::cout<<label<<' '<<index<<' '<<value.has_value()<<' '<<bits<<' '<<c.calls<<' '<<d.calls<<' '<<c.bound<<' '<<c.supported<<' '<<(c.log.empty()?"-":c.log)<<'\n';
#ifdef CHANNEL_DIAGNOSTICS
  }
#endif
  c.log.clear();
}
static void observableTrace() {
  Clock c;Drain d{c};mt::ChannelMeter m;
  if(!countersOnly)output("unbound",0,m,c,d);
  assert(!m.bind({nullptr,nullptr}));assert(m.bind(c.provider(),Drain::call,&d));c.bound=true;
  assert(!m.bind(c.provider()));if(!countersOnly)output("unsupported",0,m,c,d);
  support(m,c,true);output("idle",0,m,c,d);
  c.time=100;m.observeRx(600,true);output("rx",0,m,c,d);
  c.time=500;m.noteAcceptedStart(300);output("pending",0,m,c,d);m.creditPending();m.creditPending();output("tx",0,m,c,d);
  c.time=1000;m.observeRx(0,false);output("unknown",0,m,c,d);
  c.time=60999;output("recovery",0,m,c,d);c.time=61000;output("recovery",1,m,c,d);
  support(m,c,false);output("off",0,m,c,d);support(m,c,false);support(m,c,true);output("resumed",0,m,c,d);
  c.time=121000;output("resumed",1,m,c,d);m.noteAcceptedStart(2);m.noteAcceptedStart(3);output("overlap",0,m,c,d);
  m.noteUncertain();m.noteUncertain();output("uncertain",0,m,c,d);
  c.time=UINT64_MAX;output("badclock",0,m,c,d);c.time=122000;output("badclock",1,m,c,d);
  c.time=182000;d.amount=120;output("drain",0,m,c,d);d.unknown=true;output("drain",1,m,c,d);
  c.time=242000;d.unsupported=true;output("drain",2,m,c,d);support(m,c,true);c.time=302000;output("drain",3,m,c,d);
  uint32_t random=0x139af42u;c.step=1;
  for(unsigned i=0;i<1500;++i) {
    random=random*1664525u+1013904223u;c.time=400000+uint64_t(i)*137;
    if(i%53==0)c.time=UINT64_MAX;
    switch(random%9) {
      case 0:support(m,c,(random&16)!=0);break;
      case 1:m.observeRx(random%2500,true);break;
      case 2:m.observeRx(random%255,false);break;
      case 3:m.noteAcceptedStart(random%2500);break;
      case 4:m.creditPending();break;
      case 5:m.noteUncertain();break;
      case 6:m.uncertain();break;
      case 7:d.amount=1+random%2500;break;
      case 8:d.unknown=true;break;
    }
    output("sequence",i,m,c,d);
  }
}
#ifdef CHANNEL_DIAGNOSTICS
static void diagnosticSemantics() {
  Clock c;Drain d{c};mt::ChannelMeter m;
  assert(!m.diagnosticBound()&&!m.diagnosticSupported()&&!m.diagnosticPending()&&!m.diagnosticLastAvailable());
  assert(m.bind(c.provider(),Drain::call,&d));assert(m.diagnosticBound());assert(c.calls==0);
  m.supported(true);assert(!m.diagnosticLastAvailable());assert(c.calls==1);
  assert(m.percent().has_value()&&m.diagnosticLastAvailable());
  c.time=100;m.observeRx(321,true,0x1234,17);auto s=m.diagnosticTrace();
  assert(s.rxKnown==1&&s.rxUnknown==0&&s.uncertain==0&&s.lastMs==321&&s.irq==0x1234&&s.len==17&&s.known);
  const auto clocks=c.calls;m.observeRx(777,false,0x4321,255);s=m.diagnosticTrace();
  assert(c.calls==clocks+1&&s.rxUnknown==1&&s.uncertain==1&&s.other==0&&s.reason==zt::ChannelEvent::RxUnknown);
  assert(s.lastMs==777&&s.irq==0x4321&&s.len==255&&!s.known);
  assert(m.diagnosticLastAvailable());assert(m.percent().has_value()&&m.diagnosticLastAvailable());
  m.supported(false);m.supported(false);m.supported(true);s=m.diagnosticTrace();assert(s.support==2&&s.uncertain==3);
  m.noteAcceptedStart(30);assert(m.diagnosticPending());m.noteAcceptedStart(40);m.noteUncertain();m.noteUncertain();
  s=m.diagnosticTrace();assert(!m.diagnosticPending()&&s.overlap==1&&s.tx==1&&s.uncertain==5);
  m.uncertain(zt::ChannelEvent::NativeLoss);m.uncertain(zt::ChannelEvent::DrainLimit);m.uncertain();
  s=m.diagnosticTrace();assert(s.loss==1&&s.limit==1&&s.other==1&&s.uncertain==8);
  const auto before=c.calls;m.uncertain(zt::ChannelEvent::None);auto after=m.diagnosticTrace();
  assert(c.calls==before+1&&after.uncertain==s.uncertain&&after.reason==s.reason);
  diagnosticPure(m,c,d);
  std::cerr<<"PASS diagnostic reasons, RXmetadata, cached availability and getter clock/drain passivity\n";
}
static zt::Observed<float> sample(mt::ChannelMeter &m,Clock &c,Drain &d){
  const auto clocks=c.calls,drains=d.calls;c.log.clear();auto value=m.percent();
  assert(c.calls==clocks+1&&d.calls==drains+1&&c.log=="DC");
  assert(m.diagnosticLastAvailable()==value.has_value());diagnosticPure(m,c,d);return value;
}
static void availabilityMatrix(){
  {
    Clock c;Drain d{c};mt::ChannelMeter m;assert(!m.percent()&&c.calls==0&&d.calls==0);
    assert(m.bind(c.provider(),Drain::call,&d));assert(c.calls==0);
    auto value=sample(m,c,d);assert(value&&*value==0&&!m.diagnosticSupported());
    assert(m.diagnosticTrace().uncertain==0&&m.diagnosticTrace().support==0);
    m.supported(true);assert(m.diagnosticTrace().support==1&&m.diagnosticTrace().uncertain==1);
    assert(sample(m,c,d)&&m.diagnosticSupported());
    std::cout<<"PASS matrix startup: unbound absent; unsupported bound logger zero with one ordered sample; initial support transition explicit\n";
  }
  {
    Clock c;Drain d{c};mt::ChannelMeter m;assert(m.bind(c.provider(),Drain::call,&d));m.supported(true);
    c.time=1000;m.observeRx(6000,true);auto value=sample(m,c,d);assert(value&&*value==10);
    m.noteAcceptedStart(3000);assert(m.diagnosticPending());value=sample(m,c,d);assert(value&&*value==10);
    m.creditPending();m.creditPending();value=sample(m,c,d);assert(value&&*value>14.99999f&&*value<15.00001f&&!m.diagnosticPending());
    std::cout<<"PASS matrix known RX and completed TX retain fixed-minute credits and supported-path clock/drain order\n";
  }
  {
    Clock c;Drain d{c};mt::ChannelMeter m;assert(m.bind(c.provider(),Drain::call,&d));m.supported(true);
    c.time=1000;m.observeRx(6000,true);m.observeRx(777,false,4,0);
    m.uncertain(zt::ChannelEvent::NativeLoss);m.uncertain(zt::ChannelEvent::DrainLimit);
    auto value=sample(m,c,d);assert(value&&*value==10);const auto &t=m.diagnosticTrace();
    assert(t.rxKnown==1&&t.rxUnknown==1&&t.loss==1&&t.limit==1&&t.uncertain==3&&t.lastMs==777);
    assert(t.irq==4&&t.len==0&&!t.known&&t.reason==zt::ChannelEvent::DrainLimit);
    std::cout<<"PASS matrix unknown/loss/drain-limit preserve logged credit and all diagnostic counters\n";
  }
  {
    Clock c;Drain d{c};mt::ChannelMeter m;assert(m.bind(c.provider(),Drain::call,&d));m.supported(true);
    c.time=1000;m.observeRx(6000,true);m.supported(false);m.supported(false);
    auto value=sample(m,c,d);assert(value&&*value==10&&!m.diagnosticSupported());
    m.supported(true);value=sample(m,c,d);assert(value&&*value==10);
    assert(m.diagnosticTrace().support==2&&m.diagnosticTrace().uncertain==2);
    c.time=60000;value=sample(m,c,d);assert(value&&*value==0);
    std::cout<<"PASS matrix support pause/resume retains estimate and diagnostic transitions; aged credits become zero\n";
  }
  {
    Clock c;Drain d{c};mt::ChannelMeter m;assert(m.bind(c.provider(),Drain::call,&d));m.supported(true);
    c.time=1000;m.observeRx(6000,true);assert(sample(m,c,d));
    c.time=UINT64_MAX;assert(!sample(m,c,d));c.time=1000;auto value=sample(m,c,d);assert(value&&*value==10);
    c.time=999;assert(!sample(m,c,d));c.time=1000;value=sample(m,c,d);assert(value&&*value==10);
    std::cout<<"PASS matrix bad/backward clock refuses; next nondecreasing sample recovers without erasing credits\n";
  }
  {
    Clock c;Drain d{c};mt::ChannelMeter m;assert(m.bind(c.provider(),Drain::call,&d));m.supported(true);
    m.observeRx(UINT32_MAX,true);m.observeRx(1,true);assert(!sample(m,c,d));
    Clock aggregate;Drain ad{aggregate};mt::ChannelMeter a;assert(a.bind(aggregate.provider(),Drain::call,&ad));a.supported(true);
    a.observeRx(UINT32_MAX-10,true);aggregate.time=10000;a.observeRx(11,true);assert(!sample(a,aggregate,ad));
    Clock raw;Drain rd{raw};mt::ChannelMeter r;assert(r.bind(raw.provider(),Drain::call,&rd));r.supported(true);
    r.observeRx(60001,true);auto value=sample(r,raw,rd);assert(value&&*value>100);
    std::cout<<"PASS matrix bucket/aggregate overflow absent; representable raw value above100 leaves encoder admission separate\n";
  }
}
#endif
int main(int argc,char **argv){
#ifdef CHANNEL_DIAGNOSTICS
if(argc==2){const std::string mode=argv[1];if(mode=="--diagnostics-only"){diagnosticSemantics();return 0;}
if(mode=="--availability-matrix"){availabilityMatrix();return 0;}
assert(mode=="--counter-trace");countersOnly=true;}
#else
(void)argc;(void)argv;
#endif
observableTrace();
}
