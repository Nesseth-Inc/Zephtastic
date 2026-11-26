// SPDX-License-Identifier: GPL-3.0-only
// Actual companion Admin/config projection, synthetic local host, no RF.
#include "Companion.h"
#include <cassert>
#include <cstdio>
#include <vector>
static constexpr uint32_t Self=0x12345678,Request=0x77123456;
struct Radio final:mt::Radio {
 unsigned sends=0;
 bool ready()override{return true;}int start()override{return 0;}
 int stop()override{return 0;}int restore()override{return 0;}int health()override{return 0;}
 bool busy()override{return false;}uint32_t airtime(size_t)override{return 100;}
 int send(uint8_t*,size_t)override{++sends;return 0;}bool completion(int&)override{return false;}
};
struct Host:mt::Companion::Host {
 meshtastic_Config_BluetoothConfig desired{true,meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN,0};
 unsigned writes=0;int outcome=0;std::vector<meshtastic_FromRadio> output;
 bool readyFlag=true,editingFlag=false;
 bool ready()const override{return readyFlag;}int save(const mt::Config&)override{return -ENOTSUP;}
 bool editing()const override{return editingFlag;}
 bool bluetoothConfig(meshtastic_Config_BluetoothConfig&out)const override{out=desired;return true;}
 int saveBluetoothConfig(const meshtastic_Config_BluetoothConfig&c,uint32_t)override{
  ++writes;if(!outcome)desired=c;return outcome;
 }
 bool send(const uint8_t*p,size_t n)override{
  assert(n>=4&&p[0]==0x94&&p[1]==0xc3);meshtastic_FromRadio r{};
  auto stream=pb_istream_from_buffer(p+4,n-4);assert(pb_decode(&stream,meshtastic_FromRadio_fields,&r));output.push_back(r);return true;
 }
 bool result(meshtastic_Routing_Error code,uint32_t request=Request)const{
  for(const auto&r:output)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&
   r.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP&&r.packet.decoded.request_id==request){
   meshtastic_Routing route{};auto s=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);
   assert(pb_decode(&s,meshtastic_Routing_fields,&route));if(route.error_reason==code)return true;
  }
  return false;
 }
};
struct DeferredHost final:Host {
 unsigned requests=0;uint32_t capturedNow=0,packetId=0;bool wantAck=false,wantResponse=false;
 bool becomeUnavailable=false;int requestOutcome=BluetoothDeferred;
 meshtastic_Config_BluetoothConfig captured{};
 int saveBluetoothConfigRequest(const meshtastic_Config_BluetoothConfig&c,uint32_t now,
                                uint32_t id,bool ack,bool response)override{
  ++requests;captured=c;capturedNow=now;packetId=id;wantAck=ack;wantResponse=response;
  if(becomeUnavailable)readyFlag=false;
  return requestOutcome;
 }
};
template<class H=Host>struct TestFixture {
 Radio radio;mt::Service service;H host;mt::companion::CompanionWorkspace workspace;mt::Companion companion;
 explicit TestFixture(uint8_t model=127):service(radio,true,Self,model,true),companion(service,host,model,workspace){
  service.route.configureIdentity(Self);meshtastic_ToRadio t{};t.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;t.want_config_id=123;input(t);host.output.clear();
 }
 void input(const meshtastic_ToRadio&t){uint8_t b[512];auto stream=pb_ostream_from_buffer(b,sizeof(b));assert(pb_encode(&stream,meshtastic_ToRadio_fields,&t));companion.byte(0x94,100,0,mt::Companion::WorkspaceProof{true});companion.byte(0xc3,100,0,mt::Companion::WorkspaceProof{true});companion.byte(stream.bytes_written>>8,100,0,mt::Companion::WorkspaceProof{true});companion.byte(stream.bytes_written,100,0,mt::Companion::WorkspaceProof{true});for(size_t i=0;i<stream.bytes_written;++i)companion.byte(b[i],100,0,mt::Companion::WorkspaceProof{true});}
 void admin(const meshtastic_AdminMessage&a,uint32_t id=Request,bool ack=true,bool response=true,bool unknown=false){
  host.output.clear();meshtastic_ToRadio t{};t.which_payload_variant=meshtastic_ToRadio_packet_tag;auto&p=t.packet;
  p.to=Self;p.from=Self;p.id=id;p.want_ack=ack;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=response;
  auto stream=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));assert(pb_encode(&stream,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=stream.bytes_written;
  if(unknown){auto&payload=p.decoded.payload;assert(payload.size+3<=sizeof(payload.bytes));payload.bytes[payload.size++]=0xc0;payload.bytes[payload.size++]=0x3e;payload.bytes[payload.size++]=1;}
  input(t);
 }
 void set(unsigned mode,uint32_t pin,bool enabled=true,uint32_t id=Request,bool ack=true,bool response=true,bool unknown=false){meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;a.set_config.which_payload_variant=meshtastic_Config_bluetooth_tag;auto&b=a.set_config.payload_variant.bluetooth;b.enabled=enabled;b.mode=static_cast<meshtastic_Config_BluetoothConfig_PairingMode>(mode);b.fixed_pin=pin;admin(a,id,ack,response,unknown);}
 void get(){
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;a.get_config_request=meshtastic_AdminMessage_ConfigType_BLUETOOTH_CONFIG;admin(a);bool found=false;
  for(const auto&r:host.output)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.decoded.portnum==meshtastic_PortNum_ADMIN_APP){
   meshtastic_AdminMessage reply{};auto s=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);assert(pb_decode(&s,meshtastic_AdminMessage_fields,&reply));
   if(reply.which_payload_variant==meshtastic_AdminMessage_get_config_response_tag){const auto&b=reply.get_config_response.payload_variant.bluetooth;assert(b.enabled==host.desired.enabled&&b.mode==host.desired.mode&&b.fixed_pin==host.desired.fixed_pin);found=true;}
  }
  assert(found);
 }
};
using Fixture=TestFixture<>;
using DeferredFixture=TestFixture<DeferredHost>;
static void oneRouting(const Host&host,uint32_t request,meshtastic_Routing_Error code){
 assert(host.output.size()==1);const auto&r=host.output[0];
 assert(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP&&r.packet.decoded.request_id==request);
 meshtastic_Routing route{};auto stream=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);
 assert(pb_decode(&stream,meshtastic_Routing_fields,&route)&&!stream.bytes_left);
 assert(route.which_variant==meshtastic_Routing_error_reason_tag&&route.error_reason==code);
}
int main(){
 for(unsigned mode:{0u,1u,2u})for(uint32_t pin:{0u,999999u}){
  Fixture f;f.set(mode,pin);assert(f.host.result(meshtastic_Routing_Error_NONE)&&f.host.writes==1);f.get();assert(!f.radio.sends);
 }
 puts("PASS Bluetooth Admin all mode/PIN endpoint roundtrips preserve desired config and perform no RF");
 Fixture bad;bad.set(1,1000000);assert(bad.host.result(meshtastic_Routing_Error_BAD_REQUEST)&&!bad.host.writes);
 bad.set(3,0);assert(bad.host.result(meshtastic_Routing_Error_BAD_REQUEST)&&!bad.host.writes);
 Fixture headless(81);headless.set(0,123456);assert(headless.host.result(meshtastic_Routing_Error_BAD_REQUEST)&&!headless.host.writes);
 headless.set(1,0);assert(headless.host.result(meshtastic_Routing_Error_NONE));
 puts("PASS Bluetooth malformed fields and headless random PIN reject without storage delegation");
 Fixture busy;busy.host.outcome=-EBUSY;busy.set(1,5);assert(busy.host.result(meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED));
 busy.host.outcome=-EIO;busy.set(1,5);assert(busy.host.result(meshtastic_Routing_Error_NO_INTERFACE));
 assert(busy.host.desired.mode==meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN&&!busy.radio.sends);
 puts("PASS Bluetooth host failure mapping never projects uncommitted candidate");
 for(uint32_t id:{Request,0u})for(bool ack:{false,true})for(bool response:{false,true}){
  DeferredFixture f;f.host.becomeUnavailable=true;f.set(1,0,false,id,ack,response);
  assert(f.host.requests==1&&f.host.capturedNow==100&&f.host.packetId==id&&f.host.wantAck==ack&&f.host.wantResponse==response);
  assert(!f.host.captured.enabled&&f.host.captured.mode==meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN&&f.host.captured.fixed_pin==0);
  assert(!f.host.writes&&f.host.desired.enabled&&f.host.desired.mode==meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN&&f.host.desired.fixed_pin==0);
  // Admission may queue QueueStatus, but produces no Routing result, Admin
  // response or notification, even if the host closes its ready gate.
  assert(f.host.output.size()==1&&f.host.output[0].which_payload_variant==meshtastic_FromRadio_queueStatus_tag);
  assert(!f.radio.sends);
 }
 puts("PASS Bluetooth deferred admission forwards original zero/nonzero IDs and all flags without completion or mutation");
 for(uint32_t id:{Request,0u})for(bool ack:{false,true})for(bool response:{false,true})for(int result:{0,-EBUSY,-EIO,-EINVAL,-EACCES,-ENOTSUP}){
  DeferredFixture f;f.set(1,999999,true,id,ack,response);assert(f.host.requests==1&&!f.host.writes);
  f.host.output.clear();f.host.readyFlag=false;
  f.companion.completeBluetoothConfig(id,result,mt::Companion::WorkspaceProof{true});
  const auto expected=!result?meshtastic_Routing_Error_NONE:result==-EBUSY?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:result==-EIO?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_BAD_REQUEST;
  oneRouting(f.host,id,expected);
  assert(f.host.requests==1&&!f.host.writes&&f.host.desired.mode==meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN&&!f.radio.sends);
 }
 puts("PASS Bluetooth deferred completion emits exactly one mapped Routing result without persistence or extra output");
 DeferredFixture invalid;
 invalid.set(3,0);assert(invalid.host.result(meshtastic_Routing_Error_BAD_REQUEST)&&!invalid.host.requests);
 invalid.set(1,1000000);assert(invalid.host.result(meshtastic_Routing_Error_BAD_REQUEST)&&!invalid.host.requests);
 invalid.set(1,1,true,Request,true,true,true);assert(invalid.host.result(meshtastic_Routing_Error_BAD_REQUEST)&&!invalid.host.requests);
 invalid.host.editingFlag=true;invalid.set(1,1);assert(invalid.host.result(meshtastic_Routing_Error_BAD_REQUEST)&&!invalid.host.requests);
 DeferredFixture deferredHeadless(81);deferredHeadless.set(0,0);assert(deferredHeadless.host.result(meshtastic_Routing_Error_BAD_REQUEST)&&!deferredHeadless.host.requests);
 deferredHeadless.set(1,0);assert(deferredHeadless.host.requests==1&&!deferredHeadless.host.writes);
 assert(!invalid.host.writes&&invalid.host.desired.mode==meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN&&!invalid.radio.sends&&!deferredHeadless.radio.sends);
 for(uint32_t id:{Request,0u})for(bool ack:{false,true})for(bool response:{false,true}){
  Fixture f;f.set(2,123456,true,id,ack,response);assert(f.host.writes==1&&f.host.desired.fixed_pin==123456);
  unsigned routing=0;for(const auto&r:f.host.output)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag)++routing;
  assert(routing==1&&f.host.result(meshtastic_Routing_Error_NONE,id)&&!f.radio.sends);
 }
 puts("PASS Bluetooth canonical and headless gates precede deferred Host while synchronous SET keeps one Routing result regardless flags");
}
