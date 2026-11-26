// SPDX-License-Identifier: GPL-3.0-only
#include "Companion.h"
#include "SettingsCommit.h"
#include "network/WifiStationAdmin.h"
#include "network/WifiBootSnapshot.h"
#include "network/MqttGatewayAdmin.h"
#include "known_channel_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <map>
#include <string>
#include <vector>
static constexpr uint32_t Self=0x12345678;
using Bytes=std::vector<uint8_t>;
struct Backend:mt::SettingsBackend{
 std::map<std::string,Bytes> files;unsigned writes=0;std::string failPrefix;bool commitBeforeError=false;
 int read(const char*k,uint8_t*p,size_t n)override{auto i=files.find(k);if(i==files.end())return -ENOENT;if(i->second.size()>n)return -E2BIG;memcpy(p,i->second.data(),i->second.size());return int(i->second.size());}
 int write(const char*k,const uint8_t*p,size_t n)override{++writes;bool fail=!failPrefix.empty()&&std::string(k).rfind(failPrefix,0)==0;if(!fail||commitBeforeError)files[k]={p,p+n};return fail?-EIO:0;}
};
struct Radio:mt::Radio{
 int state=0;bool busy_=false;unsigned sends=0;
 bool ready()override{return true;}int start()override{state=1;return 0;}int restore()override{state=1;return 0;}int stop()override{state=0;return 0;}int health()override{return state;}bool busy()override{return busy_;}uint32_t airtime(size_t)override{return 100;}int send(uint8_t*,size_t)override{++sends;return 0;}bool completion(int&)override{return false;}
};
struct Host:mt::Companion::Host{
 mt::SettingsEdit&core;mt::SettingsCommit&commit;mt::network::WifiStationAdmin&admin;mt::network::MqttGatewayAdmin&mqtt;std::vector<meshtastic_FromRadio>out;bool capable=true,runtime=false,trust=false;
 Host(mt::SettingsEdit&c,mt::SettingsCommit&k,mt::network::WifiStationAdmin&a,mt::network::MqttGatewayAdmin&m):core(c),commit(k),admin(a),mqtt(m){}
 bool ready()const override{return true;}int save(const mt::Config&c)override{return core.save(c,1);}
 bool wifi()const override{return capable;}bool networkConfig(meshtastic_Config_NetworkConfig&o)const override{return admin.get(o);}
 int saveNetworkConfig(const meshtastic_Config_NetworkConfig&v,uint32_t t)override{return admin.save(v,capable,t,t);}
 uint32_t supportedModules()const override{return capable?meshtastic_ExcludedModules_MQTT_CONFIG:0;}
 bool moduleConfig(meshtastic_ModuleConfig&o,unsigned tag)const override{if(tag!=meshtastic_ModuleConfig_mqtt_tag)return false;o={};o.which_payload_variant=tag;return mqtt.get(o.payload_variant.mqtt,capable);}
 int saveModuleConfig(const meshtastic_ModuleConfig&v,uint32_t t)override{if(v.which_payload_variant!=meshtastic_ModuleConfig_mqtt_tag)return -ENOTSUP;return mqtt.save(v.payload_variant.mqtt,capable,runtime,trust,t);}
 bool mqttAvailable()const override{return capable;}bool mqttRuntimeSupported()const override{return runtime;}bool mqttTrustReady()const override{return trust;}
 bool configOkToMqtt()const override{return mqtt.configOkToMqtt(capable);}int saveConfigOkToMqtt(bool v,uint32_t t)override{return mqtt.saveConfigOkToMqtt(v,capable,t);}
 bool channelUplinkEnabled(const mt::Config&c,unsigned i)const override{return mqtt.channelUplinkEnabled(c,i,capable);}
 int saveChannelUplink(const mt::Config&c,unsigned i,bool v,uint32_t t)override{return mqtt.saveChannelUplink(c,i,v,capable,t);}
 int beginEdit(uint32_t t)override{return core.begin(t);}int commitEdit(uint32_t t)override{int r=admin.afterCommit(commit.commit(t),t);return r?r:mqtt.commit(t);}bool editing()const override{return core.open();}
 bool send(const uint8_t*p,size_t n)override{assert(n>=4);meshtastic_FromRadio v{};auto s=pb_istream_from_buffer(p+4,n-4);assert(pb_decode(&s,meshtastic_FromRadio_fields,&v));out.push_back(v);return true;}
 int error()const{assert(!out.empty());meshtastic_Routing r{};const auto&p=out.back().packet;assert(p.decoded.portnum==meshtastic_PortNum_ROUTING_APP);auto s=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);assert(pb_decode(&s,meshtastic_Routing_fields,&r));return r.error_reason;}
};
struct Fixture{
 Radio radio;Backend backend;mt::test::KnownChannelFixtureClock clock;mt::Service service;mt::SettingsJournal journal;mt::SettingsEdit core;mt::NodeJournal nodes;
 mt::network::WifiStationJournal wifiJournal;mt::network::WifiStationEdit wifi;mt::RebootSchedule reboot;mt::network::WifiStationAdmin admin;
 mt::network::MqttGatewayJournal mqttJournal;mt::network::MqttGatewayEdit mqttEdit;mt::network::MqttGatewayAdmin mqttAdmin;
 mt::SettingsCommit commit;Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion a,b;
 explicit Fixture(bool tx=true,bool wifiOn=false):service(radio,true,Self,127,true,false),core(service,journal,backend),wifi(service,wifiJournal,backend),admin(wifi,core,reboot),mqttEdit(service,mqttJournal,backend),mqttAdmin(mqttEdit,core),commit(service,radio,core,nodes,backend,nullptr,nullptr,&wifi),usb(core,commit,admin,mqttAdmin),ble(core,commit,admin,mqttAdmin),a(service,usb,127,workspace),b(service,ble,127,workspace,1){
  clock.bind(service);assert(!journal.load(backend)&&!wifiJournal.load(backend));
  if(wifiOn){mt::network::WifiStationSettings station;assert(station.set(true,"A",1,"B",1)&&!wifiJournal.save(backend,station));}
  assert(wifi.load()&&admin.loaded()&&!mqttJournal.load(backend)&&mqttEdit.load()&&mqttAdmin.loaded()&&!nodes.load(backend,service.nodes,Self));
  auto c=journal.config();c.bytes[4]=1;c.setTxEnabled(tx);assert(!journal.save(backend,c)&&service.apply(c)&&service.start(false,0)&&service.armEndpoint(tx));
 }
 void request(const meshtastic_AdminMessage&m,bool bluetooth=false,uint32_t now=10,bool unknown=false){
  meshtastic_ToRadio v{};v.which_payload_variant=meshtastic_ToRadio_packet_tag;auto&p=v.packet;p.id=71;p.to=Self;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
  auto s=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));assert(pb_encode(&s,meshtastic_AdminMessage_fields,&m));p.decoded.payload.size=s.bytes_written;
  if(unknown){p.decoded.payload.bytes[p.decoded.payload.size++]=0xf8;p.decoded.payload.bytes[p.decoded.payload.size++]=7;p.decoded.payload.bytes[p.decoded.payload.size++]=1;}
  uint8_t data[512];s=pb_ostream_from_buffer(data,sizeof(data));assert(pb_encode(&s,meshtastic_ToRadio_fields,&v));
  if(bluetooth)b.packet(data,s.bytes_written,now,mt::Companion::WorkspaceProof{true});else{a.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});a.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});a.byte(uint8_t(s.bytes_written>>8),now,0,mt::Companion::WorkspaceProof{true});a.byte(uint8_t(s.bytes_written),now,0,mt::Companion::WorkspaceProof{true});for(size_t i=0;i<s.bytes_written;++i)a.byte(data[i],now,0,mt::Companion::WorkspaceProof{true});}
 }
};
static meshtastic_AdminMessage station(bool enabled=true){meshtastic_AdminMessage m{};m.which_payload_variant=meshtastic_AdminMessage_set_config_tag;m.set_config.which_payload_variant=meshtastic_Config_network_tag;auto&n=m.set_config.payload_variant.network;n.wifi_enabled=enabled;strcpy(n.wifi_ssid,"A");strcpy(n.wifi_psk,"B");return m;}
static meshtastic_AdminMessage mqttModule(const meshtastic_ModuleConfig_MQTTConfig&c){meshtastic_AdminMessage m{};m.which_payload_variant=meshtastic_AdminMessage_set_module_config_tag;m.set_module_config.which_payload_variant=meshtastic_ModuleConfig_mqtt_tag;m.set_module_config.payload_variant.mqtt=c;return m;}
static meshtastic_AdminMessage getMqttModule(){meshtastic_AdminMessage m{};m.which_payload_variant=meshtastic_AdminMessage_get_module_config_request_tag;m.get_module_config_request=meshtastic_AdminMessage_ModuleConfigType_MQTT_CONFIG;return m;}
static meshtastic_AdminMessage getChannel(unsigned index){meshtastic_AdminMessage m{};m.which_payload_variant=meshtastic_AdminMessage_get_channel_request_tag;m.get_channel_request=index;return m;}
static meshtastic_AdminMessage getLoRa(){meshtastic_AdminMessage m{};m.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;m.get_config_request=meshtastic_AdminMessage_ConfigType_LORA_CONFIG;return m;}
static meshtastic_AdminMessage setLoRa(const meshtastic_Config_LoRaConfig&c){meshtastic_AdminMessage m{};m.which_payload_variant=meshtastic_AdminMessage_set_config_tag;m.set_config.which_payload_variant=meshtastic_Config_lora_tag;m.set_config.payload_variant.lora=c;return m;}
static meshtastic_AdminMessage decodeLastAdmin(const Host&h){assert(!h.out.empty());const auto&p=h.out.back().packet;assert(p.decoded.portnum==meshtastic_PortNum_ADMIN_APP);meshtastic_AdminMessage m{};auto a=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);assert(pb_decode(&a,meshtastic_AdminMessage_fields,&m)&&a.bytes_left==0);return m;}
static unsigned groups=0;static void pass(const char*s){++groups;printf("PASS %s\n",s);}
static void loraMqttConsentBatch(){
 {
  Fixture f;const auto before=f.service.config;const unsigned writes=f.backend.writes;
  assert(!f.core.begin(20));f.request(getLoRa(),false,21);auto response=decodeLastAdmin(f.usb);
  assert(response.which_payload_variant==meshtastic_AdminMessage_get_config_response_tag&&response.get_config_response.which_payload_variant==meshtastic_Config_lora_tag);
  auto lora=response.get_config_response.payload_variant.lora;assert(lora.tx_enabled&&!lora.config_ok_to_mqtt);
  lora.config_ok_to_mqtt=true;f.request(setLoRa(lora),false,22);
  assert(!f.usb.error()&&f.mqttAdmin.dirty()&&!f.mqttJournal.config().configOkToMqtt());
  assert(f.backend.writes==writes&&f.service.config.same(before)&&f.service.receiving()&&!f.service.endpointArmed()&&!f.radio.sends);
  assert(!f.usb.commitEdit(30)&&!f.core.open()&&!f.mqttAdmin.dirty()&&f.mqttJournal.config().configOkToMqtt());
  assert(f.backend.writes==writes+1&&f.service.config.same(before)&&f.service.receiving()&&f.service.endpointArmed()&&!f.radio.sends);
 }
 {
  Fixture f;const auto before=f.service.config;const unsigned writes=f.backend.writes;assert(!f.core.begin(20));
  f.request(getLoRa(),false,21);auto response=decodeLastAdmin(f.usb);auto lora=response.get_config_response.payload_variant.lora;
  lora.tx_enabled=false;f.request(setLoRa(lora),false,22);
  assert(!f.usb.error()&&f.core.dirty()&&!f.mqttAdmin.dirty()&&f.backend.writes==writes);
  assert(!f.service.config.txEnabled()&&!f.service.endpointArmed()&&f.service.receiving()&&!f.radio.sends);
  assert(!f.usb.commitEdit(30)&&!f.core.open()&&!f.core.dirty()&&!f.service.config.txEnabled()&&
    !f.journal.config().txEnabled()&&f.backend.writes>writes&&f.service.receiving()&&!f.service.endpointArmed()&&!f.radio.sends);
  assert(before.txEnabled());
 }
 {
  Fixture f(false);const auto before=f.service.config;const unsigned writes=f.backend.writes;assert(!f.core.begin(20));
  f.request(getLoRa(),false,21);auto response=decodeLastAdmin(f.usb);auto lora=response.get_config_response.payload_variant.lora;
  lora.tx_enabled=true;f.request(setLoRa(lora),false,22);
  assert(f.usb.error()!=meshtastic_Routing_Error_NONE&&!f.mqttAdmin.dirty()&&f.backend.writes==writes&&f.service.config.same(before));
  lora=response.get_config_response.payload_variant.lora;lora.hop_limit=uint8_t(lora.hop_limit==3?4:3);
  f.request(setLoRa(lora),false,23);
  assert(!f.usb.error()&&f.core.dirty()&&!f.mqttAdmin.dirty()&&f.backend.writes==writes&&
    !f.service.config.txEnabled()&&!f.service.endpointArmed()&&f.service.receiving()&&!f.radio.sends);
  f.request(getLoRa(),false,24);response=decodeLastAdmin(f.usb);lora=response.get_config_response.payload_variant.lora;
  lora.config_ok_to_mqtt=true;f.request(setLoRa(lora),false,25);
  assert(!f.usb.error()&&f.core.dirty()&&f.mqttAdmin.dirty()&&!f.mqttJournal.config().configOkToMqtt()&&f.backend.writes==writes);
  assert(!f.usb.commitEdit(30)&&!f.core.dirty()&&!f.mqttAdmin.dirty()&&f.mqttJournal.config().configOkToMqtt());
  assert(f.journal.config().hopLimit()==lora.hop_limit&&f.backend.writes>writes&&f.service.receiving()&&!f.service.endpointArmed()&&!f.radio.sends);
 }
 pass("TX-off LoRa edits preserve prior batch staging; consent-only changes allow TX-on/off runtime; TX enable and TX-on radio edits stay refused");
}
int main(){
 {
  for(bool ble:{false,true}){Fixture f;auto before=f.backend.files;unsigned writes=f.backend.writes;f.request(station(),ble);assert(!(ble?f.ble:f.usb).error()&&f.wifi.config().enabled()&&f.backend.writes==writes+1&&f.reboot.pending());
   for(const auto&i:before){assert(f.backend.files.at(i.first)==i.second);}
   mt::network::WifiStationJournal boot;assert(!boot.load(f.backend)&&boot.config().enabled());
   meshtastic_AdminMessage get{};get.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;get.get_config_request=meshtastic_AdminMessage_ConfigType_NETWORK_CONFIG;f.request(get,!ble,11);const auto&p=(ble?f.usb:f.ble).out.back().packet;meshtastic_AdminMessage reply{};auto s=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);assert(pb_decode(&s,meshtastic_AdminMessage_fields,&reply)&&reply.which_payload_variant==meshtastic_AdminMessage_get_config_response_tag&&reply.get_config_response.payload_variant.network.wifi_enabled&&!strcmp(reply.get_config_response.payload_variant.network.wifi_psk,"B"));
   assert(f.reboot.takeDueAction(1009)==mt::ScheduledOwnerAction::None &&
          f.reboot.takeDueAction(1010)==mt::ScheduledOwnerAction::Reboot &&
          f.reboot.takeDueAction(1011)==mt::ScheduledOwnerAction::None &&
          !f.radio.sends&&f.service.receiving()&&f.service.endpointArmed());
  }pass("real USB/BLE network setter/getter ACK, exact journal persistence and delayed reboot without RF");
 }
 {
  Fixture f;auto before=f.backend.files;f.usb.capable=false;f.request(station());assert(f.usb.error()!=0&&f.backend.files==before&&!f.reboot.pending());
  f.usb.capable=true;f.request(station(),false,11,true);assert(f.usb.error()!=0&&f.backend.files==before&&!f.reboot.pending());auto unsupported=station();unsupported.set_config.payload_variant.network.eth_enabled=true;f.request(unsupported);assert(f.usb.error()!=0&&f.backend.files==before);
  pass("incapable board, unsupported Ethernet and noncanonical wire refuse without mutation");
 }
 {
  Fixture f;auto writes=f.backend.writes;assert(!f.core.begin(20));f.request(station(),true,21);assert(!f.ble.error()&&f.wifi.dirty()&&f.backend.writes==writes&&!f.reboot.pending());
  auto c=f.service.config;assert(c.setDisplay(45,true)&&!f.core.save(c,22));assert(!f.usb.commitEdit(30)&&!f.core.open()&&!f.wifi.dirty()&&f.reboot.pending()&&f.backend.writes==writes+2);assert(!f.radio.sends&&f.service.receiving()&&f.service.endpointArmed());
  assert(f.reboot.takeDueAction(1030)==mt::ScheduledOwnerAction::Reboot);
  pass("shared core plus WiFi transaction persists both then dispatches one ordinary reboot");
 }
 {
  Fixture f;assert(!f.core.begin(20));f.request(station(),false,21);auto c=f.service.config;assert(c.setDisplay(45,true)&&!f.core.save(c,22));f.backend.failPrefix="mtbase/";assert(f.usb.commitEdit(30)==-EIO&&!f.reboot.pending()&&f.core.open()&&!f.wifi.dirty()&&f.admin.changedFromBoot());
  pass("WiFi saved before a later core error never promotes failed multi-journal commit to reboot");
 }
 {
  Fixture f(true,true);f.request(station(false));
  assert(!f.usb.error()&&!f.wifi.config().enabled()&&f.reboot.takeDueAction(1010)==mt::ScheduledOwnerAction::Reboot);
 }
 {
  Fixture f;assert(!f.core.begin(20));f.request(station(),false,21);
  f.reboot.request(7,40,mt::ScheduledOwnerAction::Shutdown);
  assert(!f.usb.commitEdit(50)&&!f.core.open()&&!f.wifi.dirty());
  assert(f.reboot.takeDueAction(1050)==mt::ScheduledOwnerAction::None&&
         f.reboot.takeDueAction(7040)==mt::ScheduledOwnerAction::Shutdown&&
         f.reboot.action()==mt::ScheduledOwnerAction::Shutdown);
  f.reboot.clear();assert(f.reboot.action()==mt::ScheduledOwnerAction::None);
 }
 {
  Fixture f;auto before=f.wifi.config();f.backend.failPrefix="mtwifi/";f.backend.commitBeforeError=true;f.request(station());assert(f.usb.error()!=0&&!f.wifi.ready()&&!f.reboot.pending()&&!memcmp(before.bytes,f.wifi.config().bytes,before.Size));
  pass("committed-then-error network save poisons readiness and never ACKs success or reboots");
 }
 {
  Fixture f;f.radio.busy_=true;auto before=f.backend.files;f.request(station());assert(f.usb.error()!=0&&f.backend.files==before&&f.wifi.ready()&&!f.reboot.pending());
  pass("busy accepted RF work blocks storage without credential mutation");
 }
 {
  Fixture f;assert(!f.core.begin(20));f.request(station(),false,21);meshtastic_AdminMessage clear{};clear.which_payload_variant=meshtastic_AdminMessage_set_config_tag;clear.set_config.which_payload_variant=meshtastic_Config_network_tag;f.request(clear,true,22);assert(!f.ble.error()&&!f.wifi.dirty()&&!f.admin.changedFromBoot());auto writes=f.backend.writes;assert(!f.usb.commitEdit(30)&&!f.reboot.pending()&&f.backend.writes==writes);
  pass("staged change restored to boot defaults is inert and does not reboot");
 }
 {
  Fixture f;meshtastic_ModuleConfig_MQTTConfig mqtt=meshtastic_ModuleConfig_MQTTConfig_init_zero;
  strcpy(mqtt.address,"broker.native.invalid");strcpy(mqtt.username,"fixture-user");strcpy(mqtt.password,"mqtt-fixture-only");strcpy(mqtt.root,"lab/native");mqtt.encryption_enabled=true;mqtt.tls_enabled=true;
  const unsigned before=f.backend.writes;f.request(mqttModule(mqtt));assert(!f.usb.error()&&f.backend.writes==before+1&&!f.mqttJournal.config().enabled()&&!f.reboot.pending()&&!f.radio.sends&&f.service.receiving()&&f.service.endpointArmed());
  f.request(getMqttModule(),false,11);auto read=decodeLastAdmin(f.usb);assert(read.which_payload_variant==meshtastic_AdminMessage_get_module_config_response_tag&&read.get_module_config_response.which_payload_variant==meshtastic_ModuleConfig_mqtt_tag);
  assert(!read.get_module_config_response.payload_variant.mqtt.enabled&&!strcmp(read.get_module_config_response.payload_variant.mqtt.address,mqtt.address)&&!strcmp(read.get_module_config_response.payload_variant.mqtt.password,mqtt.password));
  mqtt.enabled=true;const unsigned disabledRuntimeWrites=f.backend.writes;f.request(mqttModule(mqtt),false,12);assert(f.usb.error()!=meshtastic_Routing_Error_NONE&&f.backend.writes==disabledRuntimeWrites&&!f.mqttJournal.config().enabled());
  f.usb.runtime=f.usb.trust=true;f.request(mqttModule(mqtt),false,13);assert(!f.usb.error()&&f.mqttJournal.config().enabled()&&!f.reboot.pending()&&!f.radio.sends&&f.service.receiving()&&f.service.endpointArmed());
  f.request(getLoRa(),false,14);read=decodeLastAdmin(f.usb);assert(read.which_payload_variant==meshtastic_AdminMessage_get_config_response_tag&&read.get_config_response.which_payload_variant==meshtastic_Config_lora_tag&&!read.get_config_response.payload_variant.lora.config_ok_to_mqtt);
  auto lora=read.get_config_response.payload_variant.lora;lora.config_ok_to_mqtt=true;meshtastic_AdminMessage set{};set.which_payload_variant=meshtastic_AdminMessage_set_config_tag;set.set_config.which_payload_variant=meshtastic_Config_lora_tag;set.set_config.payload_variant.lora=lora;
  const unsigned beforeConsent=f.backend.writes;f.request(set,false,15);assert(!f.usb.error()&&f.backend.writes==beforeConsent+1&&f.mqttJournal.config().configOkToMqtt()&&!f.radio.sends);
  f.request(getLoRa(),false,16);read=decodeLastAdmin(f.usb);assert(read.get_config_response.payload_variant.lora.config_ok_to_mqtt);
  f.request(getChannel(1),false,17);read=decodeLastAdmin(f.usb);assert(read.which_payload_variant==meshtastic_AdminMessage_get_channel_response_tag&&!read.get_channel_response.settings.uplink_enabled&&!read.get_channel_response.settings.downlink_enabled);
  auto channel=read.get_channel_response;channel.settings.uplink_enabled=true;set={};set.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;set.set_channel=channel;
  f.request(set,false,18);assert(!f.usb.error()&&f.mqttJournal.config().channelUplinkEnabled(f.service.config,0));mt::ChannelRecord primary;assert(f.service.config.channelRecord(0,primary)&&!primary.uplink());
  f.request(getChannel(1),false,19);read=decodeLastAdmin(f.usb);assert(read.get_channel_response.settings.uplink_enabled&&!read.get_channel_response.settings.downlink_enabled);
  channel=read.get_channel_response;channel.settings.downlink_enabled=true;set={};set.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;set.set_channel=channel;const unsigned downlinkWrites=f.backend.writes;
  f.request(set,false,20);
  assert(!f.usb.error()&&f.backend.writes>downlinkWrites&&
    f.service.config.channelDownlinkEnabled(0)&&f.mqttJournal.config().channelUplinkEnabled(f.service.config,0));
  auto getUpdated=getChannel(1);f.request(getUpdated,false,21);read=decodeLastAdmin(f.usb);
  assert(read.get_channel_response.settings.downlink_enabled);
  pass("real USB MQTT module get/set, explicit runtime/trust ENOTSUP, and persisted channel downlink opt-in round-trip");
 }
 loraMqttConsentBatch();
 printf("NETWORK_RUNTIME_GROUPS %u\n",groups);assert(groups==9);
}
