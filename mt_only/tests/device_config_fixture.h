// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Host-only Device configuration fixture. Actual shared editor and journals;
// no transport, credential provider or physical radio is instantiated.
#include "Companion.h"
#include "known_channel_fixture.h"
#include "SettingsEdit.h"
#include "WallClock.h"
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>
constexpr uint32_t Node=0x12345678;
struct Radio:mt::Radio {
 bool configurable=false;unsigned configurations=0;mt::Config selected;
 int configure(const mt::Config &c)override{if(!configurable)return mt::Radio::configure(c);if(!c.valid())return -EINVAL;selected=c;++configurations;return 0;}
 unsigned starts=0,sends=0,stops=0;uint32_t air=100;bool done=false;int state=0;bool ready()override{return true;}
 int start()override{++starts;state=1;return 0;}int restore()override{state=1;return 0;}
 int stop()override{++stops;state=0;return 0;}int health()override{return state;}
 bool busyFlag=false;int pauseResult=0;
 uint16_t busyDetail=0;mutable unsigned busyDetailReads=0;
 uint16_t busyDiagnostic()const override{++busyDetailReads;return busyDetail;}
 int stopForStorage()override{return pauseResult?pauseResult:stop();}
 bool busy()override{return busyFlag;}uint32_t airtime(size_t)override{return air;}
 int send(uint8_t *,size_t)override{++sends;done=false;return 0;}bool completion(int &result)override{result=0;return done;}
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
 mt::WallClock *clock=nullptr;uint64_t clockMs=0;
 bool fittedGnss=false;bool gnssPresent()const override{return fittedGnss;}
 std::string gnssDiagnosticText;mutable unsigned gnssDiagnosticCalls=0;
 size_t gnssDiagnostic(char *out,size_t capacity)const override {
  ++gnssDiagnosticCalls;if(gnssDiagnosticText.empty()||gnssDiagnosticText.size()>=capacity)return 0;
  memcpy(out,gnssDiagnosticText.c_str(),gnssDiagnosticText.size()+1);return gnssDiagnosticText.size();
 }
 bool scheduled=false,ble=true,bleConnected=true,readyFlag=true;Serial serial{true,115200,true};
 int inputResult=-ENOTSUP;std::vector<std::pair<uint32_t,uint32_t>> uiInputs;
 std::function<int(uint32_t,uint32_t)> inputHandler;
 std::function<void(uint32_t,uint32_t,uint32_t,unsigned,const uint8_t *,size_t,int16_t,int8_t,uint32_t,bool)> previewHandler;
 std::vector<meshtastic_FromRadio> out;std::vector<std::vector<uint8_t>> records;unsigned saves=0,deprecatedRoleSaves=0;
 bool overrideDeviceSave=false;int deviceSaveResult=0;
 Host(mt::SettingsEdit &e,mt::SettingsJournal &j):edit(e),journal(j){}
 bool ready()const override{return readyFlag&&journal.ready();}
 int setWallTime(uint32_t value)override{return clock?clock->set(value,clockMs):-ENOTSUP;}
 uint32_t wallTime()const override{return clock?clock->at(clockMs):0;}
 bool bluetooth()const override{return ble;}
 Security bluetoothSecurity()const override{Security s;s.connected=bleConnected;return s;}
 Serial serialStatus()const override{return serial;}
 bool send(const uint8_t *p,size_t n)override {
  assert(n>=4&&size_t(p[2]*256+p[3])==n-4);meshtastic_FromRadio r{};
  auto stream=pb_istream_from_buffer(p+4,n-4);assert(pb_decode(&stream,meshtastic_FromRadio_fields,&r));out.push_back(r);records.emplace_back(p+4,p+n);return true;
 }
 int save(const mt::Config &c)override{++saves;return edit.save(c,now);}
 int saveDevice(const mt::Config &c,bool disabled,uint32_t at)override{
  if(overrideDeviceSave){++saves;now=at;return deviceSaveResult;}
  return mt::Companion::Host::saveDevice(c,disabled,at);
 }
 int saveDeviceDeprecatedRole(const mt::Config &c,bool disabled,uint32_t at)override{
  ++deprecatedRoleSaves;now=at;return mt::Companion::Host::saveDeviceDeprecatedRole(c,disabled,at);
 }
 int uiInput(uint32_t code,uint32_t at)override{
  uiInputs.emplace_back(code,at);return inputHandler?inputHandler(code,at):inputResult;
 }
 void preview(uint32_t from,uint32_t to,uint32_t id,unsigned port,const uint8_t *payload,size_t n,
              int16_t rssi,int8_t snr,uint32_t at,bool local)override{
  if(previewHandler)previewHandler(from,to,id,port,payload,n,rssi,snr,at,local);
 }
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
 explicit Fixture(uint8_t model=127,bool discovery=true):service(radio,true,Node,model,true,discovery),edit(service,journal,backend),usb(edit,journal),ble(edit,journal),c(service,usb,model,workspace),b(service,ble,model,workspace,1) {
    knownChannelClock.bind(service);
  assert(journal.load(backend)==0);assert(service.apply(journal.config()));
 }
 void raw(const uint8_t *bytes,size_t n,bool bluetooth,uint32_t now) {
  (bluetooth?ble:usb).now=now;
  if(bluetooth)b.packet(bytes,n,now,mt::Companion::WorkspaceProof{true});
  else {c.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});c.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});c.byte(n>>8,now,0,mt::Companion::WorkspaceProof{true});c.byte(n,now,0,mt::Companion::WorkspaceProof{true});for(size_t i=0;i<n;++i)c.byte(bytes[i],now,0,mt::Companion::WorkspaceProof{true});}
 }
 void input(const meshtastic_ToRadio &r,bool bluetooth=false,uint32_t now=1) {
  auto &host=bluetooth?ble:usb;host.now=now;uint8_t bytes[512];auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
  assert(pb_encode(&stream,meshtastic_ToRadio_fields,&r));
  raw(bytes,stream.bytes_written,bluetooth,now);
 }
 void admin(const meshtastic_AdminMessage &a,bool bluetooth=false,uint32_t now=1) {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
  p.to=Node;p.id=71;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
  auto stream=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
  assert(pb_encode(&stream,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=stream.bytes_written;input(r,bluetooth,now);
 }
 void op(pb_size_t tag,bool bluetooth=false,uint32_t now=1){meshtastic_AdminMessage a{};a.which_payload_variant=tag;admin(a,bluetooth,now);}
 void begin(uint32_t now=1){op(meshtastic_AdminMessage_begin_edit_settings_tag,false,now);assert(usb.error()==0);}
 void commit(){op(meshtastic_AdminMessage_commit_edit_settings_tag);}
 void owner(const char *name,bool bluetooth=false,uint32_t now=2) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;strcpy(a.set_owner.long_name,name);admin(a,bluetooth,now);
 }
 void provision() {auto cfg=service.config;cfg.bytes[4]=1;cfg.seal();assert(edit.save(cfg,0)==0);}
};
static inline meshtastic_AdminMessage device(uint32_t interval=10800) {
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
 a.set_config.which_payload_variant=meshtastic_Config_device_tag;auto &d=a.set_config.payload_variant.device;
 d.role=meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE;d.serial_enabled=true;d.led_heartbeat_disabled=true;d.node_info_broadcast_secs=interval;return a;
}
static inline uint32_t getter(Fixture &f,bool ble=false) {
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;
 a.get_config_request=meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG;f.admin(a,ble);return (ble?f.ble:f.usb).reply().get_config_response.payload_variant.device.node_info_broadcast_secs;
}
static inline void text(Fixture &f,uint32_t id,uint32_t now) {
 meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
 p.to=0xffffffff;p.id=id;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;p.decoded.payload.size=1;p.decoded.payload.bytes[0]='x';f.input(r,false,now);
}
