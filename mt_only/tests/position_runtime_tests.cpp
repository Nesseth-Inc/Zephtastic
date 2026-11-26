// SPDX-License-Identifier: GPL-3.0-only
#include "gnss/PositionController.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <cstdio>
#include <cmath>
static constexpr uint32_t Self=0x12345678;
struct Radio:mt::Radio {
 int state=0;unsigned sends=0;bool busy_=false;
 bool ready()override{return true;}int start()override{state=1;return 0;}int restore()override{state=1;return 0;}
 int stop()override{state=0;return 0;}int health()override{return state;}bool busy()override{return busy_;}
 uint32_t airtime(size_t)override{return 100;}int send(uint8_t*,size_t)override{++sends;return 0;}bool completion(int&)override{return false;}
};
struct Fixture {
 Radio radio;mt::test::KnownChannelFixtureClock clock;mt::Service service;mt::ModuleController modules;mt::PositionController positions;
 Fixture():service(radio,true,Self,127,true,false){
  clock.bind(service);auto c=service.config;c.bytes[4]=1;c.setTxEnabled(true);mt::GnssSettings g;g.enabled=true;g.smart=true;g.broadcastSecs=60;assert(c.setGnssSettings(g)&&service.apply(c));assert(service.start(false,0)&&service.armEndpoint(true));
 }
 void consent(unsigned precision=32){service.stop();auto c=service.config;assert(c.setPrimaryPositionPrecision(precision)&&service.apply(c));assert(service.start(false,0)&&service.armEndpoint(true));}
 void position(int32_t lat=450000000,int32_t lon=-930000000){meshtastic_Position p{};p.has_latitude_i=p.has_longitude_i=true;p.latitude_i=lat;p.longitude_i=lon;service.measuredPosition(p);}
 void poll(uint64_t now,bool ready=true){assert(clock.set(now));positions.poll(service,modules,ready,now,uint32_t(now)^0xabcdefu);}
 void clear(){service.stop();assert(!service.endpointControls());assert(service.start(false,uint32_t(clock.now()))&&service.armEndpoint(true));}
};
int main(){
 {
  Fixture f;f.position();f.poll(1000);assert(!f.service.endpointControls()&&!f.positions.schedule().lastSendMs());
  f.consent();f.service.measuredPosition({});f.poll(1001);assert(!f.service.endpointControls());f.position();f.poll(1002,false);assert(!f.service.endpointControls());
  f.poll(1003);assert(f.service.endpointControls()==1&&f.positions.schedule().lastSendMs()==1003&&!f.radio.sends);
  assert(f.positions.schedule().lastLatitude()==450000000&&f.positions.schedule().lastLongitude()==-930000000);
  puts("PASS production caller preserves consent/current-fix/readiness and queues first periodic without physical RF claims");
 }
 {
  Fixture f;f.consent();f.position();f.poll(1000);assert(f.positions.schedule().lastSendMs()==1000);f.clear();
  f.poll(601000);assert(!f.service.endpointControls()&&f.positions.schedule().lastSendMs()==1000);
  // Public default channel intentionally caps precision despite requesting32.
  f.position(451000000);f.poll(601001);assert(f.service.endpointControls()==1&&f.positions.schedule().lastSendMs()==601001);f.clear();
  f.position(451000001);f.poll(601002);assert(!f.service.endpointControls());
  puts("PASS unchanged fixes honor six-hour floor while a precision-cell move uses early smart cadence and public precision remains capped");
 }
 {
  Fixture f;f.consent();f.position();f.poll(1000);const auto stamp=f.positions.schedule().lastSendMs();f.clear();
  // Keep unrelated accepted module work pending. Replacing a measured fix
  // correctly cancels a stale position control, so that is not a busy oracle.
  uint8_t data[64];size_t size=0;uint32_t id=0;assert(mt::TelemetryAnnouncement::encode(302,{},{},data,sizeof(data),size));
  assert(!f.service.submitModuleAnnouncement(data,size,0,301999,0x98765432,id)&&id);
  f.position(460000000);f.poll(302000);assert(f.positions.schedule().lastSendMs()==stamp&&f.service.endpointControls()==1);
  f.clear();f.poll(302001);assert(!f.service.endpointControls()&&f.positions.schedule().lastSendMs()==stamp);
  f.poll(307000);assert(f.service.endpointControls()==1&&f.positions.schedule().lastSendMs()==307000);f.clear();
  f.service.measuredPosition({});f.poll(307001);assert(f.positions.schedule().lastSendMs()==307000&&!f.service.endpointControls());
  f.positions.poll(f.service,f.modules,true,100,77);assert(!f.service.endpointControls()&&f.positions.schedule().lastSendMs()==307000);
  f.position();f.service.channelMeter().supported(false);f.poll(308000);assert(!f.service.endpointControls());
  puts("PASS rejected queue, stale fix, backwards uptime and unavailable channel observation preserve prior send history");
 }
 {
  using mt::PositionDistance::meters;
  assert(meters(0,0,0,0)==0);assert(std::abs(meters(0,0,0.001,0)-111.10766f)<0.001f);
  const float crossing=meters(0,179.9,0,-179.9);assert(crossing>22200&&crossing<22230);
  assert(meters(60,0,60,0.001)>55&&meters(60,0,60,0.001)<56);
  // Existing midpoint transform decides both stationary and moved cells.
  assert(mt::PositionPrecision::truncateCoordinate(450000000,14u)==mt::PositionPrecision::truncateCoordinate(450000001,14u));
  puts("PASS literal stock trig-approx distance snapshots, antimeridian and reused precision midpoint cells");
 }
 puts("POSITION_RUNTIME_GROUPS 4");
}
