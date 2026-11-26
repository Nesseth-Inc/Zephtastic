// SPDX-License-Identifier: GPL-3.0-only
#include "Service.h"
#include <assert.h>
#include <stdio.h>
struct Mock:mt::Radio {
  bool ok=true,busyFlag=false,signal=false,rxOn=false;
  int startResult=0,restoreResult=0,stopResult=0,healthResult=1,sendResult=0,completionResult=0;
  unsigned starts=0,stops=0,tx=0,restores=0;uint32_t air=500;
  uint8_t wire[255]{};size_t len=0;
  bool ready() override{return ok;}
  int start() override{++starts;rxOn=!startResult;return startResult;}
  int restore() override{++restores;return restoreResult;}
  int stop() override{++stops;rxOn=false;return stopResult;}
  int health() override{return healthResult;}
  bool busy() override{return busyFlag;}
  uint32_t airtime(size_t) override{return air;}
  int send(uint8_t *p,size_t n) override{++tx;memcpy(wire,p,n);len=n;return sendResult;}
  bool completion(int &result) override{result=completionResult;return signal;}
};
static mt::Config provision() {mt::Config c;c.bytes[4]=1;auto n=c.node();n.setPublic();c.node(n);return c;}
static bench::Frame packet(uint32_t from=0x12345678,uint32_t id=123) {
  bench::Frame f;f.len=24;
  for(int i=0;i<4;++i){f.bytes[i]=255;f.bytes[4+i]=uint8_t(from>>(8*i));f.bytes[8+i]=uint8_t(id>>(8*i));}
  f.bytes[12]=(3<<5)|3;f.bytes[13]=8;f.bytes[15]=uint8_t(from);
  const uint8_t data[]={8,1,18,2,'h','i',72,0};memcpy(f.bytes+16,data,sizeof(data));
  uint8_t nonce[16];bench::MtCodec::nonce(from,id,nonce);
  assert(bench::mtAesCtr(nullptr,bench::MtNodeRecord::PublicKey,16,nonce,f.bytes+16,sizeof(data)));return f;
}
static unsigned passed=0;
#define PASS(name) do{++passed;puts(name);}while(0)
int main() {
  {mt::Config c;assert(c.valid()&&!c.provisioned());c=provision();assert(c.provisioned());
   for(unsigned i=0;i<sizeof(c.bytes);++i){auto bad=c;bad.bytes[i]^=1;assert(!bad.valid());}
   auto unsupported=c;unsupported.bytes[4]=2;unsupported.seal();assert(!unsupported.valid());
   auto n=c.node();n.bytes[3]=0;n.bytes[4]=0;memset(n.bytes+5,0,32);n.seal();c.node(n);assert(!c.provisioned());PASS("versioned configuration: corruption and incomplete provisioning rejected");}
  {uint8_t a[6]={0,1,2,3,4,5},b[6]={0,1,2,3,4,6};
   assert(!mt::identity(a,5));assert(mt::identity(a,6)==mt::identity(a,6));assert(mt::identity(a,6)!=mt::identity(b,6));assert(uint8_t(mt::identity(a,6)));PASS("fresh deterministic device domain identity");}
  {Mock r;mt::Service s(r,false,0x33445566,81);s.poll(90000,22);assert(!r.starts&&!r.tx&&!s.receiving());
   assert(!s.start(false,0));assert(s.apply(provision()));assert(!s.start(true,0));assert(s.start(false,0));
   auto f=packet();s.rx(f.bytes,f.len,0,2,3);s.poll(90000,5);assert(s.received==1&&!r.tx&&!s.route.pending());
   assert(!s.announce(90001,1));assert(!s.apply(provision()));s.stop();assert(!s.receiving());
   mt::Service reboot(r,false,0x33445566,81);assert(reboot.apply(s.config));assert(!reboot.receiving());PASS("release RX-only, no boot RF, no persisted enable or discovery");}
  {Mock r;mt::Service s(r,true,0x33445566,81);assert(s.apply(provision()));assert(s.start(true,0));
   auto f=packet();s.rx(f.bytes,f.len,0,1,5);s.rx(f.bytes,f.len,0,2,6);assert(s.route.pending()==1);s.poll(10000,7);
   assert(r.tx==1&&s.active());assert((r.wire[12]&7)==2&&r.wire[15]==0x66);assert(!memcmp(r.wire+16,f.bytes+16,8));
   r.signal=true;s.poll(10600,8);assert(s.sent==1&&!s.active()&&r.restores==1);s.rx(f.bytes,f.len,0,10601,0);
   assert(!s.route.pending());PASS("actual service: encrypted relay, duplicate suppression, hop and completion");}
  {Mock r;r.busyFlag=true;mt::Service s(r,true,0x33445566,81);s.apply(provision());s.start(true,0);auto f=packet();s.rx(f.bytes,f.len,0,1,0);s.poll(10000,3);assert(!r.tx&&s.route.pending()==1);s.stop();s.poll(20000,4);assert(!r.tx&&!s.route.pending());PASS("busy channel defer and stop drain");}
  {Mock r;r.sendResult=-EBUSY;mt::Service s(r,true,0x33445566,81);s.apply(provision());s.start(true,0);auto f=packet();s.rx(f.bytes,f.len,0,1,0);
   for(unsigned t=10000;t<14000;t+=1000)s.poll(t,3);
   assert(r.tx==3&&!s.active()&&!s.route.pending());PASS("bounded busy retries");}
  {Mock r;mt::Service s(r,true,0x33445566,81);s.apply(provision());s.start(true,0);auto f=packet();s.rx(f.bytes,f.len,0,1,0);
   mt::RadioTiming::Deadline deadline;assert(mt::RadioTiming::resolve(r.air,deadline));
   s.poll(10000,3);s.poll(11500,4);assert(s.active()&&!s.faulted()&&s.receiving()&&r.stops==0);
   s.poll(10000+deadline.hostMs,5);assert(s.faulted()&&!s.receiving()&&r.stops==1);assert(!s.start(true,12000+deadline.hostMs));PASS("TX deadline faults closed and requires reboot");}
  {Mock r;mt::Service s(r,true,0x33445566,81);s.apply(provision());s.start(true,0);s.rx(nullptr,0,0,1,0);assert(s.rejected==1&&!s.faulted());r.healthResult=-EIO;s.rx(nullptr,0,0,2,0);assert(s.faulted()&&!s.receiving());PASS("CRC notification versus native I/O fault");}
  {Mock r;r.startResult=-EIO;mt::Service s(r,true,0x33445566,81);s.apply(provision());assert(!s.start(true,0)&&s.faulted());PASS("radio startup failure");}
  {Mock r;r.restoreResult=-EIO;mt::Service s(r,true,0x33445566,81);s.apply(provision());s.start(true,0);auto f=packet();s.rx(f.bytes,f.len,0,1,0);s.poll(10000,0);r.signal=true;s.poll(11000,0);assert(s.faulted());PASS("RX restoration failure");}
  {Mock r;mt::Service s(r,true,0x33445566,127);s.apply(provision());s.start(true,0);assert(s.announce(1,55));s.poll(300,0);assert(r.tx==1);bench::RouteMetadata m;assert(s.codec.decode(r.wire,r.len,m)&&m.nodeInfoValid&&m.hasBitfield);
   bool model=false;for(unsigned i=0;i+1<m.payloadLength;++i)if(m.payload[i]==0x28&&m.payload[i+1]==127)model=true;assert(model);assert(r.wire[12]==0);r.signal=true;s.poll(900,1);assert(s.discovery.completed==1);PASS("T096 zero-hop Public NodeInfo model 127 and bitfield");}
  {Mock r;r.signal=true;mt::Service s(r,true,0x33445566,81);s.apply(provision());s.start(true,0);
   for(unsigned i=0;i<14;++i){auto f=packet(0x12345678,100+i);s.rx(f.bytes,f.len,0,1000+i*2000,0);s.poll(2000+i*2000,0);s.poll(2600+i*2000,0);}
   assert(r.tx==12&&s.throttled>0);PASS("finite airtime budget bounds repeated traffic");}
  {Mock r;r.air=2157;mt::Service s(r,true,0x33445566,81);s.apply(provision());s.start(true,0);
   auto f=packet();f.len=255;memset(f.bytes+24,0xa7,231);s.rx(f.bytes,f.len,0,1,0);s.poll(10000,0);
   assert(r.tx==1&&r.len==255&&!s.faulted());r.signal=true;s.poll(12300,0);assert(s.sent==1&&s.receiving());PASS("maximum 255-byte LongFast packet does not fault");}
  {Mock r;r.air=2600;mt::Service s(r,true,0x33445566,81);s.apply(provision());s.start(true,0);
   auto f=packet();s.rx(f.bytes,f.len,0,1,0);s.poll(10000,0);assert(!r.tx&&!s.faulted()&&s.receiving()&&s.failed==1);PASS("airtime policy refusal preserves receiver");}
  {Mock r;mt::Service s(r,true,0x33445566,81);s.apply(provision());s.start(true,0);
   auto f=packet();f.bytes[0]=0x77;f.bytes[1]=0x66;f.bytes[2]=0x55;f.bytes[3]=0x44;f.bytes[13]=0;
   s.rx(f.bytes,f.len,0,1,0);s.poll(10000,0);assert(r.tx==1&&s.route.routingStats.opaque==1&&!s.route.routeCount()&&!s.route.historyCount());PASS("opaque PKI-shaped transit bypasses channel signature verifier");}
  printf("PASS %u service scenario groups; no hardware\n",passed);
}
