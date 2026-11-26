// SPDX-License-Identifier: GPL-3.0-only
// Actual Companion/Service/Endpoint and wire codec; synthetic sensors/radio.
#include "telemetry_query_fixture.h"
#include <limits>
static zt::telemetry::PowerReadings readings(){
 zt::telemetry::PowerReadings r;r.voltage[0]=0.f;r.current[0]=-125.f;r.voltage[2]=12.5f;return r;
}
static meshtastic_Telemetry telemetry(const uint8_t*p,size_t n){
 meshtastic_Telemetry t{};auto in=pb_istream_from_buffer(p,n);
 assert(pb_decode(&in,meshtastic_Telemetry_fields,&t)&&!in.bytes_left&&t.which_variant==5&&!t.time);
 const auto&m=t.variant.power_metrics;assert(m.has_ch1_voltage&&m.ch1_voltage==0&&m.has_ch1_current&&m.ch1_current==-125&&m.has_ch3_voltage&&m.ch3_voltage==12.5f);
 assert(!m.has_ch2_voltage&&!m.has_ch2_current&&!m.has_ch3_current&&!m.has_ch4_voltage);return t;
}
static meshtastic_ToRadio localRequest(){
 auto r=query(77,OURS,0);r.packet.decoded.payload.size=2;r.packet.decoded.payload.bytes[0]=0x2a;r.packet.decoded.payload.bytes[1]=0;return r;
}
static void feed(Fixture&f,bool ble,const meshtastic_ToRadio&r){
 if(ble){input(f.bc,r,100);return;}
 uint8_t bytes[512];auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&out,meshtastic_ToRadio_fields,&r));
 f.c.byte(0x94,100,0,mt::Companion::WorkspaceProof{true});f.c.byte(0xc3,100,0,mt::Companion::WorkspaceProof{true});f.c.byte(out.bytes_written>>8,100,0,mt::Companion::WorkspaceProof{true});f.c.byte(out.bytes_written,100,0,mt::Companion::WorkspaceProof{true});
 for(size_t i=0;i<out.bytes_written;++i)f.c.byte(bytes[i],100,0,mt::Companion::WorkspaceProof{true});
}
int main(){
 for(uint8_t model:{81,127})for(bool ble:{false,true}){
  auto f=std::make_unique<Fixture>(false,model);f->s.stop();f->s.measuredPower(readings());
  auto&h=ble?f->bh:f->h;auto&other=ble?f->h:f->bh;const auto before=f->s.config;auto request=localRequest();
  feed(*f,ble,request);assert(results(h,77,meshtastic_Routing_Error_NO_INTERFACE)==1&&!telemetries(h));h.out.clear();
  h.powerStatus=-ENODATA;feed(*f,ble,request);assert(results(h,77,meshtastic_Routing_Error_NO_RESPONSE)==1&&!telemetries(h));h.out.clear();
  h.powerStatus=0;assert(!h.uptimeValid);feed(*f,ble,request);std::vector<uint8_t> bytes;
  assert(telemetries(h,77,&bytes)==1&&!results(h,77));(void)telemetry(bytes.data(),bytes.size());
  assert(other.out.empty()&&!f->radio.sends&&!f->s.endpointArmed()&&f->s.config.same(before));h.out.clear();
  for(unsigned mode=0;mode<5;++mode){auto bad=request;
   if(mode==0)bad.packet.id=0;if(mode==1)bad.packet.decoded.request_id=1;if(mode==2)bad.packet.decoded.has_bitfield=true;
   if(mode==3)bad.packet.decoded.payload.bytes[1]=1;if(mode==4)bad.packet.decoded.want_response=false;
   feed(*f,ble,bad);assert(!telemetries(h,bad.packet.id)&&results(h,bad.packet.id)==1&&!f->radio.sends);h.out.clear();
  }
 }
 group("measured PowerMetrics on both models and USB/BLE: disabled/absent, independent of uptime, exact correlation and no RF");
 for(uint8_t model:{81,127})for(bool ack:{false,true}){
  Fixture f(true,model);f.s.measuredPower(readings());const auto before=f.s.config;
  const auto request=rawFrame({8,67,18,2,0x2a,0,24,1,72,2},PEER,OURS,100,ack?8:0);
  f.rx(request,100,700);f.s.poll(400,0);assert(f.radio.sends==1);
  const auto&wire=f.radio.frames[0];bench::RouteMetadata m{};assert(f.s.codec.decode(wire.data(),wire.size(),m));
  assert(m.request_id==100&&m.portnum==67&&!m.wantResponse&&bool(wire[12]&8)==ack);(void)telemetry(m.payload,m.payloadLength);
  f.done(401);f.rx(request,600,701);f.s.poll(1000,0);assert(f.radio.sends==1&&f.s.config.same(before));
 }
 group("public power responder: both models, measured zero, canonical selector, ACK policy and replay suppression");
 for(unsigned mode=0;mode<7;++mode){Fixture f;f.s.measuredPower(readings());auto bytes=std::vector<uint8_t>{8,67,18,2,0x2a,0,24,1,72,2};
  if(mode==0)f.s.measuredPower({});if(mode==1)f.s.armEndpoint(false);
  if(mode==2)bytes.insert(bytes.end(),{8,67});if(mode==3)bytes[4]=0x22;if(mode==4)bytes.back()=0;
  auto frame=rawFrame(bytes,PEER,mode==5?UINT32_MAX:OURS,100);if(mode==6)frame[13]^=1;
  f.rx(frame,100,700);f.s.poll(1000,0);assert(!f.radio.sends);
 }
 group("unavailable/disabled and noncanonical/wrong-channel/broadcast power requests cannot transmit");
 {Fixture f;mt::Endpoint ep;ep.measuredPower(readings());uint8_t data[128];size_t size=0;
  assert(ep.privateTelemetryReply(PEER,100,100,data,sizeof(data),size,false,false,true));
  meshtastic_Data d{};auto in=pb_istream_from_buffer(data,size);assert(pb_decode(&in,meshtastic_Data_fields,&d)&&d.request_id==100);(void)telemetry(d.payload.bytes,d.payload.size);
  assert(!ep.privateTelemetryReply(PEER,101,101,data,sizeof(data),size,false,false,true)&&!size);
  assert(ep.privateTelemetryReply(PEER,102,5100,data,sizeof(data),size,false,false,true));
  ep.measuredPower({});assert(!ep.privateTelemetryReply(PEER,103,10100,data,sizeof(data),size,false,false,true));
  assert(!f.radio.sends);
 }
 group("actual private endpoint producer uses measured cache and shared five-second per-peer rate records");
 {Fixture f;beginQuery(f);f.rx(goodFrame());f.clients();assert(telemetries(f.h)==1);}
 group("existing remote DeviceMetrics query still completes through the actual service");
}
