// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
#include "DeviceSettingsCommit.h"
#include "modules/OperatingRole.h"
#ifdef TEST_DEVICE_ADMIN_282
#include "modules/RouterLateRoleDefaults.h"
#include "modules/TakRoleDefaults.h"
#include "stock_device_admin.h"
#endif
#include "modules/CompletionConfig.h"
struct RoleHost:Host{
 mt::Service &service;
 mt::SettingsEdit &settingsEdit;
 mt::NotificationJournal leds;
 mt::NotificationEdit notificationEdit;
 mt::ModuleJournal moduleJournal;
 mt::ModuleEdit moduleEdit;
 mt::modules::CompletionJournal completionJournal;
 mt::modules::CompletionEdit completionEdit;
 mt::NodeJournal nodes;
 mt::SettingsCommit settingsCommit;
 RoleHost(Fixture &f):Host(f.edit,f.journal),service(f.service),settingsEdit(f.edit),
   notificationEdit(service,leds,f.backend),moduleEdit(service,moduleJournal,f.backend),
   completionEdit(service,completionJournal,f.backend),
   settingsCommit(service,f.radio,f.edit,nodes,f.backend,&moduleEdit,&notificationEdit){
  assert(!leds.load(f.backend)&&notificationEdit.load());
  assert(!moduleJournal.load(f.backend)&&moduleEdit.load());
  assert(!completionJournal.load(f.backend)&&completionEdit.load());
  assert(!nodes.load(f.backend,service.nodes,Node));settingsCommit.completion(completionEdit);
 }
 bool moduleConfig(meshtastic_ModuleConfig &output,unsigned tag)const override{
  return mt::modules::completionConfig(completionEdit.config(),moduleEdit.config(),notificationEdit.config(),{},output,tag);
 }
 int saveDevice(const mt::Config &c,bool disabled,uint32_t now) override {
#include "application-role-save.inc"
  return mt::DeviceSettingsCommit::save(service,settingsEdit,notificationEdit,settingsCommit,c,disabled,true,now);
 }
};
static void send(mt::Companion &phone,const meshtastic_AdminMessage &admin,uint32_t now){
 meshtastic_ToRadio input{};input.which_payload_variant=meshtastic_ToRadio_packet_tag;
 auto &packet=input.packet;packet.to=Node;packet.id=71;packet.want_ack=true;packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
 auto &data=packet.decoded;data.portnum=meshtastic_PortNum_ADMIN_APP;data.want_response=true;
 auto payload=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
 assert(pb_encode(&payload,meshtastic_AdminMessage_fields,&admin));data.payload.size=payload.bytes_written;
 uint8_t wire[512];auto stream=pb_ostream_from_buffer(wire,sizeof(wire));assert(pb_encode(&stream,meshtastic_ToRadio_fields,&input));
 phone.packet(wire,stream.bytes_written,now,mt::Companion::WorkspaceProof{true});
}
#ifdef TEST_DEVICE_ADMIN_282
static void deviceReplacement(meshtastic_Config_DeviceConfig_Role role,
                              meshtastic_Config_DeviceConfig_RebroadcastMode mode) {
  auto f = std::make_unique<Fixture>();
  f->provision();
  RoleHost host(*f);
  mt::Companion phone(f->service,host,127,f->workspace,1);
  stock_device::Admin stock{};
  stock.config.device.role = static_cast<meshtastic_Config_DeviceConfig_Role>(f->service.config.role());

  // The first request changes role and two other device fields. The next two
  // are same-role replacements, including an interval below the stock floor.
  uint32_t now = 2;
  for (const uint32_t interval : {7201u, 8234u, 30u}) {
    auto admin = device(interval);
    auto &d = admin.set_config.payload_variant.device;
    d.role = role;
    d.rebroadcast_mode = mode;
    strcpy(d.tzdef,interval==7201 ? "EST5" : "UTC0");
    stock.apply(admin.set_config);
    send(phone,admin,now++);
    assert(host.error()==meshtastic_Routing_Error_NONE);

    // Read what a stock phone sees and then reopen both flash journals.
    assert(getter(*f)==stock.config.device.node_info_broadcast_secs);
    const auto response=f->usb.reply().get_config_response.payload_variant.device;
    assert(response.role==stock.config.device.role && response.rebroadcast_mode==stock.config.device.rebroadcast_mode);
    assert(!strcmp(response.tzdef,stock.config.device.tzdef));
    mt::SettingsJournal settings;
    mt::ModuleJournal modules;
    assert(!settings.load(f->backend) && !modules.load(f->backend));
    const auto &stored=settings.config();
    assert(stored.same(f->service.config) && stored.timezoneEquals(stock.config.device.tzdef));
    assert(stored.node().interval()==stock.config.device.node_info_broadcast_secs);
    assert(stored.ownerUnmessagable()==stock.owner.is_unmessagable);
    meshtastic_ModuleConfig telemetry{};
    assert(modules.config().get(telemetry,meshtastic_ModuleConfig_telemetry_tag));
    assert(telemetry.payload_variant.telemetry.device_update_interval==stock.moduleConfig.telemetry.device_update_interval);
  }
}
#endif
void adminCases(){
#ifdef TEST_DEVICE_ADMIN_282
 for (auto role : {meshtastic_Config_DeviceConfig_Role_ROUTER_LATE,meshtastic_Config_DeviceConfig_Role_TAK_TRACKER})
  for (auto mode : {meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY,meshtastic_Config_DeviceConfig_RebroadcastMode_NONE})
   deviceReplacement(role,mode);
 puts("PASS 2.8.2 stock DeviceConfig and NodeDB transitions, same-role writes, phone readback and journal reload");
#endif
 for(unsigned role:{5u,6u,7u,8u,9u}){
  auto f=std::make_unique<Fixture>();f->provision();RoleHost h(*f);
  mt::Companion phone(f->service,h,127,f->workspace,1);
  auto a=device();a.set_config.payload_variant.device.role=static_cast<meshtastic_Config_DeviceConfig_Role>(role);
  send(phone,a,2);assert(h.error()==0&&f->service.config.role()==role);
  mt::SettingsJournal core;mt::ModuleJournal modules;mt::modules::CompletionJournal extra;
  assert(!core.load(f->backend)&&!modules.load(f->backend)&&!extra.load(f->backend));
  assert(core.config().same(f->service.config)&&modules.config().same(h.moduleEdit.config()));
  if(role==8){
   meshtastic_ModuleConfig stored{};assert(extra.config().get(stored,meshtastic_ModuleConfig_telemetry_tag));
   assert(stored.payload_variant.telemetry.health_update_interval==INT32_MAX&&stored.payload_variant.telemetry.air_quality_interval==INT32_MAX);
   assert(core.config().node().interval()==INT32_MAX&&core.config().rebroadcastMode()==2);
   a.set_config.payload_variant.device.node_info_broadcast_secs=6400;a.set_config.payload_variant.device.rebroadcast_mode=meshtastic_Config_DeviceConfig_RebroadcastMode_ALL;
   send(phone,a,3);assert(h.error()==0&&f->service.config.node().interval()==6400&&f->service.config.rebroadcastMode()==0);
   // Periodic NodeInfo stays silent even with custom intervals, device metrics
   // and mesh beacons each have independent hidden-role admission.
   auto &s=f->service;s.stop();auto c=s.config;c.setTxEnabled(true);assert(s.apply(c)&&s.start(false,0)&&s.armEndpoint(true));
   assert(f->knownChannelClock.set(1));s.poll(1,1234);assert(f->knownChannelClock.set(70000));s.poll(70000,1235);
   assert(!s.endpointControls()&&!f->radio.sends);
  }
 }
 puts("PASS real encoded phone Admin role changes, four journals, hidden role restore and same-role customization");
}
