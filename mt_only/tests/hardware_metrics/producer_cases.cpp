// SPDX-License-Identifier: GPL-3.0-only
// Actual encoders, Service/Endpoint, Companion and ModuleController; no text matching.
#include "telemetry_query_fixture.h"
#include "ModuleController.h"
#include "ui/Model.h"
#include <cmath>
#include <string>
struct Case {const char *name;mt::HardwareSnapshot hardware;bool voltage;float volts;bool level;uint32_t percent;};
static mt::HardwareSnapshot sample(int mv=-1,int pct=-1,int usb=-1){mt::HardwareSnapshot h;if(mv>=0)h.cellMilliVolts=uint16_t(mv);if(pct>=0)h.cellPercent=uint8_t(pct);if(usb>=0)h.externalPower=usb!=0;h.capturedMs=42;return h;}
static const std::vector<Case> cases={
 {"unavailable",sample(),false,0,false,0},
 {"measured_cell",sample(3895,70,0),true,3.895f,true,70},
 {"usb_without_cell_evidence",sample(-1,-1,1),false,0,true,101},
 {"known_not_usb_without_cell_evidence",sample(-1,-1,0),false,0,false,0},
 {"voltage_without_estimate",sample(3895,-1),true,3.895f,false,0},
 {"percent_without_voltage",sample(-1,70),false,0,false,0},
 {"low_outside_envelope",sample(1999,0),false,0,false,0},
 {"high_outside_envelope",sample(4501,100),false,0,false,0},
 {"lower_envelope_measured_zero_pct",sample(2000,0),true,2.f,true,0},
 {"upper_envelope",sample(4500,100),true,4.5f,true,100},
 {"invalid_percent",sample(3895,101),true,3.895f,false,0},
 {"usb_overrides_estimate_only",sample(3895,70,1),true,3.895f,true,101},
 {"usb_independent_of_rejected_voltage",sample(1999,0,1),false,0,true,101},
 {"absence_after_present",sample(),false,0,false,0}
};
static void expected(const meshtastic_DeviceMetrics &m,const Case &c){
 assert(m.has_voltage==c.voltage&&m.has_battery_level==c.level);
 if(c.voltage)assert(std::fabs(m.voltage-c.volts)<0.000001f);else assert(m.voltage==0);
 if(c.level)assert(m.battery_level==c.percent);else assert(m.battery_level==0);
}
static meshtastic_Telemetry telemetry(const uint8_t *p,size_t n){meshtastic_Telemetry t{};auto in=pb_istream_from_buffer(p,n);assert(pb_decode(&in,meshtastic_Telemetry_fields,&t)&&!in.bytes_left&&t.which_variant==meshtastic_Telemetry_device_metrics_tag);return t;}
static meshtastic_Telemetry dataTelemetry(const uint8_t *p,size_t n,uint32_t request){meshtastic_Data d{};auto in=pb_istream_from_buffer(p,n);assert(pb_decode(&in,meshtastic_Data_fields,&d)&&!in.bytes_left&&d.portnum==meshtastic_PortNum_TELEMETRY_APP&&d.request_id==request&&d.has_bitfield&&!d.bitfield&&!d.want_response);return telemetry(d.payload.bytes,d.payload.size);}
static void directEncoders(){
 for(const auto &c:cases){uint8_t bytes[239]{};size_t n=0;
  assert(mt::TelemetryQuery::uptimeResponse(77,901,zt::Observed<float>(0.125f),zt::Observed<float>(12.5f),bytes,sizeof(bytes),n,c.hardware));
  auto t=dataTelemetry(bytes,n,901);expected(t.variant.device_metrics,c);const auto &m=t.variant.device_metrics;assert(!t.time&&m.has_uptime_seconds&&m.uptime_seconds==77&&m.has_air_util_tx&&m.air_util_tx==0.125f&&m.has_channel_utilization&&m.channel_utilization==12.5f);
  assert(mt::TelemetryAnnouncement::encode(88,zt::Observed<float>(0.f),{},bytes,sizeof(bytes),n,c.hardware));t=dataTelemetry(bytes,n,0);expected(t.variant.device_metrics,c);assert(!t.time&&t.variant.device_metrics.has_uptime_seconds&&t.variant.device_metrics.uptime_seconds==88&&t.variant.device_metrics.has_air_util_tx&&t.variant.device_metrics.air_util_tx==0&&!t.variant.device_metrics.has_channel_utilization);
 }
 group("14 hardware snapshots: actual query/announcement wire encoders preserve presence, voltage units, USB sentinel, uptime/air/channel");
}
static void feed(Fixture &f,bool ble,const meshtastic_ToRadio &q,uint32_t now){
 if(ble){input(f.bc,q,now);return;}uint8_t bytes[512];auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&out,meshtastic_ToRadio_fields,&q));f.c.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});f.c.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});f.c.byte(uint8_t(out.bytes_written>>8),now,0,mt::Companion::WorkspaceProof{true});f.c.byte(uint8_t(out.bytes_written),now,0,mt::Companion::WorkspaceProof{true});for(size_t i=0;i<out.bytes_written;++i)f.c.byte(bytes[i],now,0,mt::Companion::WorkspaceProof{true});
}
static void localAndNodeInfo(){
 for(uint8_t model:{81,127})for(bool ble:{false,true}){auto f=std::make_unique<Fixture>(false,model);f->s.stop();auto &h=ble?f->bh:f->h;auto &other=ble?f->h:f->bh;h.uptimeValid=true;h.uptime=321;uint32_t sequence=100;
  for(const auto &c:cases){const auto before=f->s.config;f->s.measuredHardware(c.hardware);h.out.clear();other.out.clear();auto q=query(++sequence,OURS,0);feed(*f,ble,q,sequence);std::vector<uint8_t> payload;assert(telemetries(h,sequence,&payload)==1&&!results(h,sequence));auto t=telemetry(payload.data(),payload.size());expected(t.variant.device_metrics,c);assert(t.variant.device_metrics.has_uptime_seconds&&t.variant.device_metrics.uptime_seconds==321&&!t.time);
   h.out.clear();meshtastic_ToRadio handshake{};handshake.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;handshake.want_config_id=++sequence;feed(*f,ble,handshake,sequence);
   unsigned local=0;for(const auto &o:h.out)if(o.which_payload_variant==meshtastic_FromRadio_node_info_tag&&o.node_info.num==OURS){++local;assert(o.node_info.has_device_metrics==(c.voltage||c.level));expected(o.node_info.device_metrics,c);assert(!o.node_info.has_position);}
   assert(local==1&&other.out.empty()&&f->s.config.same(before)&&!f->radio.sends&&!f->s.receiving()&&!f->s.endpointArmed());
  }
 }
 group("both board models and USB/BLE: actual local DeviceMetrics and handshake NodeInfo use shared RAM snapshot, clear absent fields, no RF/config writes");
}
static void publicResponder(){
 for(uint8_t model:{81,127})for(const auto &c:cases){auto f=std::make_unique<Fixture>(true,model);f->s.measuredUptime(77);f->s.measuredHardware(c.hardware);const auto before=f->s.config;auto selector=rawFrame({8,67,18,2,18,0,24,1,72,2},PEER,OURS,100,0);f->rx(selector,100,700);f->s.poll(400,0);assert(f->radio.frames.size()==1);const auto &frame=f->radio.frames.front();bench::RouteMetadata m{};assert(f->s.codec.decode(frame.data(),frame.size(),m)&&m.portnum==67&&m.request_id==100&&!m.wantResponse&&m.hasBitfield&&!m.bitfield);const auto t=telemetry(m.payload,m.payloadLength);expected(t.variant.device_metrics,c);assert(t.variant.device_metrics.has_uptime_seconds&&t.variant.device_metrics.uptime_seconds==77&&f->s.config.same(before));f->done(401);f->rx(selector,600,701);f->s.poll(1000,0);assert(f->radio.frames.size()==1);}
 group("both models: real Service public query routes measured hardware to generated Endpoint response with existing request/replay admission");
}
static void privateReplyProducer(){
 for(const auto &c:cases){auto endpoint=std::make_unique<mt::Endpoint>();endpoint->measuredUptime(123);endpoint->measuredHardware(c.hardware);uint8_t bytes[239]{};size_t n=0;assert(endpoint->privateTelemetryReply(PEER,501,500,bytes,sizeof(bytes),n));const auto t=dataTelemetry(bytes,n,501);expected(t.variant.device_metrics,c);assert(t.variant.device_metrics.has_uptime_seconds&&t.variant.device_metrics.uptime_seconds==123);assert(!endpoint->privateTelemetryReply(PEER,501,600,bytes,sizeof(bytes),n));endpoint->measuredHardware({});assert(endpoint->privateTelemetryReply(PEER,502,5500,bytes,sizeof(bytes),n));expected(dataTelemetry(bytes,n,502).variant.device_metrics,cases.front());}
 group("actual Endpoint private reply encoder consumes the same snapshot and clears replacement absence while preserving rate/replay guards; crypto transport excluded");
}
static void scheduledProducer(){
 for(const auto &c:cases){auto f=std::make_unique<Fixture>(false,127);f->s.stop();mt::ModuleController controller;mt::ModuleSettings settings;std::vector<meshtastic_Telemetry> observed;
  auto sink=[&](const uint8_t *p,size_t n,uint32_t id){assert(id);meshtastic_Data d{};auto in=pb_istream_from_buffer(p,n);assert(pb_decode(&in,meshtastic_Data_fields,&d)&&!in.bytes_left&&!d.request_id);if(d.portnum!=meshtastic_PortNum_TELEMETRY_APP)return;meshtastic_Telemetry t{};auto ti=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&ti,meshtastic_Telemetry_fields,&t)&&!ti.bytes_left);if(t.which_variant==meshtastic_Telemetry_device_metrics_tag)observed.push_back(t);};
  controller.poll(f->s,settings,true,0,41,sink);f->s.measuredHardware(c.hardware);controller.poll(f->s,settings,true,59999,42,sink);assert(observed.empty());controller.poll(f->s,settings,true,60000,43,sink);assert(observed.size()==1&&observed[0].variant.device_metrics.uptime_seconds==60);expected(observed[0].variant.device_metrics,c);f->s.measuredHardware({});controller.poll(f->s,settings,true,120000,44,sink);assert(observed.size()==2);expected(observed[1].variant.device_metrics,cases.front());assert(!f->radio.sends&&!f->s.receiving());
 }
 group("actual ModuleController deadlines publish shared hardware at 60 seconds and replacement absence next cycle with default background RF disabled");
}
static void remoteDoesNotPoisonLocal(){
 auto f=std::make_unique<Fixture>();const auto local=sample(3895,70,0);f->s.measuredHardware(local);beginQuery(*f);f->rx(goodFrame());f->clients();assert(telemetries(f->h)==1&&f->radio.sends==1);f->h.out.clear();f->h.uptimeValid=true;f->h.uptime=15;auto q=query(900,OURS,0);feed(*f,false,q,1500);std::vector<uint8_t> payload;assert(telemetries(f->h,900,&payload)==1);expected(telemetry(payload.data(),payload.size()).variant.device_metrics,cases[1]);assert(f->radio.sends==1);group("received peer 4.2V/USB claims cannot overwrite measured local 3.895V/70 percent");
}
static void displayLabels(){
 auto model=std::make_unique<mt::ui::Model>();char text[64];model->powerLabel(text,sizeof(text));assert(!strcmp(text,"VBAT UNAVAILABLE"));
 model->externalPowerKnown=true;model->externalPower=true;model->powerLabel(text,sizeof(text));assert(!strcmp(text,"USB / VBAT N/A"));
 model->externalPower=false;model->powerLabel(text,sizeof(text));assert(!strcmp(text,"VBAT UNAVAILABLE"));
 model->voltageKnown=true;model->cellMilliVolts=3895;model->powerLabel(text,sizeof(text));assert(!strcmp(text,"3.895V"));
 model->percentKnown=true;model->cellPercent=70;model->powerLabel(text,sizeof(text));assert(!strcmp(text,"3.895V ~70%"));
 model->externalPower=true;model->powerLabel(text,sizeof(text));assert(!strcmp(text,"3.895V ~70% USB"));
 model->percentKnown=false;model->powerLabel(text,sizeof(text));assert(!strcmp(text,"3.895V USB"));
 model->percentKnown=true;model->cellMilliVolts=2003;model->cellPercent=0;model->externalPowerKnown=false;model->powerLabel(text,sizeof(text));assert(!strcmp(text,"2.003V ~0%"));
 char guarded[9];memset(guarded,'?',sizeof(guarded));model->powerLabel(guarded+2,4);assert(guarded[0]=='?'&&guarded[1]=='?'&&!strcmp(guarded+2,"2.0")&&guarded[6]=='?'&&guarded[8]=='?');
 memset(guarded,'?',sizeof(guarded));model->powerLabel(guarded+2,1);assert(guarded[2]==0&&guarded[1]=='?'&&guarded[3]=='?');model->powerLabel(guarded+4,0);assert(guarded[4]=='?');
 group("actual UI powerLabel: absence, independent USB, measured volts, estimated percent including0, known flags, bounded buffers");
}
int main(){std::cout.setf(std::ios::unitbuf);directEncoders();localAndNodeInfo();publicResponder();privateReplyProducer();scheduledProducer();remoteDoesNotPoisonLocal();displayLabels();return 0;}
