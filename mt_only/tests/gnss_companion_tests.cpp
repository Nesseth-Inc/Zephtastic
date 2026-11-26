// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
static meshtastic_AdminMessage setting(bool enabled) {
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
 a.set_config.which_payload_variant=meshtastic_Config_position_tag;
 a.set_config.payload_variant.position.gps_mode=enabled?meshtastic_Config_PositionConfig_GpsMode_ENABLED:meshtastic_Config_PositionConfig_GpsMode_DISABLED;
 return a;
}
static meshtastic_Config_PositionConfig get(Fixture &f,bool ble) {
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;
 a.get_config_request=meshtastic_AdminMessage_ConfigType_POSITION_CONFIG;f.admin(a,ble);
 return (ble?f.ble:f.usb).reply().get_config_response.payload_variant.position;
}
int main() {
 {
  Fixture f;f.usb.fittedGnss=f.ble.fittedGnss=true;
  f.usb.gnssDiagnosticText=f.ble.gnssDiagnosticText="gnss_diag v=1 fitted=1 init=0 ready=0 fault=0 active=0 sentences=0 callbacks=0 promoted=0 sats=0";
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;request.want_config_id=997;
  f.input(request);f.input(request,true);assert(f.usb.gnssDiagnosticCalls==1&&f.ble.gnssDiagnosticCalls==0);
  bool found=false;
  for(const auto &r:f.usb.out)if(r.which_payload_variant==meshtastic_FromRadio_log_record_tag&&strstr(r.log_record.message,"gnss_diag "))found=true;
  assert(found);for(const auto &r:f.ble.out)if(r.which_payload_variant==meshtastic_FromRadio_log_record_tag)assert(!strstr(r.log_record.message,"gnss_diag "));
  assert(get(f,false).gps_mode==meshtastic_Config_PositionConfig_GpsMode_DISABLED);
  assert(get(f,true).gps_mode==meshtastic_Config_PositionConfig_GpsMode_DISABLED&&!f.backend.writes&&!f.radio.sends);
  std::cout<<"PASS fitted capability remains DISABLED before readiness; scalar GNSS diagnostics are USB-only, with no persistence or RF\n";
 }
 for(bool ble:{false,true}) {
  Fixture f;f.usb.fittedGnss=f.ble.fittedGnss=true;auto &h=ble?f.ble:f.usb;
  assert(get(f,ble).gps_mode==meshtastic_Config_PositionConfig_GpsMode_DISABLED);
  const uint8_t publicKey[32]={9};assert(f.service.signature.trustPublicKey(0x87654321,publicKey,100));
  auto a=setting(true);auto &p=a.set_config.payload_variant.position;
  p.gps_update_interval=UINT32_MAX;p.gps_attempt_time=900;p.position_broadcast_secs=600;p.position_flags=703;
  p.position_broadcast_smart_enabled=true;p.broadcast_smart_minimum_distance=UINT32_MAX;p.broadcast_smart_minimum_interval_secs=UINT32_MAX;
  f.admin(a,ble);assert(h.error()==0&&f.service.config.gnssSettings().enabled&&!f.radio.sends);
  auto value=get(f,!ble);assert(value.gps_mode==meshtastic_Config_PositionConfig_GpsMode_ENABLED&&value.gps_update_interval==UINT32_MAX&&value.gps_attempt_time==900&&value.position_broadcast_secs==600&&value.position_flags==703);
  assert(value.position_broadcast_smart_enabled&&value.broadcast_smart_minimum_distance==UINT32_MAX&&value.broadcast_smart_minimum_interval_secs==UINT32_MAX);
  mt::SettingsJournal journal;assert(!journal.load(f.backend)&&journal.config().same(f.service.config));
  // Actual USB/BLE setters/getters and remount preserve raw zero/max fields.
  // No cadence defaulting, interval clamp or location broadcast runs here.
  for(bool smart:{false,true})for(uint32_t distance:{0u,UINT32_MAX})for(uint32_t interval:{0u,UINT32_MAX}) {
   auto request=a;auto &position=request.set_config.payload_variant.position;
   position.position_broadcast_smart_enabled=smart;position.broadcast_smart_minimum_distance=distance;position.broadcast_smart_minimum_interval_secs=interval;
   const auto before=f.service.config;f.admin(request,ble);assert(h.error()==0&&f.service.config.sameExceptPosition(before)&&!f.radio.sends);
   const auto got=get(f,!ble);assert(got.position_broadcast_smart_enabled==smart&&got.broadcast_smart_minimum_distance==distance&&got.broadcast_smart_minimum_interval_secs==interval);
   const auto current=f.service.config.gnssSettings();assert(current.smart==smart&&current.smartMinDistance==distance&&current.smartMinInterval==interval);
   mt::SettingsJournal reboot;assert(!reboot.load(f.backend)&&reboot.config().same(f.service.config));
   const auto count=f.backend.writes;f.admin(request,ble);assert(h.error()==0&&f.backend.writes==count);a=request;
  }
  const auto writes=f.backend.writes;f.admin(a,ble);assert(h.error()==0&&f.backend.writes==writes);
  assert(f.service.signature.keyCount(101)==1);
  for(unsigned variant=0;variant<9;++variant){auto bad=a;auto &b=bad.set_config.payload_variant.position;
   if(variant==0)b.rx_gpio=25;
   if(variant==1)b.tx_gpio=26;
   if(variant==2)b.gps_en_gpio=27;
   if(variant==3)b.position_flags=0x400;
   if(variant==4)b.position_flags=1024; // outside the persisted PositionFlags mask
   if(variant==5)b.gps_enabled=true; // deprecated ambiguous selector
   if(variant==6)b.position_flags=64; // stock SEQ_NO remains unsupported
   if(variant==7)b.position_flags=64|512; // combining an unsupported bit remains invalid
   if(variant==8)b.position_flags=64;
   const auto before=f.service.config;f.admin(bad,ble);assert(h.error()==meshtastic_Routing_Error_BAD_REQUEST&&f.backend.writes==writes&&f.service.config.same(before));
  }
  meshtastic_Position fix{};fix.has_latitude_i=fix.has_longitude_i=true;
  fix.latitude_i=449876543;fix.longitude_i=-931234567;fix.location_source=meshtastic_Position_LocSource_LOC_INTERNAL;
  f.service.measuredPosition(fix);meshtastic_Position selected{};assert(f.service.localPosition(selected)&&selected.latitude_i==fix.latitude_i);
  // RAM-only observations never silently become a fixed/durable coordinate.
  assert(!f.service.config.fixedPosition()&&f.backend.writes==writes);
  auto config=f.service.config;assert(config.setPrimaryPositionPrecision(15));assert(!f.edit.save(config,100));
  uint8_t data[64]{};size_t size=0;assert(mt::PositionQuery::fixedResponse(f.service.config,73,data,sizeof(data),size,0,&fix));
  f.admin(setting(false),ble);assert(h.error()==0&&!f.service.config.positionSharing()&&!f.service.localPosition(selected));
  size=99;assert(!mt::PositionQuery::fixedResponse(f.service.config,74,data,sizeof(data),size,0,&fix)&&!size);
  // Precision/privacy changes use the established full reconfiguration path.
  // The operating node identity survives; volatile signature caches may clear.
  assert(f.service.route.identity()==Node&&!f.radio.sends);
 }
 std::cout<<"PASS actual memory USB/BLE smart config zero/max get/set/journal/no-op; unsupported GPIO/flags still reject without identity/state drift\n";
 std::cout<<"PASS transient fix selection, explicit precision, disable revocation and no coordinate persistence or RF\n";
 Fixture absent;absent.admin(setting(true));assert(absent.usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&!absent.backend.writes);
 std::cout<<"PASS absent receiver refuses enable; no fabricated fitted hardware\n";
}
