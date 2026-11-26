// SPDX-License-Identifier: GPL-3.0-only
// Real companion + shared editor + journal; pinned stock transaction semantics
// are documented in SETTINGS-EDIT-M8.md. No hardware, credentials or RF.
#include "Companion.h"
#include "known_channel_fixture.h"
#include "SettingsEdit.h"
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>
constexpr uint32_t Node=0x12345678;
struct Radio:mt::Radio {
 unsigned starts=0,sends=0;int state=0;bool ready()override{return true;}
 int start()override{++starts;state=1;return 0;}int restore()override{state=1;return 0;}
 int stop()override{state=0;return 0;}int health()override{return state;}
 bool busy()override{return false;}uint32_t airtime(size_t)override{return 100;}
 int send(uint8_t *,size_t)override{++sends;return 0;}bool completion(int &)override{return false;}
};
struct Backend:mt::SettingsBackend {
 std::map<std::string,std::vector<uint8_t>> entries;unsigned writes=0;bool fail=false,uncertain=false;
 int read(const char *key,uint8_t *out,size_t capacity)override {
  auto it=entries.find(key);if(it==entries.end())return -ENOENT;
  if(it->second.size()>capacity)return -E2BIG;
  memcpy(out,it->second.data(),it->second.size());return it->second.size();
 }
 int write(const char *key,const uint8_t *p,size_t n)override {
  ++writes;if(fail&&!uncertain)return -EIO;entries[key]={p,p+n};return fail?-EIO:0;
 }
};
struct Host:mt::Companion::Host {
 mt::SettingsEdit &edit;mt::SettingsJournal &journal;uint32_t now=1;
 bool scheduled=false,ble=true,bleConnected=true;Serial serial{true,115200,true};
 std::vector<meshtastic_FromRadio> out;
 Host(mt::SettingsEdit &e,mt::SettingsJournal &j):edit(e),journal(j){}
 bool ready()const override{return journal.ready();}
 bool bluetooth()const override{return ble;}
 Security bluetoothSecurity()const override{Security s;s.connected=bleConnected;return s;}
 Serial serialStatus()const override{return serial;}
 bool send(const uint8_t *p,size_t n)override {
  assert(n>=4&&size_t(p[2]*256+p[3])==n-4);meshtastic_FromRadio r{};
  auto stream=pb_istream_from_buffer(p+4,n-4);assert(pb_decode(&stream,meshtastic_FromRadio_fields,&r));out.push_back(r);return true;
 }
 int save(const mt::Config &c)override{return edit.save(c,now);}
 int beginEdit(uint32_t t)override{return scheduled?-EACCES:edit.begin(t);}
 int commitEdit(uint32_t t)override{return edit.commit(t);}
 bool editing()const override{return edit.open();}
 bool scheduleReboot(int32_t delay)override{scheduled=delay>=0;return true;}
 int error()const {
  assert(!out.empty());const auto &p=out.back().packet;assert(p.decoded.portnum==meshtastic_PortNum_ROUTING_APP);
  meshtastic_Routing r{};auto stream=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);
  assert(pb_decode(&stream,meshtastic_Routing_fields,&r));return r.error_reason;
 }
 meshtastic_AdminMessage reply()const {
  assert(!out.empty());const auto &p=out.back().packet;assert(p.decoded.portnum==meshtastic_PortNum_ADMIN_APP);
  meshtastic_AdminMessage r{};auto stream=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);
  assert(pb_decode(&stream,meshtastic_AdminMessage_fields,&r));return r;
 }
};
struct Fixture {
  mt::test::KnownChannelFixtureClock knownChannelClock;
 Radio radio;Backend backend;mt::SettingsJournal journal;mt::Service service;
 mt::SettingsEdit edit;Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion c,b;
 explicit Fixture(bool discovery=false):service(radio,true,Node,127,true,discovery),edit(service,journal,backend),usb(edit,journal),ble(edit,journal),c(service,usb,127,workspace,0),b(service,ble,127,workspace,1) {
    knownChannelClock.bind(service);
  assert(journal.load(backend)==0);assert(service.apply(journal.config()));
 }
 void input(const meshtastic_ToRadio &r,bool bluetooth=false,uint32_t now=1) {
  auto &host=bluetooth?ble:usb;host.now=now;uint8_t bytes[512];auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
  assert(pb_encode(&stream,meshtastic_ToRadio_fields,&r));
  if(bluetooth)b.packet(bytes,stream.bytes_written,now,mt::Companion::WorkspaceProof{true});
  else {c.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});c.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});c.byte(stream.bytes_written>>8,now,0,mt::Companion::WorkspaceProof{true});c.byte(stream.bytes_written,now,0,mt::Companion::WorkspaceProof{true});for(size_t i=0;i<stream.bytes_written;++i)c.byte(bytes[i],now,0,mt::Companion::WorkspaceProof{true});}
 }
 void admin(const meshtastic_AdminMessage &a,bool bluetooth=false,uint32_t now=1) {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
  p.to=Node;p.id=71;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
  auto stream=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
  assert(pb_encode(&stream,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=stream.bytes_written;input(r,bluetooth,now);
 }
 void op(pb_size_t tag,bool bluetooth=false,uint32_t now=1){meshtastic_AdminMessage a{};a.which_payload_variant=tag;admin(a,bluetooth,now);}
 void module(pb_size_t tag,bool bluetooth=false,uint32_t now=1){
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_module_config_tag;
  a.set_module_config.which_payload_variant=tag;admin(a,bluetooth,now);
 }
 void begin(uint32_t now=1){op(meshtastic_AdminMessage_begin_edit_settings_tag,false,now);assert(usb.error()==0);}
 void commit(){op(meshtastic_AdminMessage_commit_edit_settings_tag);}
 void owner(const char *name,bool bluetooth=false,uint32_t now=2) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;strcpy(a.set_owner.long_name,name);admin(a,bluetooth,now);
 }
 void provision() {auto cfg=service.config;cfg.bytes[4]=1;cfg.seal();assert(edit.save(cfg,0)==0);}
};
static meshtastic_AdminMessage lora(bool tx=false) {
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;a.set_config.which_payload_variant=meshtastic_Config_lora_tag;
 auto &l=a.set_config.payload_variant.lora;l.region=meshtastic_Config_LoRaConfig_RegionCode_US;l.use_preset=true;l.channel_num=20;l.sx126x_rx_boosted_gain=true;l.hop_limit=3;l.tx_enabled=tx;
 return a;
}
int main(){
 {
  auto f=std::make_unique<Fixture>();f->begin();f->owner("Batch One");assert(f->usb.error()==0&&f->edit.dirty()&&f->backend.writes==0);
  const auto pending=f->service.config;f->module(meshtastic_ModuleConfig_mqtt_tag,true,50);
  assert(f->ble.error()==0&&f->edit.open()&&f->edit.dirty()&&f->backend.writes==0&&f->service.config.same(pending));
  f->op(meshtastic_AdminMessage_get_owner_request_tag,true);auto r=f->ble.reply();assert(!strcmp(r.get_owner_response.long_name,"Batch One"));
  f->owner("Batch Two",true);f->op(meshtastic_AdminMessage_begin_edit_settings_tag,true,100);assert(f->ble.error()==0&&f->edit.dirty());
  f->commit();assert(f->usb.error()==0&&!f->edit.open()&&f->backend.writes==1);
  mt::SettingsJournal fresh;assert(fresh.load(f->backend)==0&&fresh.config().same(f->service.config));
  f->commit();assert(f->usb.error()==0&&f->backend.writes==1);
  std::cout<<"PASS disabled module no-op preserves open cross-transport batch and one durable commit\n";
 }
 {
  auto f=std::make_unique<Fixture>();f->begin();f->owner("Survives disconnect");
  meshtastic_ToRadio d{};d.which_payload_variant=meshtastic_ToRadio_disconnect_tag;d.disconnect=true;f->input(d);
  f->b.resetStream();assert(f->edit.open()&&f->edit.dirty()&&f->backend.writes==0);
  f->op(meshtastic_AdminMessage_get_owner_request_tag);assert(!strcmp(f->usb.reply().get_owner_response.long_name,"Survives disconnect"));
  assert(f->edit.poll(60001,true)==0&&f->backend.writes==0);assert(f->edit.poll(60002,true)==0&&f->backend.writes==1&&!f->edit.open());
  std::cout<<"PASS disconnect retains live batch; 60 second idle save persists it\n";
 }
 {
  auto f=std::make_unique<Fixture>();f->begin(0xfffffff0);f->owner("Wrapped clock",false,0xfffffff5);
  assert(f->edit.poll(uint32_t(0xfffffff5u+59999),true)==0&&f->edit.open());
  assert(f->edit.poll(uint32_t(0xfffffff5u+60000),true)==0&&!f->edit.open());
  std::cout<<"PASS idle deadline survives 32-bit uptime wrap\n";
 }
 {
  auto f=std::make_unique<Fixture>();f->begin();auto a=lora();a.set_config.payload_variant.lora.region=static_cast<meshtastic_Config_LoRaConfig_RegionCode>(255);
  f->admin(a);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&!f->edit.dirty());
  f->commit();assert(f->usb.error()==0&&f->backend.writes==0);
  std::cout<<"PASS invalid setters leave empty transaction and storage unchanged\n";
 }
 {
  auto f=std::make_unique<Fixture>();f->begin();f->owner("First update",false,2);f->owner("First update",false,10000);
  assert(f->edit.poll(60002,true)==0&&f->edit.open());assert(f->edit.poll(70000,true)==0&&!f->edit.open());
  std::cout<<"PASS accepted unchanged owner refreshes idle activity\n";
 }
 {
  auto f=std::make_unique<Fixture>();f->begin();f->admin(lora());assert(f->usb.error()==0&&f->service.config.provisioned()&&!f->service.receiving());
  f->admin(lora(true));assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&!f->service.endpointArmed());
  f->commit();assert(f->usb.error()==0&&!f->service.receiving()&&!f->service.endpointArmed());
  f->admin(lora(true));
  const bool postCommitArmOk=f->usb.error()==0&&f->service.receiving()&&f->service.endpointArmed();
  if(!postCommitArmOk)std::cerr<<"DIAG post-commit LoRa arm: error="<<f->usb.error()
    <<" receiving="<<f->service.receiving()<<" armed="<<f->service.endpointArmed()
    <<" provisioned="<<f->service.config.provisioned()<<" config_tx="<<f->service.config.txEnabled()
    <<" faulted="<<f->service.faulted()<<" editing="<<f->edit.open()<<" dirty="<<f->edit.dirty()
    <<" writes="<<f->backend.writes<<" radio_state="<<f->radio.state<<" radio_starts="<<f->radio.starts<<'\n';
  assert(postCommitArmOk);
  std::cout<<"PASS LoRa changes neither start RX nor arm TX during edit; explicit post-commit arm works\n";
 }
 {
  auto f=std::make_unique<Fixture>();f->provision();assert(f->service.start(false,0)&&f->service.armEndpoint(true));f->begin();
  assert(!f->service.endpointArmed()&&f->service.receiving());f->owner("Idle RX edit");unsigned writes=f->backend.writes;
  assert(f->edit.poll(90000,true)==0&&f->backend.writes==writes&&f->edit.open());
  f->service.stop();assert(f->edit.poll(90001,false)==0&&f->edit.open());
  assert(f->edit.poll(90002,true)==0&&!f->edit.open());
  std::cout<<"PASS begin disarms idle TX; auto-save waits for RF off plus caller health guard\n";
 }
 {
  auto f=std::make_unique<Fixture>();f->begin();f->owner("Do not drop");meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_reboot_seconds_tag;
  f->admin(a);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&!f->usb.scheduled);
  a.reboot_seconds=-1;f->admin(a);assert(f->usb.error()==0);f->commit();a.reboot_seconds=0;f->admin(a);assert(f->usb.error()==0&&f->usb.scheduled);
  f->op(meshtastic_AdminMessage_begin_edit_settings_tag);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&!f->edit.open());
  std::cout<<"PASS reboot rejected while editing; cancellation accepted; scheduled reboot blocks begin\n";
 }
 {
  auto f=std::make_unique<Fixture>();f->provision();assert(f->service.start(false,0)&&f->service.armEndpoint(true));
  f->commit();assert(f->usb.error()==0&&f->service.endpointArmed());f->begin();f->owner("Commit during RX");unsigned writes=f->backend.writes;
  f->commit();assert(f->usb.error()==0&&!f->edit.open()&&f->backend.writes==writes+1&&f->service.receiving()&&!f->service.endpointArmed());
  std::cout<<"PASS explicit commit quiesces storage and preserves prior RX while leaving TX disarmed\n";
 }
 for(bool uncertain:{false,true}) {
  auto f=std::make_unique<Fixture>();f->owner("Durable before");f->begin();f->owner("Live pending");f->backend.fail=true;f->backend.uncertain=uncertain;
  f->commit();assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&f->edit.open()&&f->edit.dirty());
  assert(!strcmp(reinterpret_cast<const char *>(f->service.config.node().bytes+bench::MtNodeRecord::Name),"Live pending"));
  unsigned writes=f->backend.writes;const auto failedLive=f->service.config;
  f->module(meshtastic_ModuleConfig_mqtt_tag);assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&
    f->backend.writes==writes&&f->edit.open()&&f->edit.dirty()&&f->service.config.same(failedLive));
  assert(f->edit.poll(120000,true)==0&&f->backend.writes==writes);
  f->commit();assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&f->backend.writes==writes);
  assert(f->edit.save(f->service.config,140000)==-EACCES);
  mt::SettingsJournal fresh;assert(fresh.load(f->backend)==0);
  const char *name=reinterpret_cast<const char *>(fresh.config().bytes+6+bench::MtNodeRecord::Name);
  assert(!strcmp(name,uncertain?"Live pending":"Durable before"));
  std::cout<<"PASS failed commit retains pending RAM and refuses module no-op without blind storage retry "<<uncertain<<"\n";
 }
 {
  auto f=std::make_unique<Fixture>();f->begin();f->owner("Before power loss");mt::SettingsJournal fresh;assert(fresh.load(f->backend)==0);
  assert(!fresh.config().same(f->service.config));assert(f->service.route.identity()==Node);
  std::cout<<"PASS uncommitted edits are volatile and do not alter hardware-derived identity\n";
 }
 {
  auto f=std::make_unique<Fixture>();f->begin();meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
  a.set_config.which_payload_variant=meshtastic_Config_display_tag;a.set_config.payload_variant.display.screen_on_secs=45;a.set_config.payload_variant.display.flip_screen=true;f->admin(a);assert(f->usb.error()==0);
  a={};a.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;a.set_channel.index=0;a.set_channel.role=meshtastic_Channel_Role_DISABLED;f->admin(a);assert(f->usb.error()==0&&!f->service.config.node().isPublic());
  f->commit();assert(f->usb.error()==0&&f->backend.writes==1&&f->journal.config().displaySeconds()==45&&f->journal.config().displayFlip()&&!f->journal.config().node().isPublic());
  std::cout<<"PASS display and primary channel share the current Config journal record\n";
 }
 {
  auto f=std::make_unique<Fixture>();f->provision();assert(f->service.start(false,0)&&f->service.armEndpoint(true));
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
  p.to=0xffffffff;p.id=991;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;p.decoded.payload.size=1;p.decoded.payload.bytes[0]='x';f->input(r);
  assert(f->service.endpointFree()==3);f->op(meshtastic_AdminMessage_begin_edit_settings_tag);
  assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&!f->edit.open()&&f->service.endpointArmed()&&f->service.endpointFree()==3);
  assert(f->radio.sends==0);
  std::cout<<"PASS begin refuses outstanding TX without discarding queued message\n";
 }
 {
  auto f=std::make_unique<Fixture>();f->provision();assert(f->service.start(false,0)&&f->service.armEndpoint(true));
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
  p.to=0xffffffff;p.id=992;p.want_ack=true;p.hop_limit=3;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;p.decoded.payload.size=1;p.decoded.payload.bytes[0]='r';f->input(r);
  assert(f->service.endpointFree()==3);f->radio.sends=0;
  f->module(meshtastic_ModuleConfig_telemetry_tag,true,200);
  assert(f->ble.error()==0&&f->service.receiving()&&f->service.endpointArmed()&&f->service.endpointFree()==3&&f->radio.sends==0);
  f->service.poll(300,7);assert(f->radio.sends==1&&f->service.active()&&f->service.endpointFree()==3);
  f->module(meshtastic_ModuleConfig_telemetry_tag,false,301);
  assert(f->usb.error()==0&&f->service.receiving()&&f->service.endpointArmed()&&f->service.active()&&f->service.endpointFree()==3&&f->radio.sends==1);
  std::cout<<"PASS disabled module no-op preserves queued and active retry-capable TX plus RX state\n";
 }
 {
  auto f=std::make_unique<Fixture>(true);f->provision();assert(f->service.start(false,0)&&f->service.armEndpoint(true));
  assert(f->service.submitNodeInfo(0,993,0x87654321,0,true,true,100)==0&&f->service.endpointFree()==3);
  assert(f->service.nodes.edit(0x87654321,Node,mt::NodeCache::Edit::Ignore)==1&&f->service.nodes.dirty());
  const auto before=f->service.config;f->module(meshtastic_ModuleConfig_mesh_beacon_tag,true,101);
  assert(f->ble.error()==0&&f->service.receiving()&&f->service.endpointArmed()&&f->service.endpointFree()==3&&
    f->service.nodes.dirty()&&f->service.nodes.count()==1&&f->service.config.same(before)&&f->backend.writes==1&&f->radio.sends==0);
  std::cout<<"PASS disabled module no-op preserves pending control, dirty NodeDB, durable TX intent and radio state\n";
 }
 {
  auto f=std::make_unique<Fixture>();f->op(meshtastic_AdminMessage_get_device_connection_status_request_tag);
  const auto r=f->usb.reply();const auto &s=r.get_device_connection_status_response;
  assert(r.which_payload_variant==meshtastic_AdminMessage_get_device_connection_status_response_tag);
  assert(s.has_serial&&s.serial.is_connected&&s.serial.baud==115200&&s.has_bluetooth&&s.bluetooth.is_connected);
  assert(!s.has_wifi&&!s.has_ethernet&&!s.bluetooth.pin&&!s.bluetooth.rssi&&f->radio.sends==0&&f->backend.writes==0);
  f->ble.ble=false;f->ble.serial={false,9600,true};f->op(meshtastic_AdminMessage_get_device_connection_status_request_tag,true);
  const auto t=f->ble.reply().get_device_connection_status_response;assert(t.has_serial&&!t.serial.is_connected&&t.serial.baud==9600&&!t.has_bluetooth);
  std::cout<<"PASS connection status reports measured local transports without network/PIN claims\n";
 }
}
