// SPDX-License-Identifier: GPL-3.0-only
#include "Companion.h"
#include "modules/GpioModuleWire.h"
#include "m138_gpio_sdk_vectors.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <iostream>

namespace {
struct TestRadio final : mt::Radio {
  bool running=false;unsigned sends=0;std::vector<uint8_t> last;
  bool ready()override{return true;}
  int start()override{running=true;return 0;}
  int restore()override{running=true;return 0;}
  int stop()override{running=false;return 0;}
  int health()override{return running?1:0;}
  bool busy()override{return false;}
  uint32_t airtime(size_t)override{return 80;}
  int send(uint8_t *p,size_t n)override{++sends;last.assign(p,p+n);return -EIO;}
  bool completion(int &r)override{r=0;return false;}
};
struct TestHost final : mt::Companion::Host {
  bool storage=true;unsigned previews=0;std::vector<meshtastic_FromRadio> frames;
  bool send(const uint8_t *p,size_t n)override {
    if(!p||n<4||p[0]!=0x94||p[1]!=0xc3||size_t(p[2]*256+p[3])!=n-4)return false;
    meshtastic_FromRadio f{};auto in=pb_istream_from_buffer(p+4,n-4);
    if(!pb_decode(&in,meshtastic_FromRadio_fields,&f)||in.bytes_left)return false;
    frames.push_back(f);return true;
  }
  bool ready()const override{return storage;}
  void preview(uint32_t,uint32_t,uint32_t,unsigned,const uint8_t *,size_t,int16_t,int8_t,uint32_t,bool)override{++previews;}
  int save(const mt::Config &)override{return storage?0:-EIO;}
};
struct TestPrivate final : mt::pki::PrivateRuntime {
  uint32_t local=0x12345678;unsigned channelEncodes=0,privateSubmits=0;uint8_t lastData[239]{};size_t lastDataSize=0;
  uint32_t peer()const override{return 0x33445566;}
  bool ready(uint32_t n)const override{return n==local;}
  bool publicIdentity(uint8_t out[32])const override{memset(out,0,32);return false;}
  bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,uint32_t,uint32_t,bool,uint32_t,bench::Frame &)override{return false;}
  bool encodeChannelControl(bench::MtCodec &codec,uint32_t to,uint32_t id,const uint8_t *data,size_t n,bench::Frame &out)override {
    ++channelEncodes;lastDataSize=n;assert(n<=sizeof(lastData));memcpy(lastData,data,n);size_t length=0;
    const bool ok=codec.encodeGpioRequest(local,to,id,data,n,out.bytes,sizeof(out.bytes),length);
    if(ok)out.len=length;return ok;
  }
  void freeze()override{}
  bool matchesPeer(uint32_t)const override{return false;}
  bool revoke(uint32_t)override{return true;}
  void clearEvent()override{}
  void sweep(mt::Endpoint &)override{}
  int submit(mt::Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,bool,const uint8_t *,size_t,bool,uint32_t)override{++privateSubmits;return -EPERM;}
  void receive(mt::Endpoint &,bench::MtCodec &,const uint8_t *,size_t,uint32_t,uint32_t,bool)override{}
  void proof(mt::Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t)override{}
  bool event(uint8_t,const uint8_t *,size_t,mt::pki::PrivateEvent &)override{return false;}
};
static void feed(mt::Companion &c,const meshtastic_ToRadio &request,uint32_t now) {
  uint8_t bytes[512]{};auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));
  assert(pb_encode(&out,meshtastic_ToRadio_fields,&request));
  c.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});c.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});c.byte(uint8_t(out.bytes_written>>8),now,0,mt::Companion::WorkspaceProof{true});c.byte(uint8_t(out.bytes_written),now,0,mt::Companion::WorkspaceProof{true});
  for(size_t i=0;i<out.bytes_written;++i)c.byte(bytes[i],now,0,mt::Companion::WorkspaceProof{true});
}
static void feedRaw(mt::Companion &c,const uint8_t *bytes,size_t size,uint32_t now) {
  c.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});c.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});c.byte(uint8_t(size>>8),now,0,mt::Companion::WorkspaceProof{true});c.byte(uint8_t(size),now,0,mt::Companion::WorkspaceProof{true});
  for(size_t i=0;i<size;++i)c.byte(bytes[i],now,0,mt::Companion::WorkspaceProof{true});
}
static void actualSdkVector(const mt::test::gpio_sdk::Vector &vector) {
  TestRadio radio;TestPrivate priv;TestHost host;
  mt::Service service(radio,true,priv.local,81,true,false,&priv);
  mt::Config config;config.bytes[4]=1;auto node=config.node();node.setPublic();config.node(node);
  mt::ChannelRecord gpio;assert(gpio.set(2,"gpio",nullptr,0));
  memcpy(config.bytes+126,gpio.bytes,mt::ChannelRecord::Size);config.seal();
  assert(service.apply(config)&&service.start(false,0)&&service.armEndpoint(true));
  const unsigned initialFree=service.endpointFree();
  mt::companion::CompanionWorkspace workspace;mt::Companion companion(service,host,81,workspace,0);
  meshtastic_ToRadio handshake{};handshake.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
  handshake.want_config_id=vector.id;feed(companion,handshake,1);assert(companion.handshakes==1);
  feedRaw(companion,vector.bytes,vector.size,2);
  assert(priv.channelEncodes==1&&priv.privateSubmits==0&&service.endpointFree()<initialFree&&
         service.receiving()&&!service.faulted()&&radio.sends==0);
  meshtastic_Data decoded{};auto in=pb_istream_from_buffer(priv.lastData,priv.lastDataSize);
  assert(pb_decode(&in,meshtastic_Data_fields,&decoded)&&!in.bytes_left&&
         decoded.portnum==meshtastic_PortNum_REMOTE_HARDWARE_APP&&
         decoded.want_response==vector.wantResponse&&decoded.has_bitfield&&
         decoded.bitfield==(vector.wantResponse?2u:0u));
}
static size_t hardware(meshtastic_HardwareMessage_Type type,uint64_t mask,uint64_t value,uint8_t *out,size_t cap) {
  meshtastic_HardwareMessage msg{};msg.type=type;msg.gpio_mask=mask;msg.gpio_value=value;
  auto stream=pb_ostream_from_buffer(out,cap);assert(pb_encode(&stream,meshtastic_HardwareMessage_fields,&msg));return stream.bytes_written;
}
static meshtastic_Routing_Error lastError(const TestHost &host) {
  assert(!host.frames.empty());
  for(auto i=host.frames.rbegin();i!=host.frames.rend();++i)if(i->which_payload_variant==meshtastic_FromRadio_packet_tag&&
      i->packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
      i->packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP) {
    meshtastic_Routing routing{};auto in=pb_istream_from_buffer(i->packet.decoded.payload.bytes,i->packet.decoded.payload.size);
    assert(pb_decode(&in,meshtastic_Routing_fields,&routing)&&!in.bytes_left);return routing.error_reason;
  }
  assert(false);return meshtastic_Routing_Error_NONE;
}
static bool hasError(const TestHost &host,meshtastic_Routing_Error wanted) {
  for(const auto &f:host.frames)if(f.which_payload_variant==meshtastic_FromRadio_packet_tag&&
      f.packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
      f.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP) {
    meshtastic_Routing routing{};auto in=pb_istream_from_buffer(f.packet.decoded.payload.bytes,f.packet.decoded.payload.size);
    if(pb_decode(&in,meshtastic_Routing_fields,&routing)&&routing.error_reason==wanted)return true;
  }
  return false;
}
}
int main() {
  for(const auto &vector:mt::test::gpio_sdk::vectors)actualSdkVector(vector);

  // Canonical command shape includes stock response metadata but remains narrow
  // to READ/WRITE/WATCH HardwareMessage types and a <=24-byte payload.
  uint8_t payload[24]{},data[239]{};size_t payloadSize=hardware(meshtastic_HardwareMessage_Type_READ_GPIOS,1,0,payload,sizeof(payload)),dataSize=0;
  meshtastic_MeshPacket p{};p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.id=500;p.to=0x33445566;
  p.channel=0;p.hop_limit=3;p.want_ack=true;p.decoded.portnum=meshtastic_PortNum_REMOTE_HARDWARE_APP;
  p.decoded.payload.size=payloadSize;memcpy(p.decoded.payload.bytes,payload,payloadSize);
  p.decoded.want_response=true;p.decoded.has_bitfield=true;p.decoded.bitfield=2;
  assert(mt::PublicApplication::gpioClient(p));
  assert(mt::PublicApplication::encodeGpioRequest(p,data,sizeof(data),dataSize));
  assert(mt::PublicApplication::canonicalGpioRequest(data,dataSize));
  meshtastic_Data decoded{};auto in=pb_istream_from_buffer(data,dataSize);
  assert(pb_decode(&in,meshtastic_Data_fields,&decoded)&&!in.bytes_left&&decoded.want_response&&decoded.has_bitfield&&decoded.bitfield==2);
  p.decoded.want_response=false;p.decoded.bitfield=0;
  assert(mt::PublicApplication::encodeGpioRequest(p,data,sizeof(data),dataSize));
  in=pb_istream_from_buffer(data,dataSize);decoded={};assert(pb_decode(&in,meshtastic_Data_fields,&decoded)&&!decoded.want_response&&decoded.bitfield==0);
  p.decoded.portnum=meshtastic_PortNum_TELEMETRY_APP;p.decoded.want_response=true;p.decoded.bitfield=2;
  assert(!mt::PublicApplication::client(p)); // GPIO never widens generic response claims.
  p.decoded.portnum=meshtastic_PortNum_REMOTE_HARDWARE_APP;p.decoded.want_response=false;p.decoded.bitfield=0;
  payloadSize=hardware(meshtastic_HardwareMessage_Type_GPIOS_CHANGED,1,1,payload,sizeof(payload));
  p.decoded.payload.size=payloadSize;memcpy(p.decoded.payload.bytes,payload,payloadSize);
  assert(!mt::PublicApplication::gpioClient(p));

  // Actual Service and Companion: named custom gpio uses its paused channel
  // encoder once, does not downgrade explicit PKI, and exposes request_id only
  // as PhoneAPI correlation metadata after canonical reply validation.
  TestRadio radio;TestPrivate priv;TestHost host;
  mt::Service service(radio,true,priv.local,81,true,false,&priv);
  mt::Config config;config.bytes[4]=1;auto node=config.node();node.setPublic();config.node(node);
  mt::ChannelRecord gpio;assert(gpio.set(2,"gpio",nullptr,0));memcpy(config.bytes+126,gpio.bytes,mt::ChannelRecord::Size);config.seal();
  assert(service.apply(config));assert(service.start(false,0));assert(service.armEndpoint(true));
  mt::companion::CompanionWorkspace workspace;mt::Companion companion(service,host,81,workspace,0);
  meshtastic_ToRadio handshake{};handshake.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;handshake.want_config_id=99;feed(companion,handshake,1);
  assert(companion.handshakes==1);

  payloadSize=hardware(meshtastic_HardwareMessage_Type_READ_GPIOS,1,0,payload,sizeof(payload));
  p={};p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.id=500;p.to=0x33445566;p.channel=1;
  p.hop_limit=3;p.want_ack=true;p.decoded.portnum=meshtastic_PortNum_REMOTE_HARDWARE_APP;
  p.decoded.payload.size=payloadSize;memcpy(p.decoded.payload.bytes,payload,payloadSize);
  p.decoded.want_response=true;p.decoded.has_bitfield=true;p.decoded.bitfield=2;
  meshtastic_ToRadio send{};send.which_payload_variant=meshtastic_ToRadio_packet_tag;send.packet=p;
  const unsigned beforeQueue=service.endpointFree();
  feed(companion,send,2);assert(priv.channelEncodes==1&&priv.privateSubmits==0&&radio.sends==0);
  assert(service.endpointFree()<beforeQueue);
  const unsigned afterQueue=service.endpointFree();feed(companion,send,3);
  assert(priv.channelEncodes==1&&radio.sends==0&&service.endpointFree()==afterQueue&&
    !hasError(host,meshtastic_Routing_Error_BAD_REQUEST)); // Exact-ID replay is idempotent.
  p.decoded.payload.bytes[p.decoded.payload.size-1]^=1;send.packet=p;feed(companion,send,4);
  assert(priv.channelEncodes==1&&lastError(host)==meshtastic_Routing_Error_BAD_REQUEST);
  p.decoded.payload.bytes[p.decoded.payload.size-1]^=1;p.pki_encrypted=true;send.packet=p;feed(companion,send,5);
  assert(priv.channelEncodes==1&&lastError(host)==meshtastic_Routing_Error_PKI_FAILED);

  uint8_t replyPayload[24]{},replyData[64]{},frame[255]{};size_t replySize=hardware(meshtastic_HardwareMessage_Type_READ_GPIOS_REPLY,1,1,replyPayload,sizeof(replyPayload));
  size_t wireSize=0;assert(mt::modules::GpioModuleWire::outgoing(meshtastic_PortNum_REMOTE_HARDWARE_APP,
    replyPayload,replySize,500,replyData,sizeof(replyData),wireSize));
  assert(service.selectChannel(1));size_t frameSize=0;
  assert(service.codec.encodeZeroHopData(0x33445566,priv.local,700,replyData,wireSize,frame,sizeof(frame),frameSize));
  frame[12]=uint8_t(2|(2<<5));
  bench::RouteMetadata debugMeta{};unsigned debugChannel=99;assert(service.decodeChannel(frame,frameSize,debugMeta,debugChannel)&&debugChannel==1&&mt::PublicApplication::received(service.codec,frame,frameSize,debugMeta));
  const size_t before=host.frames.size();companion.receive(frame,frameSize,-80,5,10,10,true,5,true,mt::Companion::WorkspaceProof{true});
  assert(host.frames.size()>before);
  const meshtastic_MeshPacket *received=nullptr;
  for(auto i=host.frames.rbegin();i!=host.frames.rend();++i)if(i->which_payload_variant==meshtastic_FromRadio_packet_tag&&
      i->packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
      i->packet.decoded.portnum==meshtastic_PortNum_REMOTE_HARDWARE_APP){received=&i->packet;break;}
  assert(received&&received->decoded.request_id==500&&received->decoded.payload.size==replySize&&
    !memcmp(received->decoded.payload.bytes,replyPayload,replySize)&&!received->pki_encrypted&&
    !received->public_key.size&&!received->xeddsa_signed);
  std::cout<<"M138 GPIO client and Companion integration tests passed\n";
}
