// SPDX-License-Identifier: GPL-3.0-only
#include "gnss/PositionController.h"
#include "modules/OperatingRole.h"
#include "power/PowerPolicy.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
constexpr unsigned default_telemetry_broadcast_interval_secs=3600, ONE_DAY=86400, MAX_INTERVAL=INT32_MAX;
struct StockState {
  struct {meshtastic_Config_PositionConfig position;meshtastic_Config_DeviceConfig device;} config{};
  struct {meshtastic_ModuleConfig_TelemetryConfig telemetry;meshtastic_ModuleConfig_NeighborInfoConfig neighbor_info;} moduleConfig{};
  meshtastic_User owner{};
  void apply(meshtastic_Config_DeviceConfig_Role role){
#include "donor.inc"
  }
};
static int64_t now;
int64_t k_uptime_get(){return now;}
namespace mt::power::ina {bool supports(uint32_t address){return !address;}}
using Power=mt::power::PowerPolicy;
static void power(){
 Power p;unsigned sleeps=0;
 p.setDeepSleepBackend({&sleeps,[](void*,uint32_t seconds){return seconds>0;},[](void*){return true;},[](void*){return true;},
   [](void *ctx,uint32_t seconds){assert(seconds==300);++*static_cast<unsigned*>(ctx);return 1;},[](void*){}});
 meshtastic_Config_PowerConfig cfg{};assert(p.configure(cfg,0));
 p.roleSleep(6,300,0);assert(p.poll(5000,{})==Power::State::Awake);
 cfg.is_power_saving=true;assert(p.configure(cfg,0));p.roleSleep(6,300,0);
 assert(p.poll(4999,{})==Power::State::Awake);Power::Activity busy{};busy.radio=true;
 assert(p.poll(5000,busy)==Power::State::Awake);
 now=6000;assert(p.poll(now,{})==Power::State::DeepSleepPending);
 assert(p.executeDeepSleep({})==Power::DeepSleepResult::TimerTick&&sleeps==1);
 now=306000;assert(p.poll(now,{})==Power::State::DeepSleepPending);
 assert(p.executeDeepSleep({})==Power::DeepSleepResult::TimerTick&&p.state()==Power::State::Awake);
 p.roleSleep(6,300,now);p.roleSleep(0,0,now+1);assert(p.poll(now+5000,{})!=Power::State::DeepSleepPending);
 p.roleSleep(5,300,now+6000);busy={};busy.bluetooth=true;assert(p.poll(now+11000,busy)==Power::State::Awake);
 assert(p.poll(now+12000,{})==Power::State::LightSleepEligible);
 assert(p.poll(now+800000,{})==Power::State::LightSleepEligible);
}
static void projections(){
 for(unsigned r:{5u,6u,7u,8u,9u}){
  const auto role=static_cast<meshtastic_Config_DeviceConfig_Role>(r);
  mt::Config c;assert(c.setDeviceRole(r,0));auto gps=c.gnssSettings();gps.smart=true;gps.broadcastSecs=121;
  assert(c.setGnssSettings(gps));mt::ModuleSettings modules;meshtastic_ModuleConfig t{};
  assert(modules.get(t,meshtastic_ModuleConfig_telemetry_tag));auto &telemetry=t.payload_variant.telemetry;
  telemetry.device_update_interval=123;telemetry.environment_update_interval=124;assert(modules.set(t));
  StockState stock{};stock.config.position.position_broadcast_smart_enabled=true;stock.config.position.position_broadcast_secs=121;
  stock.config.device.node_info_broadcast_secs=c.node().interval();stock.config.position.position_flags=gps.positionFlags;
  stock.moduleConfig.telemetry=telemetry;stock.apply(role);
  auto full=t;assert(mt::modules::OperatingRole::project(c,modules,&full));
  assert(c.gnssSettings().positionFlags==stock.config.position.position_flags);
  assert(c.node().interval()==stock.config.device.node_info_broadcast_secs);
  assert(c.rebroadcastMode()==unsigned(stock.config.device.rebroadcast_mode));
  meshtastic_ModuleConfig neighbor{};assert(modules.get(neighbor,meshtastic_ModuleConfig_neighbor_info_tag));
  assert(neighbor.payload_variant.neighbor_info.update_interval==stock.moduleConfig.neighbor_info.update_interval);
  assert(full.payload_variant.telemetry.air_quality_interval==stock.moduleConfig.telemetry.air_quality_interval);
  assert(full.payload_variant.telemetry.health_update_interval==stock.moduleConfig.telemetry.health_update_interval);
  mt::Config restored;assert(mt::Config::decode(c.bytes,c.Size,restored)&&restored.same(c));
  assert(modules.get(t,meshtastic_ModuleConfig_telemetry_tag));
  auto expected=stock.moduleConfig.telemetry;expected.air_quality_interval=expected.health_update_interval=0;
  assert(!memcmp(&telemetry,&expected,sizeof(telemetry)));
  assert(c.ownerUnmessagable()==stock.owner.is_unmessagable);
  assert(c.gnssSettings().smart==stock.config.position.position_broadcast_smart_enabled);
  assert(c.gnssSettings().broadcastSecs==stock.config.position.position_broadcast_secs);
 }
}
static void coordinates(){
 uint32_t state=2342;
 for(unsigned n=0;n<100000;++n){
  state=state*1664525+1013904223;
  const int32_t wire=int32_t(int64_t(state)%3600000001LL-1800000000);
  char stock[18],ported[18];snprintf(stock,sizeof(stock),"%f",double(wire)*1e-7);
  mt::stock::position::coordinate6(wire,ported);assert(!strcmp(stock,ported));
 }
}
static constexpr uint32_t Self=0x12345678;
struct PositionRadio:mt::Radio {
 int state=0;unsigned sends=0;bool busy_=false,done=false;bench::Frame frames[2]{};
 bool ready()override{return true;}int start()override{state=1;return 0;}int restore()override{state=1;return 0;}
 int stop()override{state=0;return 0;}int health()override{return state;}bool busy()override{return busy_;}
 uint32_t airtime(size_t)override{return 100;}int send(uint8_t *bytes,size_t size)override{assert(sends<2);auto &f=frames[sends++];f.len=size;memcpy(f.bytes,bytes,size);return 0;}
 bool completion(int &result)override{result=0;const bool complete=done;done=false;return complete;}
};
struct PositionFixture {
 PositionRadio radio;mt::test::KnownChannelFixtureClock clock;mt::Service service;mt::ModuleController modules;mt::PositionController positions;
 PositionFixture():service(radio,true,Self,127,true,false){
  clock.bind(service);auto c=service.config;c.bytes[4]=1;c.setTxEnabled(true);mt::GnssSettings g;g.enabled=true;g.smart=true;g.broadcastSecs=60;assert(c.setGnssSettings(g)&&service.apply(c));assert(service.start(false,0)&&service.armEndpoint(true));
 }
 void consent(unsigned precision=32){service.stop();auto c=service.config;assert(c.setPrimaryPositionPrecision(precision)&&service.apply(c));assert(service.start(false,0)&&service.armEndpoint(true));}
 void position(int32_t lat=450000000,int32_t lon=-930000000){meshtastic_Position p{};p.has_latitude_i=p.has_longitude_i=true;p.latitude_i=lat;p.longitude_i=lon;service.measuredPosition(p);}
 void poll(uint64_t now,bool ready=true){assert(clock.set(now));positions.poll(service,modules,ready,now,uint32_t(now)^0xabcdefu);}
 void clear(){service.stop();assert(!service.endpointControls());assert(service.start(false,uint32_t(clock.now()))&&service.armEndpoint(true));}
};

static void positionCases(){
 PositionFixture f;f.consent(14);auto start=[&](unsigned role){
  f.service.stop();auto cfg=f.service.config;assert(cfg.setDeviceRole(role,0));auto g=cfg.gnssSettings();
  g.smart=false;g.broadcastSecs=300;assert(cfg.setGnssSettings(g)&&f.service.apply(cfg));
  assert(f.service.start(false,0)&&f.service.armEndpoint(true));};
 start(9);f.position();const auto free=f.service.endpointFree();f.poll(1000);
 assert(f.positions.schedule().lastSendMs()==1000&&f.service.endpointControls()==1&&f.service.endpointFree()+1==free);
 f.service.measuredPosition({});assert(!f.service.endpointControls()&&f.service.endpointFree()==free);
 assert(!f.modules.takeRoleSleep());f.clear();f.positions={};start(5);f.position();f.poll(6500);
 assert(f.service.endpointControls()&&f.service.endpointFree()==free&&f.modules.takeRoleSleep()==300);
}
static void priorityAndCarrier(){
 for(bool priority:{false,true}){
  mt::Endpoint ep;bench::Frame a{},b{};a.len=b.len=17;a.bytes[8]=1;b.bytes[8]=2;
  assert(!ep.submitPrivateControl(a,0,0)&&!ep.submitModuleControl(b,0,false,false,false,false,0,false,priority));
  const auto *first=ep.next(251);assert(first&&first->frame.bytes[8]==(priority?2:1)&&!first->reliable);
 }
 PositionFixture f;f.service.stop();auto cfg=f.service.config;assert(cfg.setDeviceRole(7,0)&&f.service.apply(cfg));
 assert(f.service.start(false,0)&&f.service.armEndpoint(true));const uint8_t bytes[]={0xff,0,0x91,0x81,0x12};
 uint8_t data[32];size_t size=0;assert(mt::PublicApplication::encode(meshtastic_PortNum_ATAK_PLUGIN_V2,bytes,sizeof(bytes),0,0,data,sizeof(data),size));
 assert(!f.service.submitPublicApplication(0,UINT32_MAX,345,data,size,2,false,100));
 for(uint32_t t=100;t<=6000&&!f.radio.sends;t+=100){assert(f.clock.set(t));f.service.poll(t,65);}
 assert(f.radio.sends==1);bench::RouteMetadata decoded{};assert(f.service.codec.decode(f.radio.frames[0].bytes,f.radio.frames[0].len,decoded));
 assert(decoded.portnum==meshtastic_PortNum_ATAK_PLUGIN_V2&&decoded.payloadLength==sizeof(bytes)&&!memcmp(decoded.payload,bytes,sizeof(bytes)));
}
void adminCases();
int main(){adminCases();priorityAndCarrier();projections();coordinates();power();positionCases();puts("PASS pinned role defaults, position notification, precision/revocation, report sleep and queue admission");}
