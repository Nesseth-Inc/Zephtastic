// SPDX-License-Identifier: GPL-3.0-only
// Independent wire-level settings traces through actual Companion/SettingsEdit.
// Memory storage models fail-before and fail-after-commit; no hardware I/O.
#include "Companion.h"
#include "SettingsEdit.h"
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>
constexpr uint32_t LOCAL=0x12345678;
struct Radio:mt::Radio {
  unsigned sends=0;bool rx=false;bool ready()override{return true;}int start()override{rx=true;return 0;}
  int stop()override{rx=false;return 0;}int restore()override{rx=true;return 0;}int health()override{return rx?1:0;}
  bool busy()override{return false;}uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *,size_t)override{++sends;return 0;}bool completion(int &)override{return false;}
};
struct Backend:mt::SettingsBackend {
  std::map<std::string,std::vector<uint8_t>> bytes;unsigned writes=0;int failure=0;
  int read(const char *key,uint8_t *out,size_t cap)override {
    auto it=bytes.find(key);if(it==bytes.end())return -ENOENT;if(it->second.size()>cap)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());return it->second.size();
  }
  int write(const char *key,const uint8_t *p,size_t n)override {
    ++writes;if(failure==1)return -EIO;bytes[key]={p,p+n};return failure==2?-EIO:0;
  }
};
struct Host:mt::Companion::Host {
  mt::SettingsEdit &edit;mt::SettingsJournal &journal;uint32_t now=0;
  std::vector<meshtastic_FromRadio> out;bool bt=false,linked=false,serial=true;uint32_t baud=115200;unsigned reboots=0;
  Host(mt::SettingsEdit &e,mt::SettingsJournal &j):edit(e),journal(j){}
  bool ready()const override{return journal.ready();}int save(const mt::Config &c)override{return edit.save(c,now);}
  int beginEdit(uint32_t t)override{return edit.begin(t);}int commitEdit(uint32_t t)override{return edit.commit(t);}
  bool editing()const override{return edit.open();}Serial serialStatus()const override{return {serial,baud,true};}
  bool bluetooth()const override{return bt;}Security bluetoothSecurity()const override{Security s;s.connected=linked;return s;}
  bool scheduleReboot(int32_t t)override{if(t>=0)++reboots;return true;}
  bool send(const uint8_t *p,size_t n)override {
    assert(n>=4&&p[0]==0x94&&p[1]==0xc3&&size_t(p[2]*256+p[3])==n-4);
    meshtastic_FromRadio msg{};auto b=pb_istream_from_buffer(p+4,n-4);assert(pb_decode(&b,meshtastic_FromRadio_fields,&msg));out.push_back(msg);return true;
  }
  int error(uint32_t id)const {
    for(const auto &r:out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.decoded.request_id==id&&r.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP) {
      meshtastic_Routing v{};auto b=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);assert(pb_decode(&b,meshtastic_Routing_fields,&v));return v.error_reason;
    }
    return -1;
  }
  meshtastic_AdminMessage response(uint32_t id)const {
    meshtastic_AdminMessage a{};
    for(const auto &r:out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.decoded.request_id==id&&r.packet.decoded.portnum==meshtastic_PortNum_ADMIN_APP) {
      auto b=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);assert(pb_decode(&b,meshtastic_AdminMessage_fields,&a));return a;
    }
    return a;
  }
};
struct Fixture {
  Radio radio;mt::Service service;Backend backend;mt::SettingsJournal journal;mt::SettingsEdit edit;Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion u,b;uint32_t id=0;
  Fixture():service(radio,true,LOCAL,127,true,false),edit(service,journal,backend),usb(edit,journal),ble(edit,journal),u(service,usb,127,workspace,0),b(service,ble,127,workspace,1) {
    mt::Config c;c.bytes[4]=1;auto node=c.node();node.setPublic();c.node(node);
    assert(!journal.load(backend)&&!journal.save(backend,c)&&service.apply(c));
  }
  void input(const meshtastic_ToRadio &r,bool wireless,uint32_t now) {
    usb.now=ble.now=now;uint8_t bytes[512];auto s=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&s,meshtastic_ToRadio_fields,&r));
    if(wireless)b.packet(bytes,s.bytes_written,now,mt::Companion::WorkspaceProof{true});
    else {u.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});u.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});u.byte(s.bytes_written>>8,now,0,mt::Companion::WorkspaceProof{true});u.byte(s.bytes_written,now,0,mt::Companion::WorkspaceProof{true});for(size_t i=0;i<s.bytes_written;++i)u.byte(bytes[i],now,0,mt::Companion::WorkspaceProof{true});}
  }
  uint32_t admin(meshtastic_AdminMessage a,bool wireless=false,uint32_t now=100) {
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;p.id=++id;p.to=LOCAL;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
    auto s=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));assert(pb_encode(&s,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=s.bytes_written;input(r,wireless,now);return id;
  }
  uint32_t begin(bool wireless=false,uint32_t now=100){meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_begin_edit_settings_tag;a.begin_edit_settings=true;return admin(a,wireless,now);}
  uint32_t commit(bool wireless=false,uint32_t now=300){meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_commit_edit_settings_tag;a.commit_edit_settings=true;return admin(a,wireless,now);}
  uint32_t name(const char *name,bool wireless=false,uint32_t now=200){meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;strcpy(a.set_owner.long_name,name);return admin(a,wireless,now);}
};
static unsigned failed=0;
static void check(bool ok,const char *name){failed+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";}
static void fixtureRadioRestore() {
  Radio radio;const bool started=radio.start()==0&&radio.health()==1;
  const bool stopped=radio.stop()==0&&radio.health()==0;
  const bool restored=radio.restore()==0&&radio.health()==1;
  check(started&&stopped&&restored,"fixture_radio_restore_matches_health_contract");
}
static void sharedBatch() {
  auto f=std::make_unique<Fixture>();const auto before=f->backend.writes;
  const auto begin=f->begin();const auto name=f->name("USB named");
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_owner_request_tag;a.get_owner_request=true;
  auto query=f->admin(a,true,220);auto response=f->ble.response(query);
  check(f->usb.error(begin)==0&&f->usb.error(name)==0&&!strcmp(response.get_owner_response.long_name,"USB named")&&f->backend.writes==before,"setters_visible_cross_transport_before_persistence");
  meshtastic_ToRadio d{};d.which_payload_variant=meshtastic_ToRadio_disconnect_tag;d.disconnect=true;f->input(d,false,230);
  f->name("BLE latest",true,240);const auto commit=f->commit(true,250);mt::SettingsJournal reboot;assert(!reboot.load(f->backend));
  check(f->ble.error(commit)==0&&!f->edit.open()&&f->backend.writes==before+1&&reboot.config().same(f->service.config)&&!strcmp(reinterpret_cast<const char *>(reboot.config().node().bytes+bench::MtNodeRecord::Name),"BLE latest")&&f->service.route.identity()==LOCAL,"disconnect_preserves_shared_batch_other_transport_commits_once");
  const auto again=f->commit();check(f->usb.error(again)==0&&f->backend.writes==before+1,"repeated_commit_is_inert");
}
static void uncertainStorage() {
  bool good=true;
  for(int fail:{1,2}) {
    auto f=std::make_unique<Fixture>();const auto old=f->service.config;f->begin();f->name("Pending name");const auto live=f->service.config;
    f->backend.failure=fail;const auto id=f->commit();const auto writes=f->backend.writes;
    for(unsigned i=1;i<=5;++i)f->edit.poll(60000*i+300,true);
    f->commit(true,400000);mt::SettingsJournal reboot;f->backend.failure=0;assert(!reboot.load(f->backend));
    good&=f->usb.error(id)!=0&&f->edit.open()&&f->edit.dirty()&&!f->journal.ready()&&f->backend.writes==writes&&f->service.config.same(live)&&reboot.config().same(fail==1?old:live)&&!f->service.receiving()&&!f->service.endpointArmed()&&!f->radio.sends;
  }
  check(good,"uncertain_storage_never_false_success_retry_storm_or_hidden_rollback");
}
static void idleBoundaries() {
  auto f=std::make_unique<Fixture>();const uint32_t start=0xffffff00u,edited=start+100;const auto writes=f->backend.writes;
  f->begin(false,start);f->name("Wrap name",false,edited);
  f->edit.poll(edited+59999,true);bool good=f->edit.open()&&f->backend.writes==writes;
  f->edit.poll(edited+60000,false);good&=f->edit.open()&&f->backend.writes==writes;
  f->edit.poll(edited+60000,true);good&=!f->edit.open()&&f->backend.writes==writes+1;
  check(good,"idle_commit_wrap_boundary_requires_explicit_quiescence");
  f=std::make_unique<Fixture>();assert(f->service.start(false,0));f->begin();f->name("RX pending");const auto rxWrites=f->backend.writes;
  f->edit.poll(600000,true);check(f->edit.open()&&f->backend.writes==rxWrites&&f->service.receiving(),"continuous_rx_delays_idle_persistence_intentional_stock_difference");
}
static void securityAndStatus() {
  auto f=std::make_unique<Fixture>();f->begin();meshtastic_AdminMessage a{};
  a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;a.set_config.which_payload_variant=meshtastic_Config_lora_tag;auto &l=a.set_config.payload_variant.lora;
  l.region=meshtastic_Config_LoRaConfig_RegionCode_US;l.use_preset=true;l.hop_limit=3;l.tx_enabled=true;l.sx126x_rx_boosted_gain=true;l.tx_power=-9;
  auto tx=f->admin(a);a={};a.which_payload_variant=meshtastic_AdminMessage_reboot_seconds_tag;a.reboot_seconds=0;auto reboot=f->admin(a);a.reboot_seconds=-1;auto cancel=f->admin(a);
  check(f->usb.error(tx)==32&&f->usb.error(reboot)==32&&f->usb.error(cancel)==0&&!f->usb.reboots&&!f->service.endpointArmed()&&!f->radio.sends,"editing_never_arms_tx_or_schedules_reboot_cancel_remains_available");
  bool good=true;for(bool supported:{false,true})for(bool connected:{false,true}) {
    f->ble.bt=supported;f->ble.linked=connected;f->ble.serial=connected;f->ble.baud=connected?57600:115200;
    a={};a.which_payload_variant=meshtastic_AdminMessage_get_device_connection_status_request_tag;a.get_device_connection_status_request=true;
    const auto id=f->admin(a,true);auto response=f->ble.response(id);const auto &s=response.get_device_connection_status_response;
    good&=response.which_payload_variant==meshtastic_AdminMessage_get_device_connection_status_response_tag&&s.has_serial&&s.serial.is_connected==connected&&s.serial.baud==f->ble.baud&&s.has_bluetooth==supported&&(!supported||s.bluetooth.is_connected==connected)&&!s.bluetooth.pin&&!s.bluetooth.rssi&&!s.has_wifi&&!s.has_ethernet;
  }
  check(good,"connection_status_reports_supplied_live_transport_facts_only");
}
int main(){fixtureRadioRestore();sharedBatch();uncertainStorage();idleBoundaries();securityAndStatus();return failed?1:0;}
