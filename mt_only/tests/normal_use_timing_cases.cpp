// SPDX-License-Identifier: GPL-3.0-only
// Integrated normal Service policy, using synthetic frames and a controlled
// channel logger. No devices, transports or RF are opened by this fixture.
#include "endpoint_fixture.h"

static void positionRequest(Fixture &f) {
  f.s.stop();auto cfg=f.s.config;
  assert(cfg.setFixedPosition(123456789,-987654321,false,0));cfg.setPositionSharing(true);
  assert(f.s.apply(cfg)&&f.s.start(false,0)&&f.s.armEndpoint(true));
  const uint8_t data[]={8,3,24,1,72,2};uint8_t frame[16+sizeof(data)]{};
  for(unsigned i=0;i<4;++i){frame[i]=uint8_t(OURS>>(8*i));frame[4+i]=uint8_t(PEER>>(8*i));frame[8+i]=uint8_t(701u>>(8*i));}
  frame[13]=8;frame[15]=uint8_t(PEER);memcpy(frame+16,data,sizeof(data));uint8_t nonce[16]{};
  bench::MtCodec::nonce(PEER,701,nonce);
  assert(bench::mtAesCtr(nullptr,bench::MtNodeRecord::PublicKey,16,nonce,frame+16,sizeof(data)));
  f.s.rx(frame,sizeof(frame),3,100,700);
}

int main() {
  std::cout.setf(std::ios::unitbuf);
  {
    Fixture f(true,81,false,false);f.text(1,PEER,false);f.s.poll(300,0);
    assert(!f.radio.sends&&f.s.throttled==1&&f.s.endpointFree()==3&&!f.s.faulted());
    assert(f.s.channel_.bind({Fixture::channelClock,&f.channelNow}));f.s.channel_.supported(true);f.s.poll(600,0);
    assert(f.radio.sends==1&&f.s.active());f.done(700);
    f.clients();assert(!f.s.faulted()&&f.s.endpointFree()==4&&results(f.h,1,0)==1);
    group("unavailable channel measurement queues work without a native attempt until a real clock is bound");
  }
  {
    Fixture f;f.s.channel_.supported(false);f.text(1,PEER,false);f.s.poll(300,0);
    const auto logged=f.s.channel_.percent();assert(logged&&*logged==0);
    assert(!f.radio.sends&&f.s.throttled==1&&f.s.endpointFree()==3&&!f.s.faulted());
    f.s.channel_.supported(true);f.s.poll(600,0);assert(f.radio.sends==1);f.done(700);
    group("bound zero logger cannot admit transmission while native observation support is absent");
  }
  {
    for(uint32_t amount:{23999u,24000u}) {
      Fixture f;f.s.channel_.observeRx(amount,true);f.text(1,PEER,false);f.s.poll(300,0);
      if(amount==23999){assert(f.radio.sends==1);f.done(400);}
      else {assert(!f.radio.sends&&f.s.throttled==1&&f.s.endpointFree()==3);
        f.channelNow=60000;f.s.poll(600,0);assert(f.radio.sends==1);f.done(700);}
      assert(!f.s.faulted());
    }
    group("human traffic uses the strict forty-percent channel boundary and resumes after logger aging");
  }
  {
    for(uint32_t amount:{14999u,15000u,18000u}) {
      Fixture f;positionRequest(f);f.s.channel_.observeRx(amount,true);f.s.poll(500,0);
      if(amount==14999){assert(f.radio.sends==1);bench::RouteMetadata m;
        assert(f.s.codec.decode(f.radio.frames[0].data(),f.radio.frames[0].size(),m)&&m.portnum==3);f.done(600);}
      else assert(!f.radio.sends&&f.s.throttled==1);
      assert(!f.s.faulted());
    }
    Fixture human;human.s.channel_.observeRx(18000,true);human.text(1,PEER,false);human.s.poll(300,0);
    assert(human.radio.sends==1);human.done(400);
    group("position responses are polite at twenty-five percent while human messages remain eligible at thirty percent");
  }
  {
    Fixture f;f.radio.air=3000;
    for(unsigned i=0;i<3;++i) {
      const uint32_t at=100+i*4000;f.text(1+i,PEER,false,at);f.s.poll(at+200,0);
      assert(f.radio.sends==int(i+1)&&f.s.active());f.done(at+3200);
      assert(!f.s.active()&&!f.s.faulted());
    }
    assert(f.s.sent==3&&f.s.endpointFree()==4);
    group("normal mode completes long frames beyond the old per-frame and six-second cumulative trial limits");
  }
  {
    Fixture f;f.radio.air=120000;f.text(1,PEER,false);f.s.poll(300,0);
    assert(f.radio.sends==1&&f.s.active());f.s.poll(120300,0);
    assert(f.s.active()&&!f.s.faulted());f.done(150000);
    assert(!f.s.active()&&!f.s.faulted()&&f.s.sent==1&&f.s.endpointFree()==4);
    Fixture timeout;timeout.radio.air=120000;timeout.text(1,PEER,false);timeout.s.poll(300,0);
    timeout.s.poll(151799,0);assert(timeout.s.active()&&!timeout.s.faulted());
    timeout.s.poll(151800,0);assert(timeout.s.faulted()&&!timeout.s.active()&&timeout.radio.sends==1);
    group("long native completion uses the driver timeout plus host margin and fails at the exact finite deadline");
  }
  {
    Fixture f;f.radio.air=262143;f.text(1,PEER,false);f.s.poll(300,0);
    assert(!f.radio.sends&&f.s.endpointFree()==4&&!f.s.faulted());
    f.radio.air=100;f.text(1,PEER,false,600);f.s.poll(600,0);
    assert(f.radio.sends==1);f.done(700);
    group("unrepresentable duration refuses before publication and does not burn the packet ID");
  }
  {
    Fixture f;f.s.channel_.observeRx(60001,true);f.text(1,PEER,false);f.s.poll(300,0);
    assert(!f.radio.sends&&f.s.throttled==1&&!f.s.faulted());
    f.channelNow=60000;f.s.poll(600,0);assert(f.radio.sends==1);f.done(700);
    group("out-of-range logged utilization remains ineligible and is never converted to a fabricated zero");
  }
}
