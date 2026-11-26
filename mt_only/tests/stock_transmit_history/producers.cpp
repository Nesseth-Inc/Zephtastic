// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include "ModuleController.h"
#include "gnss/PositionController.h"
#include "stock/telemetry/SensorCadence.h"
#include "stock/telemetry/SensorModules.h"
#include "telemetry/CompletionTelemetry.h"
#include <array>
#include <map>
#include <string>
using mt::history::Kind;using mt::history::Clock;using mt::history::TransmitHistory;
static Clock sample{};
struct Store final:mt::SettingsBackend {
  std::map<std::string,std::vector<uint8_t>> rows;
  int read(const char *k,uint8_t *p,size_t n)override{
    auto it=rows.find(k);if(it==rows.end())return -ENOENT;if(it->second.size()>n)return -E2BIG;
    memcpy(p,it->second.data(),it->second.size());return int(it->second.size());
  }
  int write(const char *k,const uint8_t *p,size_t n)override{rows[k]={p,p+n};return 0;}
};
static void bind(TransmitHistory &h,Store &store,Fixture &f){
  assert(!h.load(store,OURS,nullptr,[](){return sample;}));f.s.broadcastHistory=&h;
}
static void checkNodeInfo(){
  Store store;sample={3000,1800000000};Fixture f(true,81,true);TransmitHistory h;bind(h,store,f);
  f.s.stop();auto cfg=f.s.config;cfg.setTxEnabled(true);assert(f.s.apply(cfg)&&f.s.start(false,3000)&&f.s.armEndpoint(true));
  assert(!f.s.phoneNodeInfoPing(3000,333));assert(h.has(Kind::NodeInfo)&&!h.save(store,3000));
  Fixture boot(true,81,true);TransmitHistory recovered;sample={4000,1800000011};bind(recovered,store,boot);
  boot.s.stop();assert(boot.s.apply(cfg)&&boot.s.start(false,4000)&&boot.s.armEndpoint(true));
  assert(boot.s.phoneNodeInfoPing(4000,334)==-EAGAIN&&!boot.s.nodeDiscovery.queued);
  sample={53000,1800000060};assert(!boot.s.phoneNodeInfoPing(53000,335));assert(boot.s.nodeDiscovery.queued==1);
}
static void moduleReports(){
  Store store;Fixture f;TransmitHistory h;bind(h,store,f);
  mt::ModuleController modules;mt::ModuleSettings settings;meshtastic_ModuleConfig t{};
  t.which_payload_variant=meshtastic_ModuleConfig_telemetry_tag;auto &v=t.payload_variant.telemetry;
  v.device_telemetry_enabled=v.environment_measurement_enabled=v.power_measurement_enabled=true;
  v.device_update_interval=v.environment_update_interval=v.power_update_interval=3600;assert(settings.set(t));
  auto poll=[&](mt::ModuleController &m,Fixture &fixture,uint32_t ms,uint32_t utc){
    sample={ms,utc};fixture.channelNow=ms;
    m.pollTimed(fixture.s,settings,true,ms,12345,utc,[](const uint8_t*,size_t,uint32_t,bool){});
  };
  poll(modules,f,0,1800000000);poll(modules,f,30000,1800000030);
  assert(h.has(Kind::Device)&&!h.has(Kind::Environment)&&!h.has(Kind::Power));
  poll(modules,f,55000,1800000055);assert(h.has(Kind::Environment)&&!h.has(Kind::Power));
  poll(modules,f,70000,1800000070);assert(h.has(Kind::Power)&&!h.has(Kind::Air)&&!h.has(Kind::Health));
  assert(!h.save(store,70000));Fixture boot;TransmitHistory recovered;bind(recovered,store,boot);
  mt::ModuleController cold;
  poll(cold,boot,0,1800000100);poll(cold,boot,70000,1800000170);
  for(const auto k:{Kind::Device,Kind::Environment,Kind::Power}){
    assert(recovered.has(k)&&!recovered.allows(k,3600000));}
  assert(!recovered.due(70000)); // No reconfirmation or rewrite of recovered marks.
}
namespace mt::modules {
// The worker method and wire encoder below are included verbatim from the
// firmware by run.py. The only substituted surface is sensor acquisition.
struct TestSensors {
    bool ready_=true;
    stock::telemetry::SensorCadence airRf_,healthRf_;
    struct {
        telemetry::CompletionTelemetry::Snapshot value{};
        bool snapshot(uint64_t,telemetry::CompletionTelemetry::Snapshot &out){out=value;return true;}
    } telemetry_;
#define IS_ENABLED(symbol) 0
#include "sensor-owner.inc"
#undef IS_ENABLED
};
}
static void accessory(bool air,bool good){
    Store store;Fixture f(true,81,false);TransmitHistory h;bind(h,store,f);
    mt::ModuleController modules;mt::modules::TestSensors sensor;
    auto &schedule=air?sensor.airRf_:sensor.healthRf_;schedule.configure(true,3600,0,1000);
    auto &s=sensor.telemetry_.value;
    s.hasAir=good&&air;s.hasHealth=good&&!air;
    if(s.hasAir){s.air.has_pm25_standard=true;s.air.pm25_standard=5;}
    if(s.hasHealth){s.health.has_heart_bpm=true;s.health.heart_bpm=70;}
    auto poll=[&](unsigned ms){sample={ms,1800000000+ms/1000};f.channelNow=ms;
        sensor.ownerPoll(f.s,modules,ms,sample.utc,8,[](const uint8_t*,size_t,uint32_t,bool=false){});
    };
    poll(4000);
    assert(h.has(air?Kind::Air:Kind::Health)==(good||!air));
    assert(h.has(air?Kind::Health:Kind::Air)==false);
    assert((f.s.endpointControls()>0)==good);
}
static void position(){
    Store store;Fixture f;TransmitHistory h;bind(h,store,f);sample={60000,1800000000};
    f.s.stop();auto cfg=f.s.config;cfg.setTxEnabled(true);mt::GnssSettings g;g.enabled=true;g.broadcastSecs=1500;
    assert(cfg.setGnssSettings(g)&&cfg.setPrimaryPositionPrecision(14)&&f.s.apply(cfg)&&f.s.start(false,0)&&f.s.armEndpoint(true));
    meshtastic_Position pos{};pos.has_latitude_i=pos.has_longitude_i=true;pos.latitude_i=450000000;pos.longitude_i=-930000000;
    f.s.measuredPosition(pos);mt::PositionController first;mt::ModuleController m;
    f.channelNow=60000;first.poll(f.s,m,true,sample.uptimeMs,444);
    assert(h.has(Kind::Position)&&f.s.endpointControls()==1&&!h.save(store,60000));
    Fixture boot;TransmitHistory restored;sample={5000,1800000030};bind(restored,store,boot);
    boot.s.stop();assert(boot.s.apply(cfg)&&boot.s.start(false,0)&&boot.s.armEndpoint(true));boot.s.measuredPosition(pos);
    mt::PositionController next;next.restoreLastSend(restored.last(Kind::Position));
    boot.channelNow=5000;next.poll(boot.s,m,true,sample.uptimeMs,445);assert(!boot.s.endpointControls());
    assert(!next.schedule().lastLatitude()&&!next.schedule().lastLongitude()); // Position history is timestamp-only.
}
int main(){checkNodeInfo();moduleReports();position();for(auto air:{false,true})for(auto good:{false,true})accessory(air,good);(void)&group;(void)&results;
  std::puts("PASS real NodeInfo, Position, Device, Environment, Power, Air and Health owners; missing sensors, cold boot and 60s edge");}
