// SPDX-License-Identifier: GPL-3.0-only
#include "PendingBluetoothAdmin.h"
#include <stdio.h>
#include <errno.h>

using Pending=mt::ble::PendingBluetoothAdmin;
using State=Pending::State;
static unsigned checks=0;
#define CHECK(x) do {++checks;if(!(x)){printf("FAIL line %u\n",unsigned(__LINE__));return 1;}}while(0)
static Pending::Request request(uint32_t session=7) {
  Pending::Request r;r.session=session;r.packetId=0;r.wantAck=true;r.wantResponse=true;
  r.desired.mode=mt::BluetoothSettings::FixedPin;r.desired.fixedPin=0;return r;
}
static bool cleared(const Pending::Request &r) {
  return !r.session&&!r.packetId&&!r.wantAck&&!r.wantResponse&&
    r.desired.same(mt::BluetoothSettings::defaults());
}
static bool cleared(const Pending::Completion &c) {
  return !c.session&&!c.packetId&&!c.wantAck&&!c.wantResponse&&!c.replyAllowed&&!c.result;
}
int main() {
  const uint32_t staleTokens[]={0,8};
  Pending p;Pending::Request out=request(99);Pending::Completion done;
  CHECK(!p.busy()&&p.state()==State::Empty);
  CHECK(!p.stage(request(0),true)&&p.state()==State::Empty);
  auto bad=request();bad.desired.mode=3;CHECK(!p.stage(bad,true)&&!p.busy());
  bad=request();bad.desired.fixedPin=1000000;CHECK(!p.stage(bad,true)&&!p.busy());
  bad=request();bad.desired.mode=mt::BluetoothSettings::RandomPin;
  CHECK(!p.stage(bad,false)&&!p.busy());
  CHECK(!p.claim(7,out)&&cleared(out));done.result=-EIO;done.replyAllowed=true;
  CHECK(!p.finish(7,0,done)&&cleared(done));
  CHECK(p.stage(request(),false)&&p.busy()&&p.state()==State::Pending);
  auto replacement=request(8);replacement.packetId=123;replacement.wantAck=false;
  CHECK(!p.stage(replacement,true));p.disconnected(8);p.disconnected(0);
  CHECK(p.state()==State::Pending&&p.claim(7,out)&&p.state()==State::Committing);
  CHECK(out.session==7&&!out.packetId&&out.wantAck&&out.wantResponse&&out.desired.same(request().desired));
  CHECK(!p.stage(replacement,true));out=request(99);
  CHECK(!p.claim(7,out)&&cleared(out)&&p.state()==State::Committing);
  CHECK(p.finish(7,-EIO,done)&&done.session==7&&!done.packetId&&done.wantAck&&done.wantResponse&&done.replyAllowed&&done.result==-EIO);
  CHECK(p.state()==State::Empty&&!p.busy());
  CHECK(!p.finish(7,0,done)&&cleared(done));
  CHECK(p.stage(request(),true));p.disconnected(7);
  CHECK(!p.busy()&&!p.claim(7,out)&&cleared(out));
  for(uint32_t stale:staleTokens) {
    CHECK(p.stage(request(),true));out=request(99);
    CHECK(!p.claim(stale,out)&&cleared(out)&&!p.busy());
  }
  CHECK(p.stage(request(),true)&&p.claim(7,out));p.disconnected(7);
  CHECK(p.busy()&&p.state()==State::Committing&&!p.stage(replacement,true));
  CHECK(p.finish(7,0,done)&&!done.replyAllowed&&done.session==7&&done.result==0&&!p.busy());
  CHECK(p.stage(request(),true)&&p.claim(7,out));p.disconnected(8);
  CHECK(p.finish(7,0,done)&&done.replyAllowed&&!p.busy());
  for(uint32_t stale:staleTokens) {
    auto r=request();r.packetId=91;r.wantAck=false;r.wantResponse=false;
    CHECK(p.stage(r,true)&&p.claim(7,out));
    CHECK(p.finish(stale,-EBUSY,done)&&!done.replyAllowed&&done.packetId==91&&
      !done.wantAck&&!done.wantResponse&&done.result==-EBUSY&&!p.busy());
  }
  auto random=request();random.desired.mode=mt::BluetoothSettings::RandomPin;
  CHECK(p.stage(random,true)&&p.claim(7,out)&&p.finish(7,0,done)&&done.replyAllowed);
  printf("PASS %u lifecycle assertions\n",checks);return 0;
}
