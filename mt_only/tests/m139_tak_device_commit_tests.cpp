// SPDX-License-Identifier: GPL-3.0-only
#define CONFIG_MT_STORE_FORWARD_PSRAM 1
#include "DeviceSettingsCommit.h"
#include "modules/RouterRoleDefaults.h"
#include "notifications/NotificationEdit.h"
#include "notifications/NotificationJournal.h"
#include "modules/StoreForwardEdit.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using Bytes=std::vector<uint8_t>;
static constexpr uint32_t Self=0x12345678;

struct Backend:mt::SettingsBackend {
  std::map<std::string,Bytes> files;
  std::vector<std::string> writes;
  std::string failPrefix;
  std::string failReadPrefix;
  bool uncertain=false;
  int read(const char *key,uint8_t *out,size_t cap)override {
    if(!failReadPrefix.empty()&&std::string(key).find(failReadPrefix)==0)return -EIO;
    auto it=files.find(key);if(it==files.end())return -ENOENT;
    if(it->second.size()>cap)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());return int(it->second.size());
  }
  int write(const char *key,const uint8_t *data,size_t n)override {
    writes.emplace_back(key);
    const bool fail=!failPrefix.empty()&&std::string(key).find(failPrefix)==0;
    if(!fail||uncertain)files[key]=Bytes(data,data+n);
    return fail?-EIO:0;
  }
};
struct Radio:mt::Radio {
  int state=0;unsigned sends=0,starts=0,failStartAt=0;bool busy_=false;
  bool ready()override{return true;}int start()override{++starts;if(failStartAt&&starts==failStartAt)return -EIO;state=1;return 0;}
  int restore()override{state=1;return 0;}int stop()override{state=0;return 0;}
  int health()override{return state;}bool busy()override{return busy_;}
  int stopForStorage()override{return stop();}uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t*,size_t)override{++sends;return 0;}bool completion(int&)override{return false;}
};
struct Fixture {
  Radio radio;Backend backend;mt::test::KnownChannelFixtureClock clock;
  mt::Service service;mt::SettingsJournal coreJournal;mt::SettingsEdit core;
  mt::NodeJournal nodes;mt::NotificationJournal notifyJournal;mt::NotificationEdit led;
  mt::ModuleJournal moduleJournal;mt::ModuleEdit modules;
  mt::modules::StoreForwardJournal storeForwardJournal;mt::modules::StoreForwardEdit storeForward;
  mt::SettingsCommit commit;
  Fixture():service(radio,true,Self,127,true,false),core(service,coreJournal,backend),
    led(service,notifyJournal,backend),modules(service,moduleJournal,backend),
    storeForward(service,storeForwardJournal,backend),
    commit(service,radio,core,nodes,backend,&modules,&led) {
    clock.bind(service);
    assert(!coreJournal.load(backend)&&!notifyJournal.load(backend)&&led.load());
    assert(!moduleJournal.load(backend)&&modules.load());
    assert(!storeForwardJournal.load(backend)&&storeForward.load());commit.storeForward(storeForward);
    assert(!nodes.load(backend,service.nodes,Self));
    auto initial=coreJournal.config();initial.bytes[4]=1;initial.setTxEnabled(true);
    assert(initial.setNodeInfoSeconds(3600));
    assert(!coreJournal.save(backend,initial)&&service.apply(initial));
    assert(service.start(false,0)&&service.armEndpoint(true));
    backend.writes.clear();
  }
  mt::Config coreCandidate()const {
    auto c=service.config;assert(c.setNodeInfoSeconds(7200));return c;
  }
  meshtastic_ModuleConfig telemetry(uint32_t seconds)const {
    meshtastic_ModuleConfig c{};
    assert(modules.config().get(c,meshtastic_ModuleConfig_telemetry_tag));
    c.payload_variant.telemetry.device_telemetry_enabled=true;
    c.payload_variant.telemetry.device_update_interval=seconds;
    return c;
  }
  meshtastic_ModuleConfig storeForwardConfig(bool enabled,bool server)const {
    meshtastic_ModuleConfig c{};c.which_payload_variant=meshtastic_ModuleConfig_store_forward_tag;
    assert(storeForward.config().get(c.payload_variant.store_forward));
    c.payload_variant.store_forward.enabled=enabled;c.payload_variant.store_forward.is_server=server;return c;
  }
  int save(const mt::Config &candidate,const meshtastic_ModuleConfig *module=nullptr,
           bool disabled=true,uint32_t now=10) {
    if(module)return mt::DeviceSettingsCommit::save(service,core,led,commit,candidate,disabled,true,now,&modules,module);
    return mt::DeviceSettingsCommit::save(service,core,led,commit,candidate,disabled,true,now);
  }
  int saveFullModules(const mt::Config &candidate,const mt::ModuleSettings &projected,
                      bool disabled=true,uint32_t now=10) {
    return mt::DeviceSettingsCommit::save(service,core,led,commit,candidate,disabled,true,now,
                                          &modules,nullptr,&projected);
  }
};
static void assertDurableCombined(Fixture &f,const mt::Config &expectedCore,uint32_t interval,
                                  bool telemetryEnabled=true) {
  mt::SettingsJournal core;mt::NotificationJournal notifications;mt::ModuleJournal modules;
  assert(!core.load(f.backend)&&!notifications.load(f.backend)&&!modules.load(f.backend));
  assert(core.config().same(expectedCore));
  meshtastic_ModuleConfig telemetry{};assert(modules.config().get(telemetry,meshtastic_ModuleConfig_telemetry_tag));
  assert(telemetry.payload_variant.telemetry.device_telemetry_enabled==telemetryEnabled);
  assert(telemetry.payload_variant.telemetry.device_update_interval==interval);
}
static mt::Config stageDeprecatedAlias(Fixture &f,uint32_t now) {
  auto candidate=f.service.config;assert(candidate.setDeviceRole(mt::DeviceRolePolicy::Role::CLIENT,
    candidate.rebroadcastMode()));
  assert(!f.core.begin(now));
  auto store=f.storeForwardConfig(true,true);
  assert(!f.storeForward.save(store,now,true));
  assert(!f.save(candidate,nullptr,true,now));
  return candidate;
}
int main() {
  {
    Fixture f;const auto c=f.coreCandidate();
    assert(!f.save(c));
    assert(f.backend.writes.size()==1&&f.backend.writes[0].find("mtbase/state")==0);
    assert(!f.core.open()&&!f.modules.dirty()&&f.service.receiving()&&f.service.endpointArmed());
    const auto writeCount=f.backend.writes.size();const auto same=f.service.config;
    meshtastic_ModuleConfig unchanged{};assert(f.modules.config().get(unchanged,meshtastic_ModuleConfig_telemetry_tag));
    assert(!f.save(same,&unchanged)&&f.backend.writes.size()==writeCount);
    puts("PASS legacy DeviceSettingsCommit signature and null/unchanged module fast paths");
  }
  {
    Fixture f;const auto old=f.service.config;const auto candidate=f.coreCandidate();const auto module=f.telemetry(900);
    assert(!f.core.begin(1));const auto before=f.backend.writes.size();
    assert(!f.save(candidate,&module,true,2));
    assert(f.core.open()&&f.core.dirty()&&f.modules.dirty()&&f.backend.writes.size()==before);
    assert(f.service.config.same(candidate)&&f.service.receiving());
    assert(!f.commit.commit(3));
    assert(!f.core.open()&&!f.core.dirty()&&!f.modules.dirty());
    assertDurableCombined(f,candidate,900);assert(f.service.receiving()&&f.service.endpointArmed()&&!f.radio.sends);
    assert(old.node().interval()==3600);
    puts("PASS pre-opened core edit stages module and core without I/O then coordinator verifies both");
  }
  {
    Fixture f;const auto candidate=f.coreCandidate();const auto module=f.telemetry(1200);
    assert(!f.save(candidate,&module,false,10));
    assert(!f.core.open()&&!f.modules.dirty()&&f.backend.writes.size()==3);
    assert(f.backend.writes[0].find("mtmod/state")==0&&f.backend.writes[1].find("mtnotify/state")==0&&f.backend.writes[2].find("mtbase/state")==0);
    assertDurableCombined(f,candidate,1200);
    mt::NotificationJournal durableLed;assert(!durableLed.load(f.backend)&&!durableLed.config().heartbeatDisabled());assert(f.service.receiving()&&f.service.endpointArmed()&&!f.radio.sends);
    puts("PASS immediate coordinated module-before-core verified persistence and normal receive restoration");
  }
  {
    Fixture f;const auto old=f.service.config;auto requested=old;
    assert(requested.setDeviceRole(mt::DeviceRolePolicy::Role::ROUTER,
                                   mt::DeviceRolePolicy::RebroadcastMode::ALL));
    assert(requested.setNodeInfoSeconds(10800));
    mt::modules::RouterRoleDefaults::Projection projected{};
    assert(mt::modules::RouterRoleDefaults::project(old.role(),requested,f.modules.config(),projected)==
           mt::modules::RouterRoleDefaults::Result::Applied);
    const auto before=f.backend.writes.size();
    assert(!f.saveFullModules(projected.core,projected.modules,true,10));
    assert(f.backend.writes.size()>before&&f.backend.writes[before].find("mtmod/")==0);
    assert(!f.core.open()&&!f.modules.dirty()&&f.service.receiving()&&f.service.endpointArmed());
    assertDurableCombined(f,projected.core,43200,false);
    mt::ModuleJournal durable;assert(!durable.load(f.backend));
    meshtastic_ModuleConfig telemetry{},neighbor{};
    assert(durable.config().get(telemetry,meshtastic_ModuleConfig_telemetry_tag));
    assert(durable.config().get(neighbor,meshtastic_ModuleConfig_neighbor_info_tag));
    assert(telemetry.payload_variant.telemetry.environment_update_interval==0);
    assert(neighbor.payload_variant.neighbor_info.update_interval==0);
    assert(projected.core.node().interval()==10800&&
           projected.core.rebroadcastMode()==mt::DeviceRolePolicy::RebroadcastMode::CORE_PORTNUMS_ONLY);
    puts("PASS full ROUTER telemetry+neighbor projection persists through existing ordered coordinator");
  }
  {
    Fixture f;const auto old=f.service.config;auto requested=old;
    assert(requested.setDeviceRole(mt::DeviceRolePolicy::Role::ROUTER,
                                   mt::DeviceRolePolicy::RebroadcastMode::ALL));
    mt::modules::RouterRoleDefaults::Projection projected{};
    assert(mt::modules::RouterRoleDefaults::project(old.role(),requested,f.modules.config(),projected)==
           mt::modules::RouterRoleDefaults::Result::Applied);
    f.backend.failPrefix="mtmod/";
    assert(f.saveFullModules(projected.core,projected.modules,true,10)==-EIO);
    assert(f.backend.writes.size()==1&&f.backend.writes[0].find("mtmod/")==0);
    assert(f.core.open()&&f.core.dirty()&&f.modules.dirty());
    assert(!f.service.receiving()&&!f.service.endpointArmed()&&!f.radio.sends);
    puts("PASS full ROUTER module-journal failure remains visible and keeps RF paused before core write");
  }
  {
    Fixture f;const auto old=f.service.config;const auto candidate=f.coreCandidate();const auto module=f.telemetry(1800);
    f.backend.failPrefix="mtmod/";
    assert(f.save(candidate,&module,true,10)==-EIO);
    assert(f.core.open()&&f.core.dirty()&&f.modules.dirty()&&!f.coreJournal.config().same(candidate));
    assert(f.backend.writes.size()==1&&f.backend.writes[0].find("mtmod/state")==0);
    assert(!f.service.receiving()&&!f.service.endpointArmed()&&!f.radio.sends);
    const auto writes=f.backend.writes.size();assert(f.commit.commit(11)<0&&f.backend.writes.size()==writes);
    mt::SettingsJournal reloaded;assert(!reloaded.load(f.backend)&&reloaded.config().same(old));
    puts("PASS module failure holds radio/core persistence and is not retried");
  }
  {
    Fixture f;const auto old=f.service.config;const auto candidate=f.coreCandidate();const auto module=f.telemetry(2400);
    f.backend.failPrefix="mtbase/state";
    assert(f.save(candidate,&module,true,10)==-EIO);
    assert(f.core.open()&&!f.coreJournal.ready()&&f.coreJournal.config().same(old));
    assert(!f.modules.dirty()&&f.backend.writes.size()==2);
    assert(f.backend.writes[0].find("mtmod/state")==0&&f.backend.writes[1].find("mtbase/state")==0);
    mt::ModuleJournal durableModule;assert(!durableModule.load(f.backend));
    meshtastic_ModuleConfig actual{};assert(durableModule.config().get(actual,meshtastic_ModuleConfig_telemetry_tag));
    assert(actual.payload_variant.telemetry.device_update_interval==2400);
    const auto writes=f.backend.writes.size();assert(f.commit.commit(11)<0&&f.backend.writes.size()==writes);
    mt::SettingsJournal durableCore;assert(!durableCore.load(f.backend)&&durableCore.config().same(old));
    assert(!f.radio.sends);
    puts("PASS later core failure propagates after module commit without claiming cross-journal atomicity or retry");
  }
  {
    Fixture f;const auto old=f.service.config;auto role=old;assert(role.setDeviceRole(11,0));
    const auto telemetry=f.telemetry(86400);
    assert(!f.save(role,&telemetry,true,10));
    assert(f.service.config.role()==11&&f.service.receiving()&&f.service.endpointArmed());
    assertDurableCombined(f,role,86400);
    puts("PASS Router Late role and daily telemetry defaults persist through the existing verified multi-journal coordinator");
  }
  {
    Fixture f;auto role=f.service.config;assert(role.setDeviceRole(11,0));
    const auto telemetry=f.telemetry(86400);f.backend.failPrefix="mtmod/";
    assert(f.save(role,&telemetry,true,10)==-EIO);
    assert(f.core.open()&&f.core.dirty()&&f.modules.dirty()&&f.backend.writes.size()==1);
    assert(!f.service.receiving()&&!f.service.endpointArmed()&&!f.radio.sends);
    mt::SettingsJournal durable;assert(!durable.load(f.backend)&&durable.config().role()!=11);
    puts("PASS Router Late module-default write failure is returned without committing the core role or restoring RF");
  }
  {
    Fixture f;auto role=f.service.config;assert(role.setDeviceRole(11,0));
    const auto telemetry=f.telemetry(86400);f.backend.failReadPrefix="mtmod/state0";
    assert(f.save(role,&telemetry,true,10)==-EIO);
    assert(f.core.open()&&f.modules.dirty()&&!f.service.receiving()&&!f.service.endpointArmed());
    mt::SettingsJournal durable;assert(!durable.load(f.backend)&&durable.config().role()!=11&&!f.radio.sends);
    puts("PASS Router Late module readback failure is uncertain, fails the setter, and prevents core commit or RF resume");
  }
  {
    Fixture f;auto role=f.service.config;assert(role.setDeviceRole(11,0));
    const auto telemetry=f.telemetry(86400);f.backend.failPrefix="mtbase/state";
    assert(f.save(role,&telemetry,true,10)==-EIO);
    assert(f.core.open()&&!f.coreJournal.ready()&&!f.service.receiving()&&!f.service.endpointArmed());
    f.backend.failReadPrefix.clear();
    mt::ModuleJournal durable;assert(!durable.load(f.backend));meshtastic_ModuleConfig actual{};
    assert(durable.config().get(actual,meshtastic_ModuleConfig_telemetry_tag));
    assert(actual.payload_variant.telemetry.device_update_interval==86400&&!f.radio.sends);
    puts("PASS Router Late core-write failure reports the documented module-first partial commit and leaves RF paused");
  }
  {
    Fixture f;const auto old=f.service.config;auto role=old;assert(role.setDeviceRole(11,0));
    const auto telemetry=f.telemetry(86400);f.backend.failReadPrefix="mtbase/state1";
    assert(f.save(role,&telemetry,true,10)==-EIO);
    assert(f.core.open()&&!f.coreJournal.ready()&&!f.service.receiving()&&!f.service.endpointArmed());
    f.backend.failReadPrefix.clear();
    mt::ModuleJournal durableModule;assert(!durableModule.load(f.backend));meshtastic_ModuleConfig actual{};
    assert(durableModule.config().get(actual,meshtastic_ModuleConfig_telemetry_tag));
    mt::SettingsJournal durableCore;assert(!durableCore.load(f.backend));
    assert(durableCore.config().role()==old.role()||durableCore.config().role()==11);
    assert(actual.payload_variant.telemetry.device_update_interval==86400&&!f.radio.sends);
    puts("PASS Router Late core readback failure preserves the explicit module-first partial state and keeps RF paused");
  }
  {
    Fixture f;auto role=f.service.config;assert(role.setDeviceRole(11,0));
    const auto telemetry=f.telemetry(86400);f.radio.failStartAt=f.radio.starts+3;
    assert(f.save(role,&telemetry,true,10)==-EIO);
    assert(!f.core.open()&&!f.modules.dirty()&&!f.service.receiving()&&!f.service.endpointArmed()&&!f.radio.sends);
    assertDurableCombined(f,role,86400);
    puts("PASS Router Late durable writes with failed service restoration return error and keep RF disarmed");
  }
  {
    Fixture f;auto alreadyClient=f.service.config;assert(alreadyClient.setDeviceRole(0,alreadyClient.rebroadcastMode()));
    assert(!f.core.save(alreadyClient,1));auto enabled=f.storeForwardConfig(true,false);
    assert(!f.storeForward.save(enabled,2,false));
    f.backend.writes.clear();const auto candidate=stageDeprecatedAlias(f,3);
    assert(f.storeForward.dirty()&&f.core.open());
    assert(!f.commit.commit(4));
    meshtastic_ModuleConfig stored{};mt::modules::StoreForwardJournal durableStore;
    assert(!durableStore.load(f.backend)&&durableStore.config().get(stored.payload_variant.store_forward));
    assert(stored.payload_variant.store_forward.enabled&&stored.payload_variant.store_forward.is_server);
    mt::SettingsJournal durableCore;assert(!durableCore.load(f.backend)&&durableCore.config().same(candidate));
    assert(f.service.receiving()&&f.service.endpointArmed()&&!f.radio.sends);
    puts("PASS deprecated role alias stages and verifies Store & Forward server promotion with canonical CLIENT core state");
  }
  {
    Fixture f;auto enabled=f.storeForwardConfig(true,false);assert(!f.storeForward.save(enabled,1,false));
    const auto old=f.service.config;f.backend.writes.clear();(void)stageDeprecatedAlias(f,2);
    f.backend.failPrefix="mtsf/state";
    assert(f.commit.commit(3)==-EIO&&f.core.open()&&!f.service.receiving()&&!f.service.endpointArmed());
    f.backend.failPrefix.clear();mt::SettingsJournal durableCore;
    assert(!durableCore.load(f.backend)&&durableCore.config().same(old)&&!f.radio.sends);
    puts("PASS deprecated role alias returns failure on Store & Forward persistence error without committing CLIENT or resuming RF");
  }
  {
    Fixture f;auto enabled=f.storeForwardConfig(true,false);assert(!f.storeForward.save(enabled,1,false));
    const auto old=f.service.config;f.backend.writes.clear();(void)stageDeprecatedAlias(f,2);
    f.backend.failPrefix="mtbase/state";
    assert(f.commit.commit(3)==-EIO&&f.core.open()&&!f.service.receiving()&&!f.service.endpointArmed());
    f.backend.failPrefix.clear();mt::modules::StoreForwardJournal durableStore;
    assert(!durableStore.load(f.backend));meshtastic_ModuleConfig stored{};
    assert(durableStore.config().get(stored.payload_variant.store_forward));
    mt::SettingsJournal durableCore;assert(!durableCore.load(f.backend)&&durableCore.config().same(old));
    assert(stored.payload_variant.store_forward.enabled&&stored.payload_variant.store_forward.is_server&&!f.radio.sends);
    puts("PASS deprecated role alias reports module-first partial commit on core failure with RF still paused");
  }
  {
    Fixture f;const auto old=f.service.config;const auto module=f.telemetry(999);
    assert(mt::DeviceSettingsCommit::save(f.service,f.core,f.led,f.commit,old,true,true,1,nullptr,&module)==-EINVAL);
    auto bad=module;bad.which_payload_variant=meshtastic_ModuleConfig_canned_message_tag;
    assert(f.save(old,&bad,false,2)<0&&f.backend.writes.empty()&&f.service.config.same(old));
    puts("PASS invalid optional argument pairs/modules reject before mutation or writes");
  }
  puts("M139_DEVICE_COMMIT_GROUPS 12");
}
