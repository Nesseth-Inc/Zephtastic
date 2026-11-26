// SPDX-License-Identifier: GPL-3.0-only
// Memory-only stock-client bridge. It executes the production Companion ->
// Service -> Endpoint -> codec path at both virtual endpoints and prints only
// canonical FromRadio protobufs for the Python parser. It opens no transport.
#include "Companion.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

struct BridgeRadio:mt::Radio {
  bool done=false;std::vector<std::vector<uint8_t>> frames;
  bool ready()override{return true;}int start()override{return 0;}int restore()override{return 0;}int stop()override{return 0;}
  int health()override{return 1;}bool busy()override{return false;}uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *p,size_t n)override{frames.emplace_back(p,p+n);done=false;return 0;}
  bool completion(int &status)override{status=0;return done;}
};
struct BridgeHost:mt::Companion::Host {
  std::vector<meshtastic_FromRadio> outputs;
  bool ready()const override{return true;}int save(const mt::Config &)override{return -ENOTSUP;}
  bool send(const uint8_t *p,size_t n)override {
    if(n<4||p[0]!=0x94||p[1]!=0xc3||size_t((p[2]<<8)|p[3])!=n-4)return false;
    meshtastic_FromRadio out{};auto stream=pb_istream_from_buffer(p+4,n-4);
    if(!pb_decode(&stream,meshtastic_FromRadio_fields,&out))return false;
    outputs.push_back(out);return true;
  }
};
struct BridgeNode {
  mt::test::KnownChannelFixtureClock knownChannelClock;
  BridgeRadio radio;mt::Service service;BridgeHost host;mt::companion::CompanionWorkspace workspace;mt::Companion companion;
  explicit BridgeNode(uint32_t id,unsigned role=meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE):service(radio,true,id,81,true,false),companion(service,host,81,workspace,0) {
    knownChannelClock.bind(service);
    mt::Config config;config.bytes[4]=1;assert(config.setDeviceRole(role,0));auto node=config.node();node.setPublic();config.node(node);
    assert(service.apply(config)&&service.start(false,0)&&service.armEndpoint(true));
    meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=1;
    uint8_t wire[32]{};auto stream=pb_ostream_from_buffer(wire,sizeof(wire));assert(pb_encode(&stream,meshtastic_ToRadio_fields,&hello));
    companion.packet(wire,stream.bytes_written,1,mt::Companion::WorkspaceProof{true});host.outputs.clear();
  }
  void receive(const std::vector<uint8_t> &frame,int8_t snr,uint32_t now,uint32_t random) {
    service.rx(frame.data(),frame.size(),snr,now,random);
    companion.receive(frame.data(),frame.size(),-90,snr,now,mt::Companion::WorkspaceProof{true});
  }
  void complete(uint32_t now){radio.done=true;service.poll(now,0);companion.poll(mt::Companion::WorkspaceProof{true});}
};
static std::vector<uint8_t> unhex(const char *text) {
  std::string s=text?text:"";if(s.empty()||s.size()%2)return {};
  std::vector<uint8_t> out(s.size()/2);auto nib=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
  for(size_t i=0;i<out.size();++i){int a=nib(s[2*i]),b=nib(s[2*i+1]);if(a<0||b<0)return {};out[i]=uint8_t((a<<4)|b);}return out;
}
static std::string hex(const uint8_t *p,size_t n) {
  static const char digits[]="0123456789abcdef";std::string out;out.resize(n*2);
  for(size_t i=0;i<n;++i){out[2*i]=digits[p[i]>>4];out[2*i+1]=digits[p[i]&15];}return out;
}
int main(int argc,char **argv) {
  if(argc<2||argc>3)return 64;
  auto request=unhex(argv[1]);if(request.empty())return 65;
  const std::string mode=argc==3?argv[2]:"direct";
  if(mode!="direct"&&mode!="transit"&&mode!="asymmetric")return 64;
  constexpr uint32_t source=0x12345678,relayId=0x55667788,target=0x87654321;BridgeNode a(source),relay(relayId,meshtastic_Config_DeviceConfig_Role_ROUTER),b(target);
  assert(relay.service.relaying()&&!a.service.relaying()&&!b.service.relaying());
  a.companion.packet(request.data(),request.size(),100,mt::Companion::WorkspaceProof{true});a.service.poll(300,700);
  if(a.radio.frames.size()!=1)return 66;
  a.complete(301);
  bench::RouteMetadata m;if(!a.service.codec.decode(a.radio.frames[0].data(),a.radio.frames[0].size(),m)||
    m.portnum!=meshtastic_PortNum_TRACEROUTE_APP||!m.traceRouteValid||!m.wantResponse)return 67;
  if(mode=="direct") {
    b.receive(a.radio.frames[0],-10,500,701);b.service.poll(800,702);
    if(b.radio.frames.size()!=1)return 68;
    b.complete(801);a.receive(b.radio.frames[0],-12,1000,703);
  } else if(mode=="transit") {
    relay.receive(a.radio.frames[0],-5,500,701);relay.service.poll(800,702);
    if(relay.radio.frames.size()!=1)return 68;
    relay.complete(801);
    b.receive(relay.radio.frames[0],-10,900,703);b.service.poll(1200,704);
    if(b.radio.frames.size()!=1)return 68;
    b.complete(1201);relay.receive(b.radio.frames[0],-7,1300,705);relay.service.poll(1600,706);
    if(relay.radio.frames.size()!=2)return 68;
    relay.complete(1601);a.receive(relay.radio.frames[1],-12,1700,707);
  } else {
    meshtastic_RouteDiscovery route{};route.route_count=1;route.route[0]=0x11112222;
    route.snr_towards_count=2;route.snr_towards[0]=4;route.snr_towards[1]=12;
    route.route_back_count=1;route.route_back[0]=0x33334444;route.snr_back_count=1;route.snr_back[0]=-8;
    uint8_t payload[mt::TraceRoute::MaxPayload]{},data[239]{},frame[255]{};size_t payloadSize=0,dataSize=0,frameSize=0;
    auto encoded=pb_ostream_from_buffer(payload,sizeof(payload));
    if(!pb_encode(&encoded,meshtastic_RouteDiscovery_fields,&route))return 71;
    payloadSize=encoded.bytes_written;
    const uint32_t requestId=bench::le32(a.radio.frames[0].data()+8);
    if(!mt::TraceRoute::responseData(payload,payloadSize,requestId,data,sizeof(data),dataSize)||
       !a.service.codec.encodeZeroHopData(target,source,0x700,data,dataSize,frame,sizeof(frame),frameSize))return 72;
    frame[12]=uint8_t((3<<5)|1);a.receive({frame,frame+frameSize},40,1000,703);
  }
  a.companion.poll(mt::Companion::WorkspaceProof{true});
  unsigned traces=0;
  for(const auto &out:a.host.outputs)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&
    out.packet.decoded.portnum==meshtastic_PortNum_TRACEROUTE_APP) {
    uint8_t bytes[512]{};auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
    if(!pb_encode(&stream,meshtastic_FromRadio_fields,&out))return 69;
    std::cout<<hex(bytes,stream.bytes_written)<<"\n";++traces;
  }
  return traces==1?0:70;
}
