// SPDX-License-Identifier: GPL-3.0-only
#include "notifications/NotificationController.h"
#include "notifications/NotificationEdit.h"
#include "Companion.h"
#include "SettingsCommit.h"
#include "DeviceSettingsCommit.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
static constexpr uint32_t Self=0x12345678,Peer=0x87654321;
using Bytes=std::vector<uint8_t>;
struct Backend:mt::SettingsBackend {
 std::map<std::string,Bytes> files;unsigned writes=0;bool fail=false,uncertain=false;
 int read(const char*k,uint8_t*p,size_t n)override{auto i=files.find(k);if(i==files.end())return -ENOENT;if(i->second.size()>n)return -E2BIG;memcpy(p,i->second.data(),i->second.size());return int(i->second.size());}
 int write(const char*k,const uint8_t*p,size_t n)override{++writes;if(!fail||uncertain)files[k]={p,p+n};return fail?-EIO:0;}
};
struct Radio:mt::Radio {
 int state=0;unsigned sends=0;bool busy_=false;
 bool ready()override{return true;}int start()override{state=1;return 0;}int restore()override{state=1;return 0;}
 int stop()override{state=0;return 0;}int health()override{return state;}bool busy()override{return busy_;}
 uint32_t airtime(size_t)override{return 100;}int send(uint8_t*,size_t)override{++sends;return 0;}bool completion(int&)override{return false;}
};
static meshtastic_ModuleConfig config(bool enabled=true){
 meshtastic_ModuleConfig v{};v.which_payload_variant=meshtastic_ModuleConfig_external_notification_tag;
 v.payload_variant.external_notification.enabled=enabled;v.payload_variant.external_notification.active=true;
 v.payload_variant.external_notification.alert_message=true;v.payload_variant.external_notification.output_ms=100;
 return v;
}
struct Host:mt::Companion::Host {
 mt::Service &service;
 mt::SettingsEdit &core;mt::NotificationEdit &edit;mt::SettingsCommit &commit;
 std::vector<meshtastic_FromRadio> out;bool capability=true;
 Host(mt::Service&s,mt::SettingsEdit&c,mt::NotificationEdit&e,mt::SettingsCommit&k):service(s),core(c),edit(e),commit(k){}
 bool ready()const override{return true;}int save(const mt::Config&c)override{return core.save(c,1);}
 bool heartbeatDisabled()const override{return !edit.ready()||edit.config().heartbeatDisabled();}
 int saveDevice(const mt::Config&c,bool disabled,uint32_t now)override{return mt::DeviceSettingsCommit::save(service,core,edit,commit,c,disabled,capability,now);}
 uint32_t supportedModules()const override{return capability&&edit.ready()?meshtastic_ExcludedModules_EXTNOTIF_CONFIG:0;}
 bool moduleConfig(meshtastic_ModuleConfig&o,unsigned tag)const override{return tag==meshtastic_ModuleConfig_external_notification_tag&&edit.ready()&&mt::NotificationConfig::get(edit.config(),o);}
 int saveModuleConfig(const meshtastic_ModuleConfig&v,uint32_t t)override{int r=edit.save(v,t,core.open(),capability,28);if(!r)core.activity(t);return r;}
 int beginEdit(uint32_t t)override{return core.begin(t);}int commitEdit(uint32_t t)override{return commit.commit(t);}bool editing()const override{return core.open();}
 bool send(const uint8_t*p,size_t n)override{assert(n>=4);meshtastic_FromRadio v{};auto s=pb_istream_from_buffer(p+4,n-4);assert(pb_decode(&s,meshtastic_FromRadio_fields,&v));out.push_back(v);return true;}
 int error()const{assert(!out.empty());meshtastic_Routing r{};const auto&p=out.back().packet;assert(p.decoded.portnum==meshtastic_PortNum_ROUTING_APP);auto s=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);assert(pb_decode(&s,meshtastic_Routing_fields,&r));return r.error_reason;}
};
struct Fixture {
 Radio radio;Backend backend;mt::test::KnownChannelFixtureClock clock;
 mt::Service service;mt::SettingsJournal coreJournal;mt::SettingsEdit core;
 mt::NodeJournal nodes;mt::NotificationJournal journal;mt::NotificationEdit edit;mt::SettingsCommit commit;
 Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion a,b;
 Fixture():service(radio,true,Self,127,true,false),core(service,coreJournal,backend),edit(service,journal,backend),commit(service,radio,core,nodes,backend,nullptr,&edit),usb(service,core,edit,commit),ble(service,core,edit,commit),a(service,usb,127,workspace),b(service,ble,127,workspace,1){
  clock.bind(service);assert(!coreJournal.load(backend)&&!journal.load(backend)&&edit.load()&&!nodes.load(backend,service.nodes,Self));
  auto c=coreJournal.config();c.bytes[4]=1;c.setTxEnabled(true);assert(!coreJournal.save(backend,c)&&service.apply(c));assert(service.start(false,0)&&service.armEndpoint(true));
 }
 void admin(const meshtastic_AdminMessage &value,bool bluetooth=false,uint32_t now=1,bool noncanonical=false){
  meshtastic_ToRadio v{};v.which_payload_variant=meshtastic_ToRadio_packet_tag;auto&p=v.packet;
  p.id=71;p.to=Self;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
  auto s=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));assert(pb_encode(&s,meshtastic_AdminMessage_fields,&value));p.decoded.payload.size=s.bytes_written;
  if(noncanonical){p.decoded.payload.bytes[p.decoded.payload.size++]=0xf8;p.decoded.payload.bytes[p.decoded.payload.size++]=7;p.decoded.payload.bytes[p.decoded.payload.size++]=1;}
  uint8_t bytes[512];s=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&s,meshtastic_ToRadio_fields,&v));
  if(bluetooth)b.packet(bytes,s.bytes_written,now,mt::Companion::WorkspaceProof{true});
  else {a.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});a.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});a.byte(uint8_t(s.bytes_written>>8),now,0,mt::Companion::WorkspaceProof{true});a.byte(uint8_t(s.bytes_written),now,0,mt::Companion::WorkspaceProof{true});for(size_t i=0;i<s.bytes_written;++i)a.byte(bytes[i],now,0,mt::Companion::WorkspaceProof{true});}
 }
 void set(const meshtastic_ModuleConfig&v,bool bluetooth=false,uint32_t now=1,bool noncanonical=false){meshtastic_AdminMessage m{};m.which_payload_variant=meshtastic_AdminMessage_set_module_config_tag;m.set_module_config=v;admin(m,bluetooth,now,noncanonical);}
 meshtastic_AdminMessage device(bool disabled,uint32_t interval=3600){
  meshtastic_AdminMessage m{};m.which_payload_variant=meshtastic_AdminMessage_set_config_tag;m.set_config.which_payload_variant=meshtastic_Config_device_tag;
  auto&d=m.set_config.payload_variant.device;d.serial_enabled=true;d.led_heartbeat_disabled=disabled;d.role=static_cast<meshtastic_Config_DeviceConfig_Role>(service.config.role());
  d.rebroadcast_mode=static_cast<meshtastic_Config_DeviceConfig_RebroadcastMode>(service.config.rebroadcastMode());d.node_info_broadcast_secs=interval;return m;
 }
};
static unsigned groups=0;
static void pass(const char*s){++groups;printf("PASS %s\n",s);}
int main(){
 {
  for(bool enabled:{false,true})for(bool message:{false,true})for(bool bellSetting:{false,true})for(bool muted:{false,true})for(bool bell:{false,true}){
   mt::NotificationSettings s;assert(s.set(enabled,message,bellSetting,true,0,100,0));mt::NotificationController c;c.configure(s,true);uint8_t text=bell?7:'x';
   const bool expected=enabled&&((bellSetting&&bell)||(message&&!muted));assert(c.receive(Peer,Self,1,1,&text,1,muted,false,0)==expected);assert(c.output().on==expected);
  }
  pass("32 literal stock generic-message/bell/mute truth cases including bell bypass");
 }
 {
  mt::NotificationSettings s;assert(s.set(true,true,true,true,28,100,0));mt::NotificationController c;c.configure(s,true);uint8_t p=7;
  assert(!c.receive(Self,Self,1,1,&p,1,false,false,1));assert(!c.receive(Peer,Self,1,67,&p,1,false,false,1));
  assert(!c.receive(Peer,Self,1,1,nullptr,1,false,false,1));assert(!c.receive(Peer,Self,1,1,&p,234,false,false,1));
  assert(c.receive(Peer,Self,1,1,&p,1,false,false,1));assert(!c.receive(Peer,Self,1,1,&p,1,false,false,2));c.poll(100);assert(c.output().on);c.poll(101);assert(!c.output().on&&!c.armed());
  assert(!c.receive(Peer,Self,2,66,&p,1,false,false,102));assert(c.receive(Peer,Self,2,66,&p,1,false,true,102));
  c.configure(s,false);assert(!c.output().enabled&&!c.output().on);c.configure(s,true);assert(c.receive(Peer,Self,3,meshtastic_PortNum_ALERT_APP,&p,1,false,false,200));
  pass("bounded payload, source/port/capability gates and USB/BLE fanout dedup");
 }
 {
  mt::NotificationSettings s;assert(s.set(true,true,false,true,0,100,1));mt::NotificationController c;c.configure(s,true);uint8_t p='x';const uint32_t start=UINT32_MAX-50;
  assert(c.receive(Peer,Self,1,1,&p,1,false,false,start));c.poll(start+99);assert(c.output().on);c.poll(start+100);assert(!c.output().on);c.poll(start+950);assert(c.output().on);c.poll(start+1000);assert(!c.armed());
  assert(s.set(true,true,false,true,0,0,0));c.configure(s,true);assert(c.receive(Peer,Self,2,1,&p,1,false,false,100));assert(c.output().on);c.poll(100);assert(!c.armed());
  assert(s.set(true,true,false,true,0,0,3));c.configure(s,true);assert(c.receive(Peer,Self,3,1,&p,1,false,false,0));c.poll(999);assert(c.output().on);c.poll(1000);assert(!c.output().on);c.poll(3000);assert(!c.armed());
  pass("wrap, inclusive cutoff, no catch-up, raw zero duration and 1000ms toggle default");
 }
 {
  auto v=config();mt::NotificationSettings s;assert(mt::NotificationConfig::set(s,v,true,28));const auto before=s;
  v.payload_variant.external_notification.output=27;assert(!mt::NotificationConfig::set(s,v,true,28)&&s.same(before));v=config();assert(!mt::NotificationConfig::set(s,v,false,28));
  auto bad=v;bad.payload_variant.external_notification.use_pwm=true;assert(!mt::NotificationConfig::set(s,bad,true,28));bad=v;bad.payload_variant.external_notification.output_vibra=1;assert(!mt::NotificationConfig::set(s,bad,true,28));
  bad=v;bad.payload_variant.external_notification.output_buzzer=1;assert(!mt::NotificationConfig::set(s,bad,true,28));bad=v;bad.payload_variant.external_notification.alert_message_vibra=true;assert(!mt::NotificationConfig::set(s,bad,true,28));
  bad=v;bad.payload_variant.external_notification.alert_bell_vibra=true;assert(!mt::NotificationConfig::set(s,bad,true,28));bad=v;bad.payload_variant.external_notification.alert_message_buzzer=true;assert(!mt::NotificationConfig::set(s,bad,true,28));
  bad=v;bad.payload_variant.external_notification.alert_bell_buzzer=true;assert(!mt::NotificationConfig::set(s,bad,true,28));bad=v;bad.payload_variant.external_notification.use_i2s_as_buzzer=true;assert(!mt::NotificationConfig::set(s,bad,true,28));
  v.payload_variant.external_notification.output_ms=UINT32_MAX;assert(!mt::NotificationConfig::set(s,v,true,28)&&s.same(before));
  meshtastic_ModuleConfig out{};assert(mt::NotificationConfig::get(s,out)&&out.which_payload_variant==3&&out.payload_variant.external_notification.output_ms==100);
  pass("canonical LED subset round trip and rejection preserves all bytes");
 }
 {
  for(bool ble:{false,true}){Fixture f;const auto core=f.service.config;auto before=f.backend.files;const auto writes=f.backend.writes;f.set(config(),ble);assert((ble?f.ble:f.usb).error()==0&&f.edit.config().enabled()&&f.backend.writes==writes+1);
   assert(f.service.config.same(core)&&f.service.receiving()&&f.service.endpointArmed()&&!f.radio.sends);for(const auto&i:before)assert(f.backend.files.at(i.first)==i.second);
   mt::NotificationJournal boot;assert(!boot.load(f.backend)&&boot.config().enabled());f.set(config(),!ble,2);assert((ble?f.usb:f.ble).error()==0&&f.backend.writes==writes+1);
   const auto retained=f.backend.files;f.set(config(),ble,3,true);assert((ble?f.ble:f.usb).error()!=0&&f.backend.files==retained);
   meshtastic_AdminMessage get{};get.which_payload_variant=meshtastic_AdminMessage_get_module_config_request_tag;get.get_module_config_request=meshtastic_AdminMessage_ModuleConfigType_EXTNOTIF_CONFIG;f.admin(get,!ble,4);
   const auto&packet=(ble?f.usb:f.ble).out.back().packet;assert(packet.decoded.portnum==meshtastic_PortNum_ADMIN_APP);meshtastic_AdminMessage reply{};auto stream=pb_istream_from_buffer(packet.decoded.payload.bytes,packet.decoded.payload.size);assert(pb_decode(&stream,meshtastic_AdminMessage_fields,&reply));assert(reply.get_module_config_response.payload_variant.external_notification.enabled);
  }
  pass("real USB/BLE canonical setter/getter/ACK, reboot reload, no-op and unknown-wire refusal");
 }
 {
  Fixture f;const auto writes=f.backend.writes;assert(!f.core.begin(10));f.set(config(),false,20);assert(!f.usb.error()&&f.edit.dirty()&&f.backend.writes==writes);
  assert(!f.commit.commit(30)&&!f.edit.dirty()&&!f.core.open()&&f.backend.writes==writes+1&&f.service.receiving()&&f.service.endpointArmed());
  mt::NotificationJournal boot;assert(!boot.load(f.backend)&&boot.config().enabled());assert(!f.radio.sends);
  pass("notification-only edit participates in core commit without rewriting legacy records");
 }
 {
  for(bool uncertain:{false,true}){Fixture f;const auto before=f.edit.config();f.backend.fail=true;f.backend.uncertain=uncertain;f.set(config());assert(f.usb.error()!=0&&!f.edit.ready()&&f.edit.config().same(before)&&!f.service.receiving()&&!f.radio.sends);}
  Fixture f;f.radio.busy_=true;const auto writes=f.backend.writes;f.set(config());assert(f.usb.error()!=0&&f.edit.ready()&&f.backend.writes==writes&&!f.edit.config().enabled());
  pass("uncertain write and busy radio never report durable success or reset old settings");
 }
 {
  for(bool ble:{false,true}){
   Fixture f;auto c=f.service.config;assert(c.setNodeInfoSeconds(3600)&&!f.core.save(c,0));const auto core=f.service.config;
   const auto writes=f.backend.writes;f.admin(f.device(false),ble,10);assert(!(ble?f.ble:f.usb).error()&&!f.edit.config().heartbeatDisabled()&&f.service.config.same(core)&&f.backend.writes==writes+1);
   mt::NotificationJournal boot;assert(!boot.load(f.backend)&&!boot.config().heartbeatDisabled());
   meshtastic_AdminMessage get{};get.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;get.get_config_request=meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG;f.admin(get,!ble,11);
   const auto&p=(ble?f.usb:f.ble).out.back().packet;meshtastic_AdminMessage reply{};auto s=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);assert(pb_decode(&s,meshtastic_AdminMessage_fields,&reply));assert(!reply.get_config_response.payload_variant.device.led_heartbeat_disabled);
   f.set(config(),!ble,12);assert(!(ble?f.usb:f.ble).error()&&!f.edit.config().heartbeatDisabled());
   auto bad=f.device(true);bad.set_config.payload_variant.device.button_gpio=1;const auto before=f.backend.files;f.admin(bad,ble,13);assert((ble?f.ble:f.usb).error()!=0&&f.backend.files==before&&!f.edit.config().heartbeatDisabled());
   f.admin(f.device(true),ble,14,true);assert((ble?f.ble:f.usb).error()!=0&&!f.edit.config().heartbeatDisabled());
   f.admin(f.device(true),!ble,15);assert(!(ble?f.usb:f.ble).error()&&f.edit.config().heartbeatDisabled()&&f.service.receiving()&&f.service.endpointArmed()&&!f.radio.sends);
  }
  Fixture headless;headless.usb.capability=false;const auto before=headless.backend.files;headless.admin(headless.device(false));assert(headless.usb.error()!=0&&headless.backend.files==before&&headless.edit.config().heartbeatDisabled());
  pass("real USB/BLE DeviceConfig heartbeat GET/SET persistence preserves notification settings and rejects unsupported/noncanonical changes");
 }
 {
  mt::NotificationSettings s;assert(s.set(true,true,false,true,28,100,10));
  s.bytes[3]='1';s.bytes[4]&=0x0f;s.seal();assert(s.valid());
  mt::NotificationController c;c.configure(s,true);const uint8_t text='x';
  assert(c.receive(Peer,Self,101,1,&text,1,false,false,100)&&c.armed()&&c.output().on);
  assert(s.setHeartbeatDisabled(false)&&s.bytes[3]=='2');c.configure(s,true);
  assert(c.armed()&&c.output().on);c.poll(199);assert(c.output().on);
  c.poll(200);assert(c.armed()&&!c.output().on);
  assert(s.setHeartbeatDisabled(true));c.configure(s,true);assert(c.armed()&&!c.output().on);
  assert(!c.receive(Peer,Self,101,1,&text,1,false,false,201));
  c.poll(10099);assert(c.armed());c.poll(10100);assert(!c.armed());
  assert(c.receive(Peer,Self,102,1,&text,1,false,false,20000));
  assert(s.set(true,true,false,true,28,200,10));c.configure(s,true);assert(!c.armed());
  assert(c.receive(Peer,Self,103,1,&text,1,false,false,30000));s.bytes[14]^=1;c.configure(s,true);
  assert(!c.armed()&&!c.output().enabled);
  pass("heartbeat toggles and legacy promotion preserve notification pulse/nag/dedup; policy changes and invalid CRC still stop");
 }
 printf("NOTIFICATION_RUNTIME_GROUPS %u\n",groups);
}
