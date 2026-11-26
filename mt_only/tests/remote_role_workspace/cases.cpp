// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
#include "admin/RemoteDeviceTransaction.h"
#include "modules/CompletionConfig.h"
#include "modules/CompletionEdit.h"
#include "modules/OperatingRole.h"
#include "modules/RouterLateRoleDefaults.h"
#include "modules/RouterRoleDefaults.h"
#include "ModuleEdit.h"
#include "notifications/NotificationEdit.h"
#include "network/MqttGatewayJournal.h"
#include "network/MqttGatewayConfigMapping.h"
#include "network/MqttRegionRootPolicy.h"
#include "monocypher.h"
#include <algorithm>

using Bundle=mt::admin::RemoteDeviceBundle;
using Transaction=mt::admin::RemoteDeviceTransaction;
static Fixture fixture(127,false);
static auto &settings=fixture.backend;
static auto &service=fixture.service;
static auto &radio=fixture.radio;
static auto &settingsEdit=fixture.edit;
static auto &journal=fixture.journal;
static mt::ModuleJournal moduleJournal;
static mt::modules::CompletionJournal completionJournal;
static mt::NotificationJournal notificationJournal;
static mt::network::MqttGatewayJournal mqttJournal;
static mt::ModuleEdit moduleEdit(service,moduleJournal,settings);
static mt::modules::CompletionEdit completionEdit(service,completionJournal,settings);
static mt::NotificationEdit notificationEdit(service,notificationJournal,settings);
static mt::modules::ExtendedModuleSettings extended{};
static Transaction::Domain preferenceDomain;
static bool storageReady=true,preferenceDomainReady=true,remoteConfigHeld=false;
static int settingsStartupResult=0;
static bool preferenceLockOwned(){return true;}
static bool persistentWorkPending(){return false;}
namespace mt::notification {bool available(){return true;}}
static struct {
  bool accept=true;
  bool supports(const meshtastic_ModuleConfig &)const{return accept;}
  bool supportsPower(const meshtastic_Config_PowerConfig &)const{return accept;}
} completionRuntime;
static struct {
  bool writable=true;
  bool ready()const{return mqttJournal.ready();}
  bool remotePolicyWritable()const{return ready()&&writable;}
} mqttAdmin;

#define MT_WIFI_PSRAM_BSS(name)
#include "journal.inc"
static_assert(sizeof(RemoteDeviceWork)==std::max(sizeof(RemoteSectionScratch),sizeof(RemoteDeviceProjectionScratch)));
static_assert(sizeof(RemoteSectionScratch)<sizeof(RemoteDeviceWork),"exercise the section's uncovered tail");
static_assert(alignof(RemoteDeviceWork)>=alignof(RemoteDeviceProjectionScratch));
static bool allZero(const void *p,size_t n){
  const auto *bytes=static_cast<const uint8_t *>(p);
  return std::all_of(bytes,bytes+n,[](uint8_t b){return b==0;});
}
static void poison(){
  // Include the bytes past the end of the smaller section. Each callback acquires the union anew.
  memset(static_cast<void*>(&remoteDeviceWork),0x9d,sizeof(remoteDeviceWork));
}
static void checkClear(){assert(allZero(&remoteDeviceWork,sizeof(remoteDeviceWork)));}

struct TestHost:Host {
  TestHost():Host(settingsEdit,::journal){}
  unsigned commits=0;
  bool moduleConfig(meshtastic_ModuleConfig &wire,unsigned tag)const override{
    return mt::modules::completionConfig(completionEdit.config(),moduleEdit.config(),notificationEdit.config(),extended,wire,tag);
  }
  bool powerConfig(meshtastic_Config_PowerConfig &out)const override{
    out={};(void)completionEdit.config().power(out);return true;
  }
  int saveRemoteConfig(const mt::Config &candidate,uint32_t now)override{
    // At the production callback boundary the projection must already be gone.
    checkClear();assert(remoteDevicePreparing&&candidate.same(remoteDeviceCandidate.core));
    const auto snapshot=remoteDeviceCandidate;++commits;
    bool durable=false;uint32_t generation=0;
    const int rc=settingsEdit.persistForRemoteReply(candidate,now,durable,generation,persistRemoteDevice,nullptr);
    checkClear();assert(!memcmp(&snapshot,&remoteDeviceCandidate,sizeof(snapshot)));
    if(!rc)assert(durable&&generation==journal.generation()&&remoteDeviceJournalsMatch());
    return rc;
  }
#define CONFIG_MT_RF_ADMIN_GET 1
#include "setters.inc"
#undef CONFIG_MT_RF_ADMIN_GET
};

static void reset(){
  service.stop();settings.entries.clear();settings.fail=false;remoteConfigHeld=remoteDevicePending=false;
  assert(!journal.load(settings)&&!moduleJournal.load(settings)&&!completionJournal.load(settings));
  assert(!notificationJournal.load(settings)&&!mqttJournal.load(settings));
  assert(moduleEdit.load()&&completionEdit.load()&&notificationEdit.load());
  assert(service.apply(journal.config()));preferenceDomain.node=Node;preferenceDomain.publicKey[0]=0x56;
}
static void sections(){
  reset();Bundle sample{};
  for(unsigned section=0;section<Bundle::SectionCount;++section){
    size_t length=0;const uint8_t *bytes=sample.section(section,length);
    poison();assert(!roleValidate(nullptr,section,bytes,length));checkClear();
    poison();assert(roleValidate(nullptr,section,bytes,length-1)==-EINVAL);checkClear();
    poison();assert(!roleSave(nullptr,section,bytes,length));checkClear();
    poison();assert(roleSave(nullptr,section,nullptr,length)==-EINVAL);checkClear();
  }
  poison();assert(&remoteDeviceWork.beginSection()==static_cast<void*>(&remoteDeviceWork));
  auto &projection=remoteDeviceWork.beginProjection();
  assert(&projection==static_cast<void*>(&remoteDeviceWork));
  // Value initialization resets protobuf fields. Padding/inactive union bytes
  // need the explicit whole-arena wipe checked after every acquired scope.
  uint8_t output[512]{};auto wire=pb_ostream_from_buffer(output,sizeof(output));
  assert(pb_encode(&wire,meshtastic_ModuleConfig_fields,&projection.wire)&&!wire.bytes_written);
  assert(pb_encode(&wire,meshtastic_Config_PowerConfig_fields,&projection.power)&&!wire.bytes_written);
  size_t length=0;const uint8_t *core=sample.section(0,length);
  poison();assert(!roleValidate(nullptr,0,core,length));checkClear();
  auto changed=journal.config();assert(changed.setDisplay(45,true));
  settings.fail=true;poison();assert(roleSave(nullptr,0,changed.bytes,sizeof(changed.bytes))==-EIO);checkClear();
  settings.fail=false;assert(!journal.load(settings)&&!journal.config().same(changed));
  poison();assert(!roleSave(nullptr,0,changed.bytes,sizeof(changed.bytes)));checkClear();
  assert(journal.config().same(changed));
}
static void roles(){
  for(auto role:{mt::DeviceRolePolicy::Role::ROUTER,mt::DeviceRolePolicy::Role::ROUTER_LATE,
                 mt::DeviceRolePolicy::Role::CLIENT_HIDDEN}){
    reset();TestHost host;mt::Config value=service.config;
    assert(value.setDeviceRole(role,mt::DeviceRolePolicy::RebroadcastMode::ALL));
    // Force a real error inside the projection block, then retry against the same workspace.
    if(role==mt::DeviceRolePolicy::Role::ROUTER){
      completionRuntime.accept=false;poison();
      assert(host.saveRemoteDevice(value,true,100)==-ENOTSUP);checkClear();
      assert(!host.commits&&!remoteDevicePending&&!remoteDevicePreparing);
      completionRuntime.accept=true;
    }
    poison();assert(!host.saveRemoteDevice(value,true,110));checkClear();
    assert(host.commits==1&&remoteDevicePending&&!remoteDevicePreparing&&journal.config().role()==role);
    assert(remoteDeviceJournalsMatch()&&!radio.sends);
  }
}
static void regionRoot(){
  reset();TestHost host;mt::Config cfg=service.config;
  auto request=cfg.radioRequest();request.regionCode=1;assert(cfg.setRadio(request));
  mqttAdmin.writable=false;poison();assert(host.saveRemoteLoRa(cfg,true,190)==-EACCES);checkClear();
  assert(!host.commits&&!remoteDevicePending&&!remoteDevicePreparing);
  mqttAdmin.writable=true;poison();assert(!host.saveRemoteLoRa(cfg,true,200));checkClear();
  assert(host.commits==1&&remoteDevicePending&&remoteDeviceJournalsMatch());
  using Mqtt=mt::network::MqttGatewaySettings;
  const auto &mqtt=mqttJournal.config();assert(mqtt.configOkToMqtt()&&mqtt.bytes[Mqtt::RootLength]==6);
  assert(!memcmp(mqtt.bytes+Mqtt::Root,"msh/US",6)&&!radio.sends);
}
int main(){
  sections();roles();regionRoot();
  printf("PASS production role/MQTT projections, synchronous transactions, five journal sections, rejection/reuse, full %zu-byte wipe and %zu-byte tail\n",
         sizeof(RemoteDeviceWork),sizeof(RemoteDeviceWork)-sizeof(RemoteSectionScratch));
}
