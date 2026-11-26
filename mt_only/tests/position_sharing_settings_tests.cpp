#include "device_config_fixture.h"
static meshtastic_AdminMessage setting(unsigned precision) {
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;
 auto &c=a.set_channel;c.index=0;c.role=meshtastic_Channel_Role_PRIMARY;c.has_settings=true;c.settings.psk.size=1;c.settings.psk.bytes[0]=1;c.settings.has_module_settings=true;c.settings.module_settings.position_precision=precision;return a;
}
int main(){
 for(auto model:{81,127})for(bool ble:{false,true}){
  Fixture f(model,false);auto c=f.service.config;c.bytes[4]=1;c.seal();assert(!f.edit.save(c,0));auto &h=ble?f.ble:f.usb;
  f.admin(setting(32),ble,1);assert(h.error()==meshtastic_Routing_Error_BAD_REQUEST&&!f.service.config.positionSharing());
  c=f.service.config;c.setFixedPosition(123456789,-987654321,false,0);assert(!f.edit.save(c,2));
  f.admin(setting(32),ble,3);assert(h.error()==0&&f.service.config.positionSharing());
  mt::SettingsJournal reloaded;assert(!reloaded.load(f.backend)&&reloaded.config().positionSharing());
  meshtastic_AdminMessage get{};get.which_payload_variant=meshtastic_AdminMessage_get_channel_request_tag;get.get_channel_request=1;
  f.admin(get,ble,4);assert(h.reply().get_channel_response.settings.module_settings.position_precision==15);
  auto writes=f.backend.writes;f.admin(setting(32),ble,5);assert(h.error()==0&&f.backend.writes==writes);
  for(unsigned bad:{1,16,31,33,255}){f.admin(setting(bad),ble,6);assert(h.error()==meshtastic_Routing_Error_BAD_REQUEST&&f.service.config.positionSharing());}

  f.admin(setting(0),ble,8);assert(h.error()==0&&!f.service.config.positionSharing()&&f.service.config.fixedPosition());
  assert(!reloaded.load(f.backend)&&!reloaded.config().positionSharing());
  f.admin(get,ble,9);assert(!h.reply().get_channel_response.settings.module_settings.position_precision);
  f.admin(setting(32),ble,10);assert(h.error()==0&&f.service.config.positionSharing());
  f.backend.fail=true;f.admin(setting(0),ble,11);assert(h.error()!=0&&f.service.config.positionSharing());
 }
 std::cout<<"PASS both models and memory USB/BLE: explicit consent, rejection, journal restart, no-op, write failure and revocation\n";
}

