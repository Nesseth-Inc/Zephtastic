// SPDX-License-Identifier: GPL-3.0-only
// Real companion + shared editor + journal; pinned stock transaction semantics
// are documented in SETTINGS-EDIT-M8.md. No hardware, credentials or RF.
#include "Companion.h"
#include "known_channel_fixture.h"
#include "SettingsEdit.h"
#include "ModuleEdit.h"
#include "StatusMessagePayload.h"
#include "TelemetryAnnouncement.h"
#include "ModuleSchedule.h"
#include "NeighborObservations.h"
#include "ModuleController.h"
#include <limits>
#include "SettingsCommit.h"
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>
constexpr uint32_t Node=0x12345678;
struct Radio:mt::Radio {
 unsigned starts=0,sends=0;uint8_t lastChannel=0;int state=0;bool ready()override{return true;}
 // Explicit opt-in: this synthetic backend records real Config resolution;
 // it does not configure a modem or use Radio's LongFast-only fallback.
 mt::LoRaPresets::Profile configuredProfile{};
 int configure(const mt::Config&c)override{mt::USChannelSelection channel{};return c.resolveRadio(configuredProfile,channel)?0:-EINVAL;}
 int start()override{++starts;state=1;return 0;}int restore()override{state=1;return 0;}
 int stop()override{state=0;return 0;}int health()override{return state;}
 bool busy()override{return false;}uint32_t airtime(size_t)override{return 100;}
 int send(uint8_t *frame,size_t length)override{assert(length>13);lastChannel=frame[13];++sends;return 0;}bool completion(int &)override{return false;}
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
 mt::SettingsEdit &edit;mt::SettingsJournal &journal;mt::ModuleEdit &modules;uint32_t now=1;
 mt::ModuleController *observations=nullptr;mt::Service *service=nullptr;uint64_t uptime=0;
 unsigned neighborInfoCallbacks=0;
 int localNeighbors(meshtastic_NeighborInfo&out)override{return observations&&service?observations->localNeighbors(*service,modules.config(),ready()&&modules.ready(),uptime,out):-ENOTSUP;}
 bool scheduled=false,ble=true,bleConnected=true;Serial serial{true,115200,true};
 std::vector<meshtastic_FromRadio> out;size_t capacity=SIZE_MAX;bool rejectComplete=false;
 Host(mt::SettingsEdit &e,mt::SettingsJournal &j,mt::ModuleEdit &m):edit(e),journal(j),modules(m){}
 bool ready()const override{return journal.ready();}
 uint32_t supportedModules()const override{return meshtastic_ExcludedModules_STATUSMESSAGE_CONFIG|meshtastic_ExcludedModules_NEIGHBORINFO_CONFIG|meshtastic_ExcludedModules_RANGETEST_CONFIG|meshtastic_ExcludedModules_TELEMETRY_CONFIG;}
 bool moduleConfig(meshtastic_ModuleConfig &out,unsigned tag)const override{return modules.ready()&&modules.config().get(out,tag);}
 int saveModuleConfig(const meshtastic_ModuleConfig &config,uint32_t t)override{const int result=modules.save(config,t,edit.open());if(!result)edit.activity(t);return result;}

 bool bluetooth()const override{return ble;}
 void observeRadioNeighborInfo(uint32_t from,unsigned hops,bool known,float snr,uint32_t now,
                               const meshtastic_NeighborInfo&info)override {
  (void)now;
  ++neighborInfoCallbacks;
  if(observations&&service)observations->observeNeighborInfo(*service,from,service->route.identity(),hops,uptime,known,snr,info);
 }
 Security bluetoothSecurity()const override{Security s;s.connected=bleConnected;return s;}
 Serial serialStatus()const override{return serial;}
 bool send(const uint8_t *p,size_t n)override {
  if(out.size()>=capacity)return false;
  assert(n>=4&&size_t(p[2]*256+p[3])==n-4);meshtastic_FromRadio r{};
  auto stream=pb_istream_from_buffer(p+4,n-4);assert(pb_decode(&stream,meshtastic_FromRadio_fields,&r));if(rejectComplete&&r.which_payload_variant==meshtastic_FromRadio_config_complete_id_tag)return false;out.push_back(r);return true;
 }
 int save(const mt::Config &c)override{return edit.save(c,now);}
 int beginEdit(uint32_t t)override{return scheduled?-EACCES:edit.begin(t);}
 int commitEdit(uint32_t t)override{const int saved=modules.commit(t);return saved?saved:edit.commit(t);}
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
 Radio radio;Backend backend;mt::SettingsJournal journal;mt::ModuleJournal modules;mt::Service service;
 mt::SettingsEdit edit;mt::ModuleEdit moduleEdit;Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion c,b;
 explicit Fixture(bool discovery=false):service(radio,true,Node,127,true,discovery),edit(service,journal,backend),moduleEdit(service,modules,backend),usb(edit,journal,moduleEdit),ble(edit,journal,moduleEdit),c(service,usb,127,workspace),b(service,ble,127,workspace,1) {
    knownChannelClock.bind(service);
  assert(journal.load(backend)==0&&modules.load(backend)==0&&moduleEdit.load());assert(service.apply(journal.config()));usb.service=ble.service=&service;
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

static meshtastic_AdminMessage status(const char*text){meshtastic_AdminMessage m{};m.which_payload_variant=meshtastic_AdminMessage_set_module_config_tag;m.set_module_config.which_payload_variant=meshtastic_ModuleConfig_statusmessage_tag;strcpy(m.set_module_config.payload_variant.statusmessage.node_status,text);return m;}
static std::vector<std::vector<uint8_t>> canonicalStream(const std::vector<meshtastic_FromRadio>&messages){
 std::vector<std::vector<uint8_t>> out;for(auto message:messages){message.id=0;uint8_t bytes[512]{};auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&stream,meshtastic_FromRadio_fields,&message));out.emplace_back(bytes,bytes+stream.bytes_written);}return out;
}
static void group(const char*name){std::cout<<"PASS "<<name<<"\n";}
#include "module_controller_runtime_cases.inc"
int main(){
 {auto f=std::make_unique<Fixture>();f->admin(status("Ready"));assert(f->usb.error()==0&&f->backend.writes==1&&f->modules.config().same(f->moduleEdit.config()));meshtastic_AdminMessage get{};get.which_payload_variant=meshtastic_AdminMessage_get_module_config_request_tag;get.get_module_config_request=meshtastic_AdminMessage_ModuleConfigType_STATUSMESSAGE_CONFIG;f->admin(get,true);assert(!strcmp(f->ble.reply().get_module_config_response.payload_variant.statusmessage.node_status,"Ready"));group("shared local module setter, verified journal and opposite-transport getter");}
 {auto f=std::make_unique<Fixture>();f->begin();f->admin(status("Pending"),true,15);assert(f->ble.error()==0&&f->moduleEdit.dirty()&&f->backend.writes==0&&f->edit.due(60015)&&!f->edit.due(60014));f->commit();assert(f->usb.error()==0&&!f->edit.open()&&!f->moduleEdit.dirty()&&f->backend.writes==1);mt::ModuleJournal reboot;assert(reboot.load(f->backend)==0&&reboot.config().same(f->moduleEdit.config()));group("begin/commit shares module visibility and refreshes idle activity");}
 {auto f=std::make_unique<Fixture>();auto m=status("Good");f->admin(m);const auto original=f->moduleEdit.config();unsigned writes=f->backend.writes;m.set_module_config.which_payload_variant=meshtastic_ModuleConfig_telemetry_tag;m.set_module_config.payload_variant.telemetry={};m.set_module_config.payload_variant.telemetry.power_screen_enabled=true;f->admin(m);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&f->backend.writes==writes&&f->moduleEdit.config().same(original));group("unsupported power screen configuration rejects without changing module state");}
 {auto f=std::make_unique<Fixture>();f->module(meshtastic_ModuleConfig_mqtt_tag);assert(f->usb.error()==0&&f->backend.writes==0);meshtastic_AdminMessage m{};m.which_payload_variant=meshtastic_AdminMessage_set_module_config_tag;m.set_module_config.which_payload_variant=meshtastic_ModuleConfig_mqtt_tag;m.set_module_config.payload_variant.mqtt.enabled=true;f->admin(m);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&f->backend.writes==0);group("excluded modules retain exact disabled no-op compatibility");}
 {auto f=std::make_unique<Fixture>();auto m=status("Unknown");meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto&p=r.packet;p.to=Node;p.id=99;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;auto stream=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));assert(pb_encode(&stream,meshtastic_AdminMessage_fields,&m));p.decoded.payload.size=stream.bytes_written;for(uint8_t b:{uint8_t(0xf8),uint8_t(7),uint8_t(1)})p.decoded.payload.bytes[p.decoded.payload.size++]=b;f->input(r);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&f->backend.writes==0);group("unknown Admin wire fields cannot become a module write");}
 {auto f=std::make_unique<Fixture>();f->backend.fail=f->backend.uncertain=true;const auto original=f->moduleEdit.config();f->admin(status("Uncertain"));assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&!f->moduleEdit.ready()&&f->moduleEdit.config().same(original));unsigned writes=f->backend.writes;f->admin(status("Retry"));assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&f->backend.writes==writes);group("uncertain module commit rejects further blind writes");}
 {auto f=std::make_unique<Fixture>();f->provision();auto cfg=f->service.config;cfg.setTxEnabled(true);assert(f->edit.save(cfg,0)==0&&f->service.start(false,0)&&f->service.armEndpoint(true));const auto original=f->service.config;f->admin(status("Retain"));assert(f->usb.error()==0&&f->service.config.same(original)&&f->service.receiving()&&f->service.endpointArmed()&&f->service.route.identity()==Node&&f->radio.sends==0);group("module persistence preserves core identity, RX and prior TX intent without RF");}

 {char text[80]{};uint8_t bytes[100];memset(bytes,0xaa,sizeof(bytes));size_t size=9;using P=mt::StatusMessagePayload;assert(P::encode(text,bytes,sizeof(bytes),size)==P::Result::Disabled&&size==0&&bytes[0]==0xaa);memset(text,'x',sizeof(text));assert(P::encode(text,bytes,sizeof(bytes),size)==P::Result::Invalid&&bytes[0]==0xaa);text[79]=0;assert(P::encode(text,bytes,1,size)==P::Result::Invalid&&bytes[0]==0xaa);assert(P::encode(text,bytes,sizeof(bytes),size)==P::Result::Encoded);meshtastic_StatusMessage decoded{};auto in=pb_istream_from_buffer(bytes,size);assert(pb_decode(&in,meshtastic_StatusMessage_fields,&decoded)&&strlen(decoded.status)==79);group("status disabled, bounded text and nonpartial encoded payload");}
 {uint8_t bytes[128]{};size_t size=0;assert(mt::TelemetryAnnouncement::encode(123,zt::Observed<float>{},zt::Observed<float>{},bytes,sizeof(bytes),size));meshtastic_Data d{};auto in=pb_istream_from_buffer(bytes,size);assert(pb_decode(&in,meshtastic_Data_fields,&d)&&d.portnum==67&&d.has_bitfield&&d.bitfield==0&&!d.request_id&&!d.want_response);meshtastic_Telemetry t{};in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&in,meshtastic_Telemetry_fields,&t)&&t.variant.device_metrics.has_uptime_seconds&&t.variant.device_metrics.uptime_seconds==123&&!t.variant.device_metrics.has_voltage&&!t.variant.device_metrics.has_battery_level);assert(!mt::TelemetryQuery::uptimeResponse(123,0,{}, {},bytes,sizeof(bytes),size));group("unsolicited measured DeviceMetrics never weakens query guard");}
 {mt::ModuleSchedule schedule;schedule.configure(0xfffffff0u,100,32);assert(!schedule.due(0xfffffff1u)&&!schedule.due(15)&&schedule.due(16));schedule.consumed(200);assert(!schedule.due(299)&&schedule.due(300));schedule.consumed(90000);assert(!schedule.due(90000)&&schedule.due(90100));schedule.configure(0,0x80000000u,0);assert(!schedule.due(0));schedule.configure(0,1,0x80000000u);assert(!schedule.due(0));schedule.configure(0,1,0);assert(schedule.due(0));schedule.disable();assert(!schedule.due(1));group("wrap-safe module scheduling has no catchup burst");}

 {mt::NeighborObservations table;mt::NodeCache cache;meshtastic_NeighborInfo out{};assert(!table.snapshot(cache,Node,14400,0,out));assert(!table.observe(0,Node,0,100,true,1)&&!table.observe(Node,Node,0,100,true,1)&&!table.observe(77,Node,1,100,true,1));assert(table.observe(77,Node,0,100,true,-1.25f,14400));assert(table.direct(77));assert(table.snapshot(cache,Node,14400,200,out)&&out.neighbors_count==1&&out.neighbors[0].node_id==77&&out.neighbors[0].snr==-1.25f&&!out.neighbors[0].last_rx_time&&!out.neighbors[0].node_broadcast_interval_secs);assert(table.snapshot(cache,Node,14400,28800100,out));assert(!table.snapshot(cache,Node,14400,28801000,out));group("direct neighbors expire on stock whole-second boundary and omit peer timestamps");}
 {mt::NeighborObservations table;mt::NodeCache cache;meshtastic_NeighborInfo out{};
  assert(table.observeAnnouncement(77,Node,88,21600,14400,1,100,true,-3.5f));
  assert(!table.direct(88)&&!table.direct(77));
  assert(table.snapshot(cache,Node,14400,100,out)&&out.neighbors_count==1&&out.neighbors[0].node_id==88);
  assert(table.snapshot(cache,Node,14400,28800100,out)&&out.neighbors_count==1); // configured fallback applies to an indirect claim.
  assert(table.snapshot(cache,Node,14400,28800101,out)); // Same whole second as the TTL boundary.
  assert(!table.snapshot(cache,Node,14400,28801000,out)); // Stock getTime() advances to the next second.
  assert(table.observeAnnouncement(88,Node,88,28800,14400,0,28800200,true,-1.f));
  assert(table.direct(88)); // Original sender's advertised interval refreshes the entry.
  assert(table.snapshot(cache,Node,14400,86400200,out)&&out.neighbors_count==1);
  assert(table.observeAnnouncement(89,Node,88,21600,14400,1,86400200,true,-4.f));
  assert(!table.direct(88)); // A later relayed report never upgrades its SNR to direct RF.
  assert(table.snapshot(cache,Node,14400,144000200,out)&&out.neighbors_count==1);
  assert(table.snapshot(cache,Node,14400,144000200,out));
  assert(!table.snapshot(cache,Node,14400,144001000,out));
  group("stock NeighborInfo tracks only last_sent_by_id and applies per-entry two-interval expiry");}
 {for(bool router:{false,true}){mt::NeighborObservations table;mt::NodeCache cache;meshtastic_NeighborInfo out{};const uint32_t advertised=mt::NeighborIntervals::advertisedSeconds(0,router);assert(advertised==(router?43200u:3600u));assert(table.observeAnnouncement(77,Node,88,0,0,1,100,true,-1.f));assert(table.snapshot(cache,Node,advertised,999,out)&&out.neighbors_count==1);assert(!table.snapshot(cache,Node,advertised,1000,out));mt::NeighborObservations directDefault;assert(directDefault.observe(77,Node,0,100,true,-1.f));assert(directDefault.snapshot(cache,Node,advertised,999,out)&&out.neighbors_count==1);assert(!directDefault.snapshot(cache,Node,advertised,1000,out));}
  mt::NeighborObservations explicitInterval;mt::NodeCache cache;meshtastic_NeighborInfo out{};assert(explicitInterval.observeAnnouncement(77,Node,88,0,21600,1,0,true,-1.f));assert(explicitInterval.snapshot(cache,Node,3600,43200000,out)&&out.neighbors_count==1);assert(!explicitInterval.snapshot(cache,Node,3600,43201000,out));
  mt::NeighborObservations direct;assert(direct.observe(88,Node,0,0,true,-1.f,21600));assert(direct.snapshot(cache,Node,3600,43200000,out)&&out.neighbors_count==1);assert(!direct.snapshot(cache,Node,3600,43201000,out));
  group("zero interval remains zero for indirect cache expiry in client/router roles; explicit interval uses stock two-interval seconds");}
 {mt::NeighborObservations table;mt::NodeCache cache;meshtastic_NeighborInfo out{};for(uint32_t id=10;id<22;++id)assert(table.observe(id,Node,0,uint64_t(id),false,0));assert(table.snapshot(cache,Node,14400,30,out)&&out.neighbors_count==10);for(unsigned i=0;i<out.neighbors_count;++i)assert(out.neighbors[i].node_id>=12&&out.neighbors[i].snr==0);assert(cache.edit(12,Node,mt::NodeCache::Edit::Ignore)>=0);assert(table.snapshot(cache,Node,14400,30,out)&&out.neighbors_count==9);table.clear();assert(!table.snapshot(cache,Node,14400,30,out));group("bounded neighbor replacement, unknown SNR and local ignore policy");}
 {mt::NeighborObservations table;mt::NodeCache cache;meshtastic_NeighborInfo out{};for(uint32_t id=100;id<110;++id)assert(table.observe(id,Node,0,500,false,0));assert(table.observe(110,Node,0,500,false,0)&&table.observe(111,Node,0,500,false,0));assert(table.snapshot(cache,Node,14400,500,out)&&out.neighbors_count==10);for(unsigned i=0;i<10;++i)assert(out.neighbors[i].node_id==102+i);mt::NeighborObservations zero;assert(zero.observeAnnouncement(77,Node,0,0,14400,0,100,true,-1.f)&&!zero.snapshot(cache,Node,14400,100,out));group("full table replacement preserves stock FIFO order for equal local timestamps and zero sentinel does not report self");}
 {uint8_t payload[16];memset(payload,0xaa,sizeof(payload));size_t size=1;assert(!mt::RangeTestPayload::encode(0,payload,sizeof(payload),size)&&!size&&payload[0]==0xaa);assert(!mt::RangeTestPayload::encode(1,payload,4,size)&&!size&&payload[0]==0xaa);assert(mt::RangeTestPayload::encode(UINT32_MAX,payload,sizeof(payload),size)&&size==14&&!memcmp(payload,"seq 4294967295",14));group("range sender has bounded stock ASCII sequence bytes");}
 {auto f=std::make_unique<Fixture>();mt::ModuleController modules;mt::ModuleSettings settings;std::vector<meshtastic_Data> packets;auto sink=[&](const uint8_t*p,size_t n,uint32_t id){assert(id);meshtastic_Data d{};auto stream=pb_istream_from_buffer(p,n);assert(pb_decode(&stream,meshtastic_Data_fields,&d));packets.push_back(d);};modules.poll(f->service,settings,true,0,91,sink);modules.poll(f->service,settings,true,59999,92,sink);assert(packets.empty());modules.poll(f->service,settings,true,60000,93,sink);assert(packets.size()==2&&f->radio.sends==0);meshtastic_Telemetry t{};auto stream=pb_istream_from_buffer(packets[1].payload.bytes,packets[1].payload.size);assert(pb_decode(&stream,meshtastic_Telemetry_fields,&t)&&t.which_variant==meshtastic_Telemetry_local_stats_tag&&t.variant.local_stats.uptime_seconds==60&&t.variant.local_stats.num_total_nodes==1&&!packets[1].request_id);modules.poll(f->service,settings,true,60001,94,sink);assert(packets.size()==2);modules.poll(f->service,settings,true,900000,95,sink);assert(packets.size()==3);group("default modules publish measured local telemetry without background RF or catchup");}
 {auto f=std::make_unique<Fixture>();mt::ModuleController modules;mt::ModuleSettings settings;meshtastic_ModuleConfig range{};range.which_payload_variant=meshtastic_ModuleConfig_range_test_tag;range.payload_variant.range_test.enabled=true;range.payload_variant.range_test.sender=1;assert(settings.set(range));std::vector<meshtastic_Data> packets;auto sink=[&](const uint8_t*p,size_t n,uint32_t){meshtastic_Data d{};auto stream=pb_istream_from_buffer(p,n);assert(pb_decode(&stream,meshtastic_Data_fields,&d));if(d.portnum==meshtastic_PortNum_RANGE_TEST_APP)packets.push_back(d);};modules.poll(f->service,settings,true,0,91,sink);modules.poll(f->service,settings,true,4999,92,sink);assert(packets.empty());modules.poll(f->service,settings,true,5000,93,sink);assert(packets.size()==1&&!memcmp(packets[0].payload.bytes,"seq 1",5));modules.poll(f->service,settings,true,1000000,94,sink);assert(packets.size()==2&&!memcmp(packets[1].payload.bytes,"seq 2",5));auto changed=status("Only status changed").set_module_config;assert(settings.set(changed));modules.poll(f->service,settings,true,28799000,95,sink);assert(packets.size()==3);modules.poll(f->service,settings,true,28800000,96,sink);assert(packets.size()==3);group("range starts after five seconds, consumes failed sends and stops at eight hours");}
 {auto f=std::make_unique<Fixture>();mt::ModuleController modules;mt::ModuleSettings settings;auto m=status("Working").set_module_config;assert(settings.set(m));m={};m.which_payload_variant=meshtastic_ModuleConfig_neighbor_info_tag;m.payload_variant.neighbor_info.enabled=true;m.payload_variant.neighbor_info.transmit_over_lora=true;assert(settings.set(m));unsigned statuses=0,neighbors=0;auto sink=[&](const uint8_t*p,size_t n,uint32_t){meshtastic_Data d{};auto stream=pb_istream_from_buffer(p,n);assert(pb_decode(&stream,meshtastic_Data_fields,&d));statuses+=d.portnum==meshtastic_PortNum_NODE_STATUS_APP;neighbors+=d.portnum==meshtastic_PortNum_NEIGHBORINFO_APP;};modules.poll(f->service,settings,true,0,91,sink);modules.observe(77,Node,0,1,true,-2.5f);modules.poll(f->service,settings,true,119999,92,sink);assert(!statuses&&!neighbors);modules.poll(f->service,settings,true,120000,93,sink);assert(statuses==1&&!neighbors);modules.poll(f->service,settings,true,3600000,94,sink);assert(!neighbors&&f->radio.sends==0);modules.poll(f->service,settings,true,3600001,95,sink);assert(!neighbors);group("status cadence remains local-only; zero-interval neighbor rows expire on stock whole-second TTL");}
 {
  auto f=std::make_unique<Fixture>();f->provision();
  auto cfg=f->service.config;cfg.setTxEnabled(true);
  mt::ChannelRecord secondary;const uint8_t alias=1;
  assert(secondary.set(2,"Test",&alias,1)&&cfg.setSecondaryChannel(1,secondary));
  assert(f->edit.save(cfg,0)==0&&f->service.start(false,0)&&f->service.armEndpoint(true));
  const auto primaryHash=f->service.codec.channelHash();assert(f->service.selectChannel(1));
  const auto secondaryHash=f->service.codec.channelHash();assert(primaryHash!=secondaryHash);
  mt::ModuleController controller;mt::ModuleSettings settings;
  assert(settings.set(status("Bound to primary").set_module_config));
  auto sink=[](const uint8_t *,size_t,uint32_t){};
  controller.poll(f->service,settings,true,0,31,sink);
  controller.poll(f->service,settings,true,120000,32,sink);
  assert(f->service.moduleAnnouncementPending()&&f->service.codec.channelHash()==secondaryHash);
  f->service.poll(120250,33);
  assert(f->radio.sends==1&&f->radio.lastChannel==primaryHash);
  group("scheduled status uses the primary broadcast channel and restores the caller's secondary");
 }
 {auto f=std::make_unique<Fixture>();f->provision();auto cfg=f->service.config;cfg.setTxEnabled(true);assert(f->edit.save(cfg,0)==0&&f->service.start(false,0)&&f->service.armEndpoint(true));uint8_t bytes[128]{};size_t size=0;assert(mt::TelemetryAnnouncement::encode(1,{}, {},bytes,sizeof(bytes),size));uint32_t id=0;assert(f->service.submitModuleAnnouncement(bytes,size,0,0,102,id)==0&&id);assert(f->service.endpointFree()==4);uint32_t second=1;assert(f->service.submitModuleAnnouncement(bytes,size,0,1,103,second)==-EAGAIN&&!second);f->service.poll(249,104);assert(f->radio.sends==0);f->service.poll(250,105);assert(f->radio.sends==1);mt::Endpoint::Result result;assert(!f->service.takeMessageResult(0,result)&&!f->service.takeMessageResult(1,result));group("module control queue sends once without consuming client slots or client result ownership");}
 {uint8_t bytes[128]{};size_t size=0;bool telemetry=false;assert(mt::TelemetryAnnouncement::encode(1,{}, {},bytes,sizeof(bytes),size)&&mt::ModuleAnnouncement::canonical(bytes,size,telemetry)&&telemetry);meshtastic_Data d{};d.portnum=meshtastic_PortNum_NODE_STATUS_APP;d.has_bitfield=true;auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&stream,meshtastic_Data_fields,&d)&&!mt::ModuleAnnouncement::canonical(bytes,stream.bytes_written,telemetry));d.payload.size=1;d.payload.bytes[0]=1;d.request_id=1;stream=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&stream,meshtastic_Data_fields,&d)&&!mt::ModuleAnnouncement::canonical(bytes,stream.bytes_written,telemetry));assert(!mt::LocalStatsQuery::uptimeResponse(1,0,{}, {},bytes,sizeof(bytes),size));group("internal module admission rejects empty or correlated control bodies");}
 {auto f=std::make_unique<Fixture>();f->provision();auto cfg=f->service.config;cfg.setTxEnabled(true);assert(f->edit.save(cfg,0)==0&&f->service.start(false,0)&&f->service.armEndpoint(true));mt::NodeJournal nodes;assert(!nodes.load(f->backend,f->service.nodes,Node));mt::SettingsCommit commit(f->service,f->radio,f->edit,nodes,f->backend,&f->moduleEdit);f->begin();f->admin(status("Combined"),false,2);assert(f->moduleEdit.dirty()&&f->edit.open()&&!f->service.endpointArmed());assert(!commit.commit(60002)&&!f->moduleEdit.dirty()&&!f->edit.open()&&f->service.receiving()&&f->service.endpointArmed());mt::ModuleJournal reboot;assert(!reboot.load(f->backend)&&reboot.config().same(f->moduleEdit.config()));group("shared coordinator verifies module journal before restoring core receive and TX intent");}

 {mt::PeerStatus value;assert(!value.present());assert(mt::StatusMessagePayload::decode(nullptr,0,value)&&value.present()&&!value.text()[0]);value.clear();assert(!value.present());group("peer status distinguishes initialized absence, received clear and lifecycle clear");}
 {char text[80]{};memset(text,'X',79);uint8_t bytes[81]{};size_t n=0;mt::PeerStatus value;assert(mt::StatusMessagePayload::encode(text,bytes,sizeof(bytes),n)==mt::StatusMessagePayload::Result::Encoded&&n==81&&mt::StatusMessagePayload::decode(bytes,n,value)&&strlen(value.text())==79);const auto before=value;const std::vector<std::vector<uint8_t>> bad={{0x0a,2,'A'},{0x10,1},{0x0a,1,'A',0x0a,1,'B'},{0x0a,0x81,0,'A'},{0x0a,3,'A',0,'B'}};for(const auto&b:bad){assert(!mt::StatusMessagePayload::decode(b.data(),b.size(),value)&&!memcmp(&value,&before,sizeof(value)));}assert(!mt::StatusMessagePayload::decode(bytes,sizeof(bytes)+1,value)&&!memcmp(&value,&before,sizeof(value)));group("status maximum text and atomic rejection of malformed unknown duplicate nonminimal or embedded-NUL payloads");}
 {auto f=std::make_unique<Fixture>();mt::NodeCache::Entry entry{};entry.identity(77,81,0);assert(f->service.nodes.accept(entry,Node,-2,1)>=0);char text[80]="Ready";mt::PeerStatus value;assert(value.assign(text));const auto before=*f->service.nodes.get(0);const bool dirty=f->service.nodes.dirty();assert(f->service.nodes.observeStatus(77,value)&&!f->service.nodes.observeStatus(78,value));assert(!memcmp(before.bytes,f->service.nodes.get(0)->bytes,sizeof(before.bytes))&&f->service.nodes.dirty()==dirty);assert(f->service.nodes.edit(77,Node,mt::NodeCache::Edit::Ignore)==1&&!f->service.nodes.observeStatus(77,value));assert(f->service.nodes.edit(77,Node,mt::NodeCache::Edit::Unignore)==1);char empty[80]{};assert(value.assign(empty)&&f->service.nodes.observeStatus(77,value)&&f->service.nodes.get(0)->status.present()&&!f->service.nodes.get(0)->status.text()[0]);assert(f->service.nodes.edit(77,Node,mt::NodeCache::Edit::Remove)==1);assert(f->service.nodes.accept(entry,Node,0,0)>=0&&!f->service.nodes.get(0)->status.present());group("status changes only admitted peer RAM, respects ignore, stores empty clear and disappears on removal");}
 {auto f=std::make_unique<Fixture>();mt::NodeCache::Entry entry{};entry.identity(77,81,0);assert(f->service.nodes.accept(entry,Node,-2,1)>=0);char text[80]="Cached status";mt::PeerStatus value;assert(value.assign(text)&&f->service.nodes.observeStatus(77,value)&&f->service.nodes.observeLastHeard(77,1791000000));for(bool ble:{false,true}){meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=987;auto&h=ble?f->ble:f->usb;h.out.clear();f->input(r,ble);(ble?f->b:f->c).poll(mt::Companion::WorkspaceProof{true});unsigned completed=0,replayed=0;for(const auto&m:h.out){if(m.which_payload_variant==meshtastic_FromRadio_config_complete_id_tag)++completed;if(m.which_payload_variant==meshtastic_FromRadio_packet_tag&&m.packet.decoded.portnum==meshtastic_PortNum_NODE_STATUS_APP){assert(completed==1&&m.packet.from==77&&m.packet.to==UINT32_MAX&&m.packet.id&&!m.packet.pki_encrypted&&!m.packet.public_key.size&&!m.packet.xeddsa_signed&&m.packet.has_rx_time&&m.packet.rx_time==1791000000);mt::PeerStatus decoded;assert(mt::StatusMessagePayload::decode(m.packet.decoded.payload.bytes,m.packet.decoded.payload.size,decoded)&&!strcmp(decoded.text(),text));++replayed;}}assert(completed==1&&replayed==1);}char empty[80]{};assert(value.assign(empty)&&f->service.nodes.observeStatus(77,value));f->usb.out.clear();meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=988;f->input(r);f->c.poll(mt::Companion::WorkspaceProof{true});for(const auto&m:f->usb.out)assert(m.which_payload_variant!=meshtastic_FromRadio_packet_tag||m.packet.decoded.portnum!=meshtastic_PortNum_NODE_STATUS_APP);assert(!f->radio.sends&&!f->backend.writes);group("stock-compatible status replay follows completion on both transports, skips empty and creates no RF or persistence");}
 {mt::NeighborInfoPayload decoder;meshtastic_NeighborInfo out{};out.node_id=91;assert(decoder.decode(nullptr,0,out)&&!out.node_id);meshtastic_NeighborInfo value{};value.node_id=77;value.neighbors_count=1;value.neighbors[0].node_id=88;value.neighbors[0].snr=-2.5f;uint8_t bytes[258]{};auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&stream,meshtastic_NeighborInfo_fields,&value)&&decoder.decode(bytes,stream.bytes_written,out)&&out.node_id==77&&out.neighbors[0].snr==-2.5f);const auto before=out;const uint8_t bad[]={0x28,1};assert(!decoder.decode(bad,sizeof(bad),out)&&!memcmp(&out,&before,sizeof(out)));value.neighbors[0].snr=std::numeric_limits<float>::infinity();stream=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&stream,meshtastic_NeighborInfo_fields,&value)&&!decoder.decode(bytes,stream.bytes_written,out)&&!memcmp(&out,&before,sizeof(out)));group("canonical NeighborInfo decoder preserves output on unknown fields and nonfinite SNR");}
 {for(bool ble:{false,true}){auto f=std::make_unique<Fixture>();mt::ModuleController controller;auto &h=ble?f->ble:f->usb;h.observations=&controller;h.uptime=2000;meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;r.packet.to=Node;r.packet.id=81;r.packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;r.packet.decoded.portnum=meshtastic_PortNum_NEIGHBORINFO_APP;r.packet.decoded.want_response=true;f->input(r,ble);assert(h.error()==meshtastic_Routing_Error_NO_RESPONSE);meshtastic_ModuleConfig config{};config.which_payload_variant=meshtastic_ModuleConfig_neighbor_info_tag;config.payload_variant.neighbor_info.enabled=true;assert(f->moduleEdit.save(config,1,false)==0);const auto writes=f->backend.writes;h.out.clear();f->input(r,ble);assert(h.out.size()==2&&h.out[0].which_payload_variant==meshtastic_FromRadio_queueStatus_tag&&h.out[0].queueStatus.mesh_packet_id==81&&h.out.back().packet.from==Node&&h.out.back().packet.to==Node&&h.out.back().packet.decoded.request_id==81);meshtastic_NeighborInfo info{};assert(mt::NeighborInfoPayload::decode(h.out.back().packet.decoded.payload.bytes,h.out.back().packet.decoded.payload.size,info)&&info.node_id==Node&&!info.last_sent_by_id&&!info.neighbors_count);controller.poll(f->service,f->moduleEdit.config(),true,2000,1,[](const uint8_t*,size_t,uint32_t){});controller.observe(77,Node,0,2001,true,-2.5f);h.uptime=2002;h.out.clear();f->input(r,ble);assert(h.out.size()==2&&mt::NeighborInfoPayload::decode(h.out.back().packet.decoded.payload.bytes,h.out.back().packet.decoded.payload.size,info)&&info.neighbors_count==1&&info.neighbors[0].node_id==77&&info.neighbors[0].snr==-2.5f);assert(!f->radio.sends&&f->backend.writes==writes&&!h.out.back().packet.pki_encrypted&&!h.out.back().packet.public_key.size);}group("local neighbor reads distinguish disabled and valid empty snapshots, return fresh real observations on requesting transport with zero RF");}

 {auto f=std::make_unique<Fixture>();f->provision();assert(f->service.start(false,0));
  for(uint32_t id:{77u,89u,90u}){mt::NodeCache::Entry entry{};entry.identity(id,81,0);assert(f->service.nodes.accept(entry,Node,0,0)>=0);}
  mt::ModuleController controller;auto settings=controllerSettings(false,true);controller.poll(f->service,settings,true,0,1,[](const uint8_t*,size_t,uint32_t){});
  f->usb.observations=&controller;f->usb.service=&f->service;f->usb.uptime=100;
  auto receive=[&](uint32_t source,uint32_t id,meshtastic_NeighborInfo info,bool viaMqtt,bool radioReceive,bool appendUnknown,bool duplicateLastSent=false){
   info.node_id=source;uint8_t body[233]{},data[239]{},frame[255]{};size_t bodySize=0,dataSize=0,frameSize=0;
   auto out=pb_ostream_from_buffer(body,sizeof(body));assert(pb_encode(&out,meshtastic_NeighborInfo_fields,&info));bodySize=out.bytes_written;
   if(appendUnknown){assert(bodySize+3<=sizeof(body));body[bodySize++]=0x98;body[bodySize++]=0x06;body[bodySize++]=0x01;}
   if(duplicateLastSent){assert(bodySize+2<=sizeof(body));body[bodySize++]=0x10;body[bodySize++]=90;}
   assert(mt::PublicApplication::encode(meshtastic_PortNum_NEIGHBORINFO_APP,body,bodySize,0,0,data,sizeof(data),dataSize));
   assert(f->service.codec.encodeZeroHopData(source,Node,id,data,dataSize,frame,sizeof(frame),frameSize));
   if(viaMqtt)frame[12]|=0x10;
   f->c.receive(frame,frameSize,-90,-2,100,1800,true,0,radioReceive,mt::Companion::WorkspaceProof{true});
  };
  meshtastic_NeighborInfo claimed{};claimed.last_sent_by_id=88;claimed.node_broadcast_interval_secs=21600;
  claimed.neighbors_count=1;claimed.neighbors[0].node_id=99;claimed.neighbors[0].snr=4.0f;
  receive(77,9001,claimed,false,true,true);assert(f->usb.neighborInfoCallbacks==1);
  meshtastic_NeighborInfo snapshot{};assert(controller.localNeighbors(f->service,settings,true,100,snapshot)==0&&snapshot.neighbors_count==1&&snapshot.neighbors[0].node_id==77);
  receive(77,9005,claimed,false,true,false,true);assert(f->usb.neighborInfoCallbacks==2);
  assert(controller.localNeighbors(f->service,settings,true,100,snapshot)==0&&snapshot.neighbors_count==1&&snapshot.neighbors[0].node_id==77);
  meshtastic_NeighborInfo dummy{};dummy.last_sent_by_id=89;dummy.neighbors_count=1;dummy.neighbors[0].node_id=0;
  receive(89,9002,dummy,false,true,false);assert(f->usb.neighborInfoCallbacks==3);
  assert(controller.localNeighbors(f->service,settings,true,100,snapshot)==0&&snapshot.neighbors_count==1&&snapshot.neighbors[0].node_id==77);
  meshtastic_NeighborInfo mqtt{};mqtt.last_sent_by_id=91;receive(77,9003,mqtt,true,true,false);
  receive(77,9004,mqtt,false,false,false);assert(f->usb.neighborInfoCallbacks==3);
  assert(controller.localNeighbors(f->service,settings,true,100,snapshot)==0&&snapshot.neighbors_count==1&&snapshot.neighbors[0].node_id==77);
  meshtastic_NeighborInfo nonfinite{};nonfinite.last_sent_by_id=92;nonfinite.neighbors_count=1;
  nonfinite.neighbors[0].node_id=93;nonfinite.neighbors[0].snr=std::numeric_limits<float>::quiet_NaN();
  receive(77,9006,nonfinite,false,true,false);assert(f->usb.neighborInfoCallbacks==4);
  assert(controller.localNeighbors(f->service,settings,true,100,snapshot)==0&&snapshot.neighbors_count==1&&
         snapshot.neighbors[0].node_id==77);
  assert(!f->radio.sends&&f->backend.writes==1);
  assert(controller.localNeighbors(f->service,settings,true,43200000,snapshot)==0&&snapshot.neighbors_count==1);
  assert(controller.localNeighbors(f->service,settings,true,43201000,snapshot)==0&&!snapshot.neighbors_count);
  group("actual Companion attributes direct NeighborInfo to the header source, including unused nonfinite listed SNR; dummy, MQTT and nonradio cannot create edges");}
 {for(unsigned mode=0;mode<6;++mode){auto f=std::make_unique<Fixture>();meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto&p=r.packet;p.to=Node;p.id=81;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_NEIGHBORINFO_APP;p.decoded.want_response=true;switch(mode){case 0:p.from=77;break;case 1:p.decoded.want_response=false;break;case 2:p.decoded.payload.size=1;p.decoded.payload.bytes[0]=0;break;case 3:p.decoded.request_id=1;break;case 4:p.hop_start=1;break;case 5:p.decoded.has_bitfield=true;break;}f->input(r);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&!f->radio.sends&&!f->backend.writes);}group("local neighbor request rejects forged source and malformed nonempty correlated or routed selectors without RF");}

 {auto f=std::make_unique<Fixture>();f->provision();assert(f->service.start(false,0));mt::NodeCache::Entry entry{};entry.identity(77,81,0);assert(f->service.nodes.accept(entry,Node,0,0)>=0);for(bool ble:{false,true}){meshtastic_ToRadio h{};h.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;h.want_config_id=33;f->input(h,ble);}f->usb.out.clear();f->ble.out.clear();const auto writes=f->backend.writes;auto packet=[&](uint32_t source,uint32_t id,const uint8_t*body,size_t length){uint8_t data[239]{},frame[255]{};size_t size=0,n=0;assert(mt::PublicApplication::encode(36,body,length,0,0,data,sizeof(data),size)&&f->service.codec.encodeZeroHopData(source,Node,id,data,size,frame,sizeof(frame),n));f->service.rx(frame,n,-2,1000,0);f->c.receive(frame,n,-90,-2,1000,1791000000,mt::Companion::WorkspaceProof{true});f->b.receive(frame,n,-90,-2,1000,1791000000,mt::Companion::WorkspaceProof{true});};const uint8_t valid[]={0x0a,5,'R','e','a','d','y'};packet(77,9001,valid,sizeof(valid));assert(f->service.nodes.get(0)->status.present()&&!strcmp(f->service.nodes.get(0)->status.text(),"Ready")&&f->usb.out.size()==1&&f->ble.out.size()==1);packet(77,9001,valid,sizeof(valid));assert(f->usb.out.size()==1&&f->ble.out.size()==1);const uint8_t bad[]={0x10,1};packet(77,9002,bad,sizeof(bad));assert(!strcmp(f->service.nodes.get(0)->status.text(),"Ready")&&f->usb.out.size()==2&&f->ble.out.size()==2);packet(77,9003,nullptr,0);assert(f->service.nodes.get(0)->status.present()&&!f->service.nodes.get(0)->status.text()[0]&&f->usb.out.size()==3&&f->ble.out.size()==3);const auto count=f->service.nodes.count();packet(78,9004,valid,sizeof(valid));assert(f->service.nodes.count()==count&&f->service.nodes.find(78)<0);assert(f->service.nodes.edit(77,Node,mt::NodeCache::Edit::Ignore)==1);const auto usb=f->usb.out.size(),ble=f->ble.out.size();packet(77,9005,valid,sizeof(valid));assert(f->usb.out.size()==usb&&f->ble.out.size()==ble&&!f->service.nodes.get(0)->status.text()[0]&&!f->radio.sends&&f->backend.writes==writes);group("real receive path caches canonical status once, forwards opaque malformed payload without caching, honors empty clear and ignores, never invents identities or writes storage");}
 {auto f=std::make_unique<Fixture>();char text[80]="Deferred";mt::PeerStatus value;assert(value.assign(text));for(unsigned i=0;i<mt::NodeCache::Capacity;++i){mt::NodeCache::Entry entry{};entry.identity(100+i,81,0);assert(f->service.nodes.accept(entry,Node,0,0)>=0&&f->service.nodes.observeStatus(100+i,value));}f->ble.capacity=64;meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=991;f->input(r,true);unsigned completed=0;for(const auto&m:f->ble.out)completed+=m.which_payload_variant==meshtastic_FromRadio_config_complete_id_tag;assert(completed==1&&f->ble.out.size()<64&&!f->b.outputDrops);for(unsigned i=0;i<32;++i)f->b.poll(mt::Companion::WorkspaceProof{true});assert(f->ble.out.size()==64&&!f->b.outputDrops);std::map<uint32_t,unsigned> seen;auto drain=[&](){for(const auto&m:f->ble.out)if(m.which_payload_variant==meshtastic_FromRadio_packet_tag&&m.packet.decoded.portnum==meshtastic_PortNum_NODE_STATUS_APP){assert(m.packet.from>=100&&m.packet.from<116);++seen[m.packet.from];}f->ble.out.clear();};drain();for(unsigned i=0;i<32;++i)f->b.poll(mt::Companion::WorkspaceProof{true});drain();assert(seen.size()==16);for(const auto&row:seen)assert(row.second==1);assert(!f->b.outputDrops&&!f->radio.sends&&!f->backend.writes);f->input(r,true);f->b.resetStream();f->ble.out.clear();for(unsigned i=0;i<32;++i)f->b.poll(mt::Companion::WorkspaceProof{true});assert(f->ble.out.empty());f->ble.rejectComplete=true;f->input(r,true);f->ble.out.clear();for(unsigned i=0;i<32;++i)f->b.poll(mt::Companion::WorkspaceProof{true});assert(f->ble.out.empty()&&!f->b.outputDrops);group("BLE 64-slot backpressure retains all sixteen status replays exactly once; disconnect or rejected completion cancels replay");}
 {auto baseline=std::make_unique<Fixture>();mt::NodeCache::Entry a{},b{};a.identity(701,81,0);b.identity(702,81,0);assert(baseline->service.nodes.accept(a,Node,0,0)>=0&&baseline->service.nodes.accept(b,Node,0,0)>=0);meshtastic_ToRadio h{};h.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;h.want_config_id=991;baseline->input(h,true);const auto expected=canonicalStream(baseline->ble.out);assert(!expected.empty()&&baseline->ble.out.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag);
  auto throttled=std::make_unique<Fixture>();a={};b={};a.identity(701,81,0);b.identity(702,81,0);assert(throttled->service.nodes.accept(a,Node,0,0)>=0&&throttled->service.nodes.accept(b,Node,0,0)>=0);throttled->ble.capacity=3;throttled->input(h,true);assert(throttled->ble.out.size()==3);std::vector<meshtastic_FromRadio> delivered;
  for(unsigned cycle=0;cycle<64;++cycle){delivered.insert(delivered.end(),throttled->ble.out.begin(),throttled->ble.out.end());throttled->ble.out.clear();throttled->b.poll(mt::Companion::WorkspaceProof{true});if(throttled->ble.out.size()<3&&throttled->ble.out.size()&&throttled->ble.out.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag){delivered.insert(delivered.end(),throttled->ble.out.begin(),throttled->ble.out.end());throttled->ble.out.clear();break;}}
  assert(canonicalStream(delivered)==expected&&!throttled->b.outputDrops);group("BLE handshake resumes across repeated queue-full polls with byte-identical ordered configuration and one completion");}
 {auto baseline=std::make_unique<Fixture>();meshtastic_ToRadio h{};h.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;h.want_config_id=333;baseline->input(h,true);const auto expected=canonicalStream(baseline->ble.out);
  auto f=std::make_unique<Fixture>();f->ble.capacity=3;h.want_config_id=111;f->input(h,true);assert(f->ble.out.size()==3);const auto oldPrefix=canonicalStream(f->ble.out);h.want_config_id=333;f->input(h,true);assert(f->ble.out.size()==3);std::vector<meshtastic_FromRadio> delivered=f->ble.out;f->ble.out.clear();
  for(unsigned cycle=0;cycle<64;++cycle){f->b.poll(mt::Companion::WorkspaceProof{true});delivered.insert(delivered.end(),f->ble.out.begin(),f->ble.out.end());f->ble.out.clear();if(!delivered.empty()&&delivered.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag)break;}
  unsigned complete=0;for(const auto&m:delivered)if(m.which_payload_variant==meshtastic_FromRadio_config_complete_id_tag){++complete;assert(m.config_complete_id==333);}assert(oldPrefix.size()==3&&delivered.size()==oldPrefix.size()+expected.size()&&canonicalStream(std::vector<meshtastic_FromRadio>(delivered.begin()+oldPrefix.size(),delivered.end()))==expected&&complete==1&&!f->b.outputDrops);
  auto reset=std::make_unique<Fixture>();reset->ble.capacity=3;h.want_config_id=444;reset->input(h,true);assert(reset->ble.out.size()==3);reset->b.resetStream();reset->ble.out.clear();h.want_config_id=555;reset->input(h,true);std::vector<meshtastic_FromRadio> fresh;for(unsigned cycle=0;cycle<64;++cycle){fresh.insert(fresh.end(),reset->ble.out.begin(),reset->ble.out.end());reset->ble.out.clear();reset->b.poll(mt::Companion::WorkspaceProof{true});if(!fresh.empty()&&fresh.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag){fresh.insert(fresh.end(),reset->ble.out.begin(),reset->ble.out.end());reset->ble.out.clear();break;}}complete=0;for(const auto&m:fresh)if(m.which_payload_variant==meshtastic_FromRadio_config_complete_id_tag){++complete;assert(m.config_complete_id==555);}assert(complete==1);group("new want_config replaces the pending tail and stream reset prevents stale completion");}
 controllerRuntimeCases();
 environmentControllerCases();
 powerControllerCases();
 std::cout<<"MODULE_CONFIG_GROUPS 46\n";


}
