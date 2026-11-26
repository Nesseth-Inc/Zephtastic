// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include "known_channel_fixture.h"
#include "TelemetryAnnouncement.h"

static constexpr uint32_t A=0x12345678,B=0x87654321,H1=0x11112222,H2=0x33334444;
static meshtastic_ToRadio localTraceRequest(uint32_t packet,uint32_t to,unsigned hop,
  const meshtastic_RouteDiscovery &route=meshtastic_RouteDiscovery_init_zero) {
  meshtastic_ToRadio in{};in.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=in.packet;
  p.id=packet;p.to=to;p.hop_limit=hop;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_TRACEROUTE_APP;p.decoded.want_response=true;
  auto stream=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
  assert(pb_encode(&stream,meshtastic_RouteDiscovery_fields,&route));p.decoded.payload.size=stream.bytes_written;
  return in;
}
struct TraceNode {
  mt::test::KnownChannelFixtureClock knownChannelClock;
  Radio radio;mt::Service service;Host usbHost,bleHost;mt::companion::CompanionWorkspace workspace;mt::Companion usb,ble;uint32_t id;
  explicit TraceNode(uint32_t node,bool tx=true,uint32_t start=0):service(radio,tx,node,81,true,false),usbHost(service),bleHost(service),usb(service,usbHost,81,workspace,0),ble(service,bleHost,81,workspace,1),id(node) {
    knownChannelClock.bind(service);
    mt::Config cfg;cfg.bytes[4]=1;auto n=cfg.node();n.setPublic();cfg.node(n);
    assert(service.apply(cfg)&&service.start(false,start));if(tx)assert(service.armEndpoint(true));
    meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=1;
    input(usb,hello,start+1);input(ble,hello,start+1);usbHost.out.clear();bleHost.out.clear();
  }
  void request(uint32_t packet,uint32_t to,unsigned hop,const meshtastic_RouteDiscovery &route=meshtastic_RouteDiscovery_init_zero,uint8_t owner=0) {
    auto in=localTraceRequest(packet,to,hop,route);input(owner?ble:usb,in,100);
  }
  void receive(const std::vector<uint8_t> &frame,int8_t snr,uint32_t now,uint32_t random=700) {
    service.rx(frame.data(),frame.size(),snr,now,random);
    usb.receive(frame.data(),frame.size(),-90,snr,now,mt::Companion::WorkspaceProof{true});ble.receive(frame.data(),frame.size(),-90,snr,now,mt::Companion::WorkspaceProof{true});
  }
  void poll(uint32_t now,uint32_t random=0){service.poll(now,random);usb.poll(mt::Companion::WorkspaceProof{true});ble.poll(mt::Companion::WorkspaceProof{true});}
  void done(uint32_t now){radio.done=true;service.poll(now,0);usb.poll(mt::Companion::WorkspaceProof{true});ble.poll(mt::Companion::WorkspaceProof{true});}
};
static std::vector<uint8_t> traceFrame(TraceNode &codecNode,uint32_t from,uint32_t to,uint32_t packet,
  uint32_t request,bool wantResponse,const meshtastic_RouteDiscovery &route,uint8_t flags=0) {
  uint8_t routeBytes[233]{},data[239]{},frame[255]{};size_t routeSize=0,dataSize=0,frameSize=0;
  auto routeOut=pb_ostream_from_buffer(routeBytes,sizeof(routeBytes));assert(pb_encode(&routeOut,meshtastic_RouteDiscovery_fields,&route));routeSize=routeOut.bytes_written;
  if(wantResponse)assert(mt::TraceRoute::requestData(routeBytes,routeSize,data,sizeof(data),dataSize));
  else assert(mt::TraceRoute::responseData(routeBytes,routeSize,request,data,sizeof(data),dataSize));
  if(wantResponse)assert(codecNode.service.codec.encodeTraceRouteRequest(from,to,packet,data,dataSize,frame,sizeof(frame),frameSize));
  else assert(codecNode.service.codec.encodeZeroHopData(from,to,packet,data,dataSize,frame,sizeof(frame),frameSize));
  frame[12]=flags;return {frame,frame+frameSize};
}
static std::vector<uint8_t> uncheckedRequestFrame(TraceNode &codecNode,uint32_t from,uint32_t to,uint32_t packet,
  const meshtastic_RouteDiscovery &route,uint8_t flags=0) {
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_TRACEROUTE_APP;data.want_response=true;
  data.has_bitfield=true;auto payload=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
  assert(pb_encode(&payload,meshtastic_RouteDiscovery_fields,&route));data.payload.size=payload.bytes_written;
  uint8_t encoded[239]{},frame[255]{};auto wire=pb_ostream_from_buffer(encoded,sizeof(encoded));
  assert(pb_encode(&wire,meshtastic_Data_fields,&data));size_t frameSize=0;
  assert(codecNode.service.codec.encodeTraceRouteRequest(from,to,packet,encoded,wire.bytes_written,
    frame,sizeof(frame),frameSize));frame[12]=flags;return {frame,frame+frameSize};
}
static bool traceOutput(const Host &host,uint32_t request,uint32_t from,meshtastic_RouteDiscovery *route=nullptr) {
  for(const auto &out:host.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&
    out.packet.decoded.portnum==meshtastic_PortNum_TRACEROUTE_APP&&out.packet.decoded.request_id==request&&out.packet.from==from) {
    if(route){*route={};auto in=pb_istream_from_buffer(out.packet.decoded.payload.bytes,out.packet.decoded.payload.size);assert(pb_decode(&in,meshtastic_RouteDiscovery_fields,route));}
    return true;
  }
  return false;
}
static meshtastic_RouteDiscovery directTowards(int8_t q4) {
  meshtastic_RouteDiscovery r{};r.snr_towards_count=1;r.snr_towards[0]=q4;return r;
}
static std::vector<uint8_t> genericResponse(TraceNode &node,uint32_t from,uint32_t packet,uint32_t request) {
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_POSITION_APP;data.request_id=request;data.has_bitfield=true;
  uint8_t encoded[239]{},frame[255]{};auto wire=pb_ostream_from_buffer(encoded,sizeof(encoded));
  assert(pb_encode(&wire,meshtastic_Data_fields,&data));size_t frameSize=0;
  assert(node.service.codec.encodeZeroHopData(from,node.id,packet,encoded,wire.bytes_written,
    frame,sizeof(frame),frameSize));return {frame,frame+frameSize};
}
static std::vector<uint8_t> routingAck(TraceNode &node,uint32_t from,uint32_t to,
    uint32_t packet,uint32_t request,uint8_t flags=0) {
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_ROUTING_APP;data.request_id=request;data.has_bitfield=true;
  meshtastic_Routing routing{};routing.which_variant=meshtastic_Routing_error_reason_tag;
  routing.error_reason=meshtastic_Routing_Error_NONE;
  auto payload=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
  assert(pb_encode(&payload,meshtastic_Routing_fields,&routing));data.payload.size=payload.bytes_written;
  uint8_t encoded[239]{},frame[255]{};size_t size=0;
  auto wire=pb_ostream_from_buffer(encoded,sizeof(encoded));assert(pb_encode(&wire,meshtastic_Data_fields,&data));
  assert(node.service.codec.encodeZeroHopData(from,to,packet,encoded,wire.bytes_written,frame,sizeof(frame),size));
  frame[12]=flags;return {frame,frame+size};
}
static void rawRequest(TraceNode &node,uint32_t id,const std::vector<uint8_t> &payload,
                       uint32_t to=B,unsigned hop=3,unsigned channel=0,bool pki=false) {
  meshtastic_ToRadio in{};in.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=in.packet;
  p.id=id;p.to=to;p.hop_limit=hop;p.channel=channel;p.pki_encrypted=pki;
  p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_TRACEROUTE_APP;
  p.decoded.want_response=true;p.decoded.payload.size=payload.size();if(!payload.empty())memcpy(p.decoded.payload.bytes,payload.data(),payload.size());
  input(node.usb,in,100);
}
static void checkHopScalingService(bool rf,bool viaMqtt,unsigned expected) {
  TraceNode node(A);
  uint8_t telemetry[128]{};
  size_t size=0;
  assert(mt::TelemetryAnnouncement::encode(123,{}, {},telemetry,sizeof(telemetry),size));
  for(uint32_t peer=201;peer<=282;++peer) {
    uint8_t frame[255]{};
    size_t length=0;
    assert(node.service.codec.encodeZeroHopData(peer,A,peer,telemetry,size,frame,sizeof(frame),length));
    frame[12]=uint8_t(3u|(3u<<5)|(viaMqtt?0x10u:0u));
    node.service.rx(frame,length,5,1000,12345,rf);
  }
  // Complete a three-hop broadcast before the cap changes. Its request ID
  // must remain idempotent in history even after the RF policy moves.
  uint8_t original[64]{};
  size_t originalSize=0;
  assert(mt::PublicApplication::encode(meshtastic_PortNum_NEIGHBORINFO_APP,nullptr,0,0,0,
                                       original,sizeof(original),originalSize));
  constexpr uint32_t earlyId=44001;
  assert(!node.service.submitPublicApplication(0,UINT32_MAX,earlyId,original,originalSize,3,false,1100));
  node.poll(1600,12345);
  assert(node.radio.sends==1 && (node.radio.frames.back()[12]&7)==3);
  node.done(1601);
  // Feed the real channel meter at 35% for one hour so its smoothed
  // reading crosses stock's three-consecutive-five-minute congestion gate.
  for(uint32_t now=10000;now<=3630000u;now+=10000u) {
    assert(node.knownChannelClock.set(now));
    node.service.channelMeter().observeRx(3500,true);
    node.service.poll(now,12345);
  }
  const uint32_t send=3700000;
  assert(node.knownChannelClock.set(send));
  assert(node.radio.sends==1);
  assert(!node.service.submitPublicApplication(0,UINT32_MAX,earlyId,original,originalSize,3,false,send));
  assert(node.service.submitPublicApplication(0,UINT32_MAX,earlyId,original,originalSize,2,false,send)==-EINVAL);
  uint32_t id=0;
  assert(!node.service.submitModuleAnnouncement(telemetry,size,3,send,54321,id));
  node.poll(send+300,12345);
  assert(node.radio.sends==2);
  const auto &sent=node.radio.frames.back();
  assert(bench::le32(sent.data()+8)==id && sent[12]==uint8_t(expected|(expected<<5)));
  bench::RouteMetadata metadata;
  assert(node.service.codec.decode(sent.data(),sent.size(),metadata));
  assert(metadata.portnum==meshtastic_PortNum_TELEMETRY_APP && metadata.payloadLength);
  node.done(send+301);
  for(const uint32_t to:{UINT32_MAX,B}) {
    uint8_t data[64]{};
    size_t n=0;
    assert(mt::PublicApplication::encode(meshtastic_PortNum_NEIGHBORINFO_APP,nullptr,0,0,0,data,sizeof(data),n));
    const auto packet=(to==B ? 54324u : 54323u);
    const uint32_t now=send+(to==B ? 3000u : 1500u);
    assert(!node.service.submitPublicApplication(0,to,packet,data,n,3,false,now));
    assert(!node.service.submitPublicApplication(0,to,packet,data,n,3,false,now+1));
    assert(node.service.submitPublicApplication(0,to,packet,data,n,2,false,now+2)==-EINVAL);
    node.poll(now+300);
    const auto &neighbor=node.radio.frames.back();
    const unsigned hops=to==B ? 3 : expected;
    assert(bench::le32(neighbor.data()+8)==packet && neighbor[12]==uint8_t(hops|(hops<<5)));
    node.done(now+301);
  }
}
int main() {
  (void)&results;
  checkHopScalingService(true,false,0);
  checkHopScalingService(true,true,3);
  checkHopScalingService(false,false,3);
  group("Measured congestion and RF peers cap Telemetry and phone-origin NeighborInfo broadcasts while preserving unicast, MQTT and local-injection behavior");
  {
    TraceNode a(A);
    for(unsigned bits: {0u,1u,2u,3u,4u,255u}) {
      // Stock Router adds bit1 to an ordinary empty SDK TRACE request.
      meshtastic_Data data{};data.portnum=meshtastic_PortNum_TRACEROUTE_APP;
      data.want_response=true;data.has_bitfield=true;data.bitfield=bits;
      uint8_t encoded[239]{},frame[255]{};size_t frameSize=0;
      auto wire=pb_ostream_from_buffer(encoded,sizeof(encoded));assert(pb_encode(&wire,meshtastic_Data_fields,&data));
      const bool expected=bits==0||bits==2;
      assert(a.service.codec.encodeTraceRouteRequest(A,B,77,encoded,wire.bytes_written,frame,sizeof(frame),frameSize)==expected);
      bench::RouteMetadata metadata;assert(bench::MtCodec::decodeData(encoded,wire.bytes_written,metadata));
      assert(mt::TraceRoute::requestShape(metadata)==expected);
      if(expected) {
        assert(a.service.codec.validateTraceRouteData(frame,frameSize,encoded,wire.bytes_written));
        assert(a.service.codec.decode(frame,frameSize,metadata)&&metadata.bitfield==bits);
      }
    }
    group("stock TRACE request bit2 and legacy bit0 preserve exact bytes; MQTT and unknown bits remain refused");
  }
  {
    TraceNode a(A),b(B);meshtastic_Data request{};
    request.portnum=meshtastic_PortNum_TRACEROUTE_APP;request.want_response=true;
    request.has_bitfield=true;request.bitfield=2; // Pinned stock Router enrichment.
    uint8_t data[239]{},frame[255]{};size_t size=0;
    auto wire=pb_ostream_from_buffer(data,sizeof(data));assert(pb_encode(&wire,meshtastic_Data_fields,&request));
    assert(a.service.codec.encodeTraceRouteRequest(A,B,77,data,wire.bytes_written,frame,sizeof(frame),size));
    b.receive(std::vector<uint8_t>(frame,frame+size),-10,500);b.poll(800,900);
    assert(b.radio.sends==1);const auto &reply=b.radio.frames[0];bench::RouteMetadata received;
    assert(b.service.codec.decode(reply.data(),reply.size(),received)&&received.traceRouteValid);
    assert(bench::le32(reply.data())==A&&bench::le32(reply.data()+4)==B&&
      received.portnum==70&&received.request_id==77&&!received.wantResponse&&received.hasBitfield&&!received.bitfield);
    meshtastic_RouteDiscovery route{};auto payload=pb_istream_from_buffer(received.payload,received.payloadLength);
    assert(pb_decode(&payload,meshtastic_RouteDiscovery_fields,&route)&&payload.bytes_left==0);
    assert(!route.route_count&&route.snr_towards_count==1&&route.snr_towards[0]==-40&&
      !route.route_back_count&&!route.snr_back_count);
    group("stock bit2 request crosses destination Service admission and produces one canonical correlated RF reply");
  }
  {
    for(unsigned start:{1u,3u}) {
      TraceNode source(A),destination(B);meshtastic_Data request{};
      request.portnum=meshtastic_PortNum_TRACEROUTE_APP;request.want_response=true;
      request.has_bitfield=true;request.bitfield=2;
      uint8_t data[239]{},frame[255]{};size_t size=0;
      auto wire=pb_ostream_from_buffer(data,sizeof(data));assert(pb_encode(&wire,meshtastic_Data_fields,&request));
      const uint32_t packet=780+start;
      assert(source.service.codec.encodeTraceRouteRequest(A,B,packet,data,wire.bytes_written,frame,sizeof(frame),size));
      // Stock PhoneAPI forces RF want_ack; Router records the initial hop budget.
      frame[12]=uint8_t(start|(start<<5)|8);
      destination.receive({frame,frame+size},6,500);destination.poll(800,900+start);
      assert(destination.radio.sends==1);const auto &reply=destination.radio.frames[0];
      bench::RouteMetadata received;assert(destination.service.codec.decode(reply.data(),reply.size(),received));
      assert(bench::le32(reply.data())==A&&bench::le32(reply.data()+4)==B&&
        received.portnum==70&&received.request_id==packet&&!received.wantResponse&&received.hasBitfield&&!received.bitfield);
      assert((reply[12]&uint8_t(~8u))==uint8_t(2|(2<<5)));
      meshtastic_RouteDiscovery route{};auto payload=pb_istream_from_buffer(received.payload,received.payloadLength);
      assert(pb_decode(&payload,meshtastic_RouteDiscovery_fields,&route)&&payload.bytes_left==0);
      assert(!route.route_count&&route.snr_towards_count==1&&route.snr_towards[0]==24&&
        !route.route_back_count&&!route.snr_back_count);
    }
    group("stock bit2 plus forced RF ACK and explicit or default hop budget reach destination and generate direct replies");
  }
  {
    TraceNode source(A),destination(B);meshtastic_RouteDiscovery empty{};
    const auto request=traceFrame(source,A,B,100,0,true,empty,uint8_t(1|(1<<5)|8));
    destination.receive(request,6,10);destination.poll(260);assert(destination.radio.sends==1);
    const auto response=destination.radio.frames[0];const uint32_t replyId=bench::le32(response.data()+8);
    assert(response[12]==uint8_t(2|(2<<5)|8));destination.done(261);
    assert(!destination.service.storageStopSafe());
    destination.receive(routingAck(destination,H1,B,705,replyId),1,270);assert(!destination.service.storageStopSafe());
    destination.receive(routingAck(destination,A,B,706,replyId+1),1,280);assert(!destination.service.storageStopSafe());
    destination.receive(routingAck(destination,A,H2,707,replyId),1,290);assert(!destination.service.storageStopSafe());
    destination.receive(request,6,300);destination.poll(600);assert(destination.radio.sends==1);
    destination.receive(routingAck(destination,A,B,708,replyId,8),1,610);destination.poll(1000);
    assert(destination.service.storageStopSafe()&&destination.radio.sends==1);
    assert(!traceOutput(destination.usbHost,100,A)&&!traceOutput(destination.bleHost,100,A));
    group("generated reliable TRACE requires the matching peer and response ID ACK; duplicates and ACK-of-ACK produce no extra send");
  }
  {
    TraceNode source(A),destination(B);meshtastic_RouteDiscovery empty{};
    destination.receive(traceFrame(source,A,B,101,0,true,empty,8),6,10);destination.poll(260);
    assert(destination.radio.sends==1&&(destination.radio.frames[0][12]&8));
    const auto response=destination.radio.frames[0];destination.done(261);
    destination.poll(20000);destination.poll(20300);assert(destination.radio.sends==2&&destination.radio.frames[1]==response);
    destination.done(20301);destination.poll(40000);destination.poll(40300);
    assert(destination.radio.sends==3&&destination.radio.frames[2]==response);destination.done(40301);
    destination.poll(60000);destination.poll(60300);
    assert(destination.radio.sends==4&&destination.radio.frames[3]==response);destination.done(60301);
    destination.poll(80000);destination.poll(80300);
    assert(destination.radio.sends==5&&destination.radio.frames[4]==response);destination.done(80301);
    destination.poll(100000);destination.poll(110000);
    assert(destination.radio.sends==5&&destination.service.storageStopSafe());
    group("lost ACK retries generated reliable directed TRACE five total attempts with identical bytes then releases control storage");
  }
  {
    TraceNode a(A),b(B);a.request(77,B,0);a.poll(300);assert(a.radio.sends==1);a.done(301);
    b.receive(a.radio.frames[0],-10,500);b.poll(800,900);assert(b.radio.sends==1);
    bench::RouteMetadata generated;assert(b.service.codec.decode(b.radio.frames[0].data(),b.radio.frames[0].size(),generated));
    assert(generated.portnum==70&&generated.request_id==77&&!generated.wantResponse&&generated.traceRouteValid);
    meshtastic_RouteDiscovery first{};auto pin=pb_istream_from_buffer(generated.payload,generated.payloadLength);
    assert(pb_decode(&pin,meshtastic_RouteDiscovery_fields,&first)&&!first.route_count&&first.snr_towards_count==1&&first.snr_towards[0]==-40);
    b.done(801);a.receive(b.radio.frames[0],-12,1000);a.usb.poll(mt::Companion::WorkspaceProof{true});a.ble.poll(mt::Companion::WorkspaceProof{true});meshtastic_RouteDiscovery result{};
    assert(traceOutput(a.usbHost,77,B,&result)&&!traceOutput(a.bleHost,77,B));
    assert(!result.route_count&&result.snr_towards_count==1&&result.snr_towards[0]==-40&&
      !result.route_back_count&&result.snr_back_count==1&&result.snr_back[0]==-48);
    group("direct endpoint round trip correlates one owner and integer-dB-derived q4 SNR through production queues");
  }
  {
    TraceNode a(A),b(B);meshtastic_RouteDiscovery pre{};pre.route_count=1;pre.route[0]=H1;pre.snr_towards_count=1;pre.snr_towards[0]=8;
    auto request=traceFrame(a,A,B,88,0,true,pre,uint8_t((3<<5)|1));b.receive(request,-40,1000);b.poll(1300,901);
    assert(b.radio.sends==1);bench::RouteMetadata m;assert(b.service.codec.decode(b.radio.frames[0].data(),b.radio.frames[0].size(),m));
    meshtastic_RouteDiscovery out{};auto in=pb_istream_from_buffer(m.payload,m.payloadLength);assert(pb_decode(&in,meshtastic_RouteDiscovery_fields,&out));
    assert(out.route_count==2&&out.route[0]==H1&&out.route[1]==0xffffffffu&&out.snr_towards_count==3&&
      out.snr_towards[0]==8&&out.snr_towards[1]==-128&&out.snr_towards[2]==-127&&b.radio.frames[0][12]==(3|(3<<5)));
    assert(!b.service.route.routeCount()&&!b.service.nodes.count());
    group("destination preserves pre-populated forward route, fills unknown hop, clamps measured SNR and uses bounded response hop");
  }
  {
    struct HopCase {uint8_t hop,start,response;};
    for(const auto c:{HopCase{0,0,0},HopCase{3,3,2},HopCase{2,3,3},HopCase{1,3,3}}) {
      TraceNode source(A),destination(B);meshtastic_RouteDiscovery empty{};
      auto request=traceFrame(source,A,B,900+c.hop,0,true,empty,uint8_t((c.start<<5)|c.hop));
      destination.receive(request,-3,1000,1000+c.hop);destination.poll(1300,1100+c.hop);
      assert(destination.radio.sends==1&&destination.radio.frames[0][12]==uint8_t(c.response|(c.response<<5)));
      destination.done(1301);
    }
    group("destination response header follows the pinned known-hop budget for 0/0, 3/3, 3/2 and 3/1");
  }
  {
    TraceNode a(A),b(B);a.request(89,B,0,meshtastic_RouteDiscovery_init_zero,1);a.poll(300);a.done(301);
    b.receive(a.radio.frames[0],-2,500);b.poll(800,901);b.done(801);a.receive(b.radio.frames[0],-3,1000);
    assert(!traceOutput(a.usbHost,89,B)&&traceOutput(a.bleHost,89,B));
    group("BLE-originated request receives the correlated callback only on its owning transport");
  }
  {
    TraceNode a(A);a.request(99,B,3);a.poll(300);a.done(301);
    meshtastic_RouteDiscovery asymmetric{};asymmetric.route_count=1;asymmetric.route[0]=H1;
    asymmetric.snr_towards_count=2;asymmetric.snr_towards[0]=4;asymmetric.snr_towards[1]=12;
    asymmetric.route_back_count=1;asymmetric.route_back[0]=H2;asymmetric.snr_back_count=1;asymmetric.snr_back[0]=-8;
    auto response=traceFrame(a,B,A,700,99,false,asymmetric,uint8_t((3<<5)|1));a.receive(response,40,500);meshtastic_RouteDiscovery out{};
    assert(traceOutput(a.usbHost,99,B,&out));
    assert(out.route_back_count==2&&out.route_back[0]==H2&&out.route_back[1]==0xffffffffu&&out.snr_back_count==3&&
      out.snr_back[0]==-8&&out.snr_back[1]==-128&&out.snr_back[2]==127);
    assert(!a.service.route.routeCount()&&!a.service.nodes.count());
    group("origin preserves asymmetric reverse route, fills unknown hop and clamps positive SNR");
  }
  {
    TraceNode a(A);a.request(111,B,3);a.poll(300);auto route=directTowards(4);
    auto early=traceFrame(a,B,A,703,111,false,route);a.receive(early,-1,301);assert(!traceOutput(a.usbHost,111,B));
    a.done(302);
    a.receive(genericResponse(a,B,704,111),-1,350);bool forwarded=false;
    for(const auto &out:a.usbHost.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&
      out.packet.decoded.portnum==meshtastic_PortNum_POSITION_APP)forwarded=true;
    // M27 rejects an unmatched correlated Position before client parsing.
    assert(!forwarded&&!traceOutput(a.usbHost,111,B));
    auto wrong=traceFrame(a,B+1,A,701,111,false,route);a.receive(wrong,-1,400);assert(!traceOutput(a.usbHost,111,B+1));
    auto unsolicited=traceFrame(a,B,A,702,112,false,route);a.receive(unsolicited,-1,401);assert(!traceOutput(a.usbHost,112,B));
    auto right=early;a.receive(right,-1,402);assert(traceOutput(a.usbHost,111,B));
    a.receive(right,-1,403);assert(traceOutput(a.usbHost,111,B));unsigned count=0;
    for(auto &out:a.usbHost.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.decoded.portnum==70)++count;
    assert(count==1);
    group("early, wrong-peer, wrong-request and duplicate responses cannot forge a callback");
  }
  {
    TraceNode a(A);a.request(120,B,3);a.poll(300);a.done(301);
    auto ack=a.radio.frames[0];meshtastic_Data d{};d.portnum=meshtastic_PortNum_ROUTING_APP;d.request_id=120;d.has_bitfield=true;
    meshtastic_Routing routing{};routing.which_variant=meshtastic_Routing_error_reason_tag;routing.error_reason=meshtastic_Routing_Error_NONE;
    auto po=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));assert(pb_encode(&po,meshtastic_Routing_fields,&routing));d.payload.size=po.bytes_written;
    uint8_t bytes[239],frame[255];auto dataOut=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&dataOut,meshtastic_Data_fields,&d));size_t n=0;
    assert(a.service.codec.encodeZeroHopData(B,A,704,bytes,dataOut.bytes_written,frame,sizeof(frame),n));a.receive({frame,frame+n},-1,400);
    assert(!traceOutput(a.usbHost,120,B));a.service.poll(90200,0);a.usb.poll(mt::Companion::WorkspaceProof{true});
    bool failed=false;for(auto &out:a.usbHost.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&
      out.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP&&out.packet.decoded.request_id==120)failed=true;
    assert(failed);
    group("Routing ACK cannot masquerade as traceroute and application wait expires without RF retry");
  }
  {
    TraceNode b(B);meshtastic_RouteDiscovery empty{};auto request=traceFrame(b,A,B,130,0,true,empty);
    b.receive(request,-3,1000,800);b.receive(request,-3,4000,801);b.poll(4300,802);assert(b.radio.sends==1);b.done(4301);
    b.receive(request,-3,20000,803);b.poll(20300,804);assert(b.radio.sends==1);
    assert(b.service.armEndpoint(false));auto next=traceFrame(b,A+1,B,131,0,true,empty);b.receive(next,-3,21000,805);b.poll(22000,806);assert(b.radio.sends==1);
    TraceNode busy(B);busy.radio.sendStatus=-EBUSY;busy.receive(traceFrame(b,A,B,132,0,true,empty),-3,23000,807);
    for(auto now:{23300u,24000u,25000u})busy.poll(now,808);
    assert(busy.radio.sends==3&&!busy.service.faulted()&&!busy.service.active()&&!busy.service.sent&&!busy.service.endpointControls());
    assert(busy.radio.frames[0]==busy.radio.frames[1]&&busy.radio.frames[1]==busy.radio.frames[2]);
    busy.poll(30000,808);assert(busy.radio.sends==3);
    TraceNode collision(B);meshtastic_RouteDiscovery inconsistent{};
    inconsistent.snr_towards_count=1;inconsistent.snr_towards[0]=4;
    collision.receive(uncheckedRequestFrame(collision,A,B,133,inconsistent),-3,26000,809);
    collision.receive(traceFrame(collision,A,B,133,0,true,empty),-3,26001,810);
    collision.poll(27000,811);assert(collision.radio.sends==0);
    group("duplicate request schedules one response; TX-disabled remains off and CAD-busy admission stops after three identical unaccepted attempts");
  }
  {
    TraceNode a(A);for(uint32_t i=1;i<=4;++i)a.request(200+i,B,3);a.request(205,B,3);
    bool limited=false;for(auto &out:a.usbHost.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&
      out.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP&&out.packet.decoded.request_id==205)limited=true;
    assert(limited&&a.service.endpointFree()==0);
    TraceNode off(A,false);off.request(300,B,3);assert(!off.radio.sends);
    group("origin queue and compile-time TX gate refuse without fabricating traceroute completion");
  }
  {
    TraceNode a(A);auto source=localTraceRequest(350,B,3);source.packet.decoded.source=A;
    input(a.usb,source,100);assert(a.service.endpointFree()==4&&!a.radio.sends&&results(a.usbHost,350,meshtastic_Routing_Error_BAD_REQUEST)==1);
    auto dest=localTraceRequest(351,B,3);dest.packet.decoded.dest=B;
    input(a.ble,dest,101);assert(a.service.endpointFree()==4&&!a.radio.sends&&results(a.bleHost,351,meshtastic_Routing_Error_BAD_REQUEST)==1);
    auto valid=localTraceRequest(352,B,3);input(a.usb,valid,102);assert(a.service.endpointFree()==3);
    a.poll(400);assert(a.radio.sends==1);
    group("USB and BLE reject caller decoded source or destination without reservation and a fresh canonical request still admits");
  }
  {
    TraceNode a(A);std::vector<uint8_t> unpacked={0x0d,0x22,0x22,0x11,0x11,0x10,0x08};
    rawRequest(a,390,unpacked);a.poll(300);assert(a.radio.sends==1);bench::RouteMetadata m;
    assert(a.service.codec.decode(a.radio.frames[0].data(),a.radio.frames[0].size(),m)&&m.traceRouteValid);
    meshtastic_RouteDiscovery route{};auto stream=pb_istream_from_buffer(m.payload,m.payloadLength);
    assert(pb_decode(&stream,meshtastic_RouteDiscovery_fields,&route)&&route.route_count==1&&route.route[0]==H1&&
      route.snr_towards_count==1&&route.snr_towards[0]==8);
    TraceNode negative(A);std::vector<uint8_t> canonicalNegative={0x0d,0x22,0x22,0x11,0x11,0x10,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x01};
    rawRequest(negative,391,canonicalNegative);negative.poll(300);assert(negative.radio.sends==1);
    group("actual request path accepts schema-compatible unpacked repeated route and SNR fields");
  }
  {
    TraceNode a(A);meshtastic_ToRadio bad{};bad.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=bad.packet;
    p.id=400;p.to=B;p.hop_limit=3;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_TRACEROUTE_APP;
    p.decoded.want_response=true;p.decoded.payload.size=2;p.decoded.payload.bytes[0]=0x28;p.decoded.payload.bytes[1]=1;
    input(a.usb,bad,100);assert(a.service.endpointFree()==4&&!a.radio.sends);
    p.id=401;p.decoded.payload.size=1;p.decoded.payload.bytes[0]=0x0a;input(a.usb,bad,101);assert(a.service.endpointFree()==4);
    p.id=402;p.decoded.payload.size=2;p.decoded.payload.bytes[0]=0x12;p.decoded.payload.bytes[1]=0xff;input(a.usb,bad,102);assert(a.service.endpointFree()==4);
    rawRequest(a,403,{0x10,0x80,0x00});assert(a.service.endpointFree()==4); // overlong zero varint
    rawRequest(a,410,{0x10,0x80,0x80,0x80,0x80,0x10});assert(a.service.endpointFree()==4); // 2^32 overflow
    rawRequest(a,411,{0x10,0xff,0xff,0xff,0xff,0x0f});assert(a.service.endpointFree()==4); // noncanonical int32 -1
    std::vector<uint8_t> tooMany;for(unsigned i=0;i<9;++i){tooMany.push_back(0x0d);for(unsigned j=0;j<4;++j)tooMany.push_back(uint8_t(i>>(8*j)));}
    rawRequest(a,404,tooMany);assert(a.service.endpointFree()==4);
    meshtastic_RouteDiscovery full{};full.route_count=8;for(unsigned i=0;i<8;++i)full.route[i]=H1+i;
    a.request(405,B,3,full);assert(a.service.endpointFree()==4);
    meshtastic_RouteDiscovery inconsistent{};inconsistent.snr_towards_count=1;inconsistent.snr_towards[0]=4;
    a.request(406,B,3,inconsistent);assert(a.service.endpointFree()==4);
    rawRequest(a,407,{},0xffffffffu);rawRequest(a,408,{},B,3,1);rawRequest(a,409,{},B,3,0,true);
    assert(a.service.endpointFree()==4&&!a.radio.sends);
    group("unknown, truncated, overlong, over-count, broadcast, secondary-channel and private-shaped requests reject without mutation");
  }
  {
    TraceNode destination(B);meshtastic_RouteDiscovery empty{};unsigned sends=0;
    for(uint32_t i=0;i<16;++i) {
      auto frame=traceFrame(destination,A+i,B,600+i,0,true,empty);
      const uint32_t now=1000+i*1000;destination.receive(frame,-1,now,2000+i);destination.poll(now+300,3000+i);
      assert(destination.radio.sends==int(++sends));destination.done(now+301);
    }
    auto overflow=traceFrame(destination,A+16,B,616,0,true,empty);
    destination.receive(overflow,-1,18000,4000);destination.poll(18300,4001);assert(destination.radio.sends==int(sends));
    auto earliest=traceFrame(destination,A,B,600,0,true,empty);
    destination.receive(earliest,-1,590999,4002);destination.poll(591299,4003);assert(destination.radio.sends==int(sends));
    destination.receive(earliest,-1,601000,4004);destination.poll(601300,4005);assert(destination.radio.sends==int(++sends));destination.done(601301);

    const uint32_t born=0xffff0000u;TraceNode wrapped(B,true,born-1000);
    auto wrap=traceFrame(wrapped,A,B,700,0,true,empty);wrapped.receive(wrap,-1,born,5000);wrapped.poll(born+300,5001);
    assert(wrapped.radio.sends==1);wrapped.done(born+301);
    wrapped.receive(wrap,-1,born+599999,5002);wrapped.poll(born+600299,5003);assert(wrapped.radio.sends==1);
    wrapped.receive(wrap,-1,born+600000,5004);wrapped.poll(born+600300,5005);assert(wrapped.radio.sends==2);
    group("all 16 request replay slots fail closed at capacity and expire or reuse safely across uint32 rollover");
  }
  {
    TraceNode destination(B);meshtastic_RouteDiscovery maximum{};
    maximum.route_count=maximum.snr_towards_count=7;
    for(unsigned i=0;i<7;++i){maximum.route[i]=H1+i;maximum.snr_towards[i]=-127;}
    auto request=traceFrame(destination,A,B,800,0,true,maximum,uint8_t(7u<<5));bench::RouteMetadata requestMetadata;
    assert(destination.service.codec.decode(request.data(),request.size(),requestMetadata));
    uint8_t normalized[mt::TraceRoute::MaxPayload]{},data[239]{},frame[255]{};size_t normalizedSize=0,dataSize=0,frameSize=0;
    assert(mt::TraceRoute::destination(requestMetadata,0,7,-32,normalized,sizeof(normalized),normalizedSize));
    assert(normalizedSize==112);size_t refused=99;
    assert(!mt::TraceRoute::destination(requestMetadata,0,7,-32,normalized,normalizedSize-1,refused)&&refused==0);
    assert(mt::TraceRoute::responseData(normalized,normalizedSize,800,data,sizeof(data),dataSize)&&dataSize==123);
    refused=99;assert(!mt::TraceRoute::responseData(normalized,normalizedSize,800,data,dataSize-1,refused)&&refused==0);
    assert(destination.service.codec.encodeZeroHopData(B,A,801,data,dataSize,frame,sizeof(frame),frameSize)&&frameSize==139);
    refused=99;assert(!destination.service.codec.encodeZeroHopData(B,A,801,data,dataSize,frame,frameSize-1,refused)&&refused==0);
    destination.receive(request,-32,1000,801);destination.poll(1300,802);assert(destination.radio.sends==1);
    bench::RouteMetadata generated;assert(destination.service.codec.decode(destination.radio.frames[0].data(),destination.radio.frames[0].size(),generated));
    assert(generated.payloadLength==112);

    TraceNode origin(A);origin.request(810,B,3);origin.poll(300);origin.done(301);
    meshtastic_RouteDiscovery asymmetric{};asymmetric.route_count=7;asymmetric.snr_towards_count=8;
    asymmetric.route_back_count=7;asymmetric.snr_back_count=7;
    for(unsigned i=0;i<7;++i){asymmetric.route[i]=H1+i;asymmetric.route_back[i]=H2+i;asymmetric.snr_towards[i]=-127;asymmetric.snr_back[i]=-127;}
    asymmetric.snr_towards[7]=-127;
    auto response=traceFrame(origin,B,A,811,810,false,asymmetric,uint8_t(7u<<5));bench::RouteMetadata responseMetadata;
    assert(origin.service.codec.decode(response.data(),response.size(),responseMetadata));
    normalizedSize=0;assert(mt::TraceRoute::origin(responseMetadata,0,7,-32,normalized,sizeof(normalized),normalizedSize));
    assert(normalizedSize==224);refused=99;
    assert(!mt::TraceRoute::origin(responseMetadata,0,7,-32,normalized,normalizedSize-1,refused)&&refused==0);
    assert(mt::TraceRoute::responseData(normalized,normalizedSize,810,data,sizeof(data),dataSize)&&dataSize==236);
    assert(origin.service.codec.encodeZeroHopData(B,A,811,data,dataSize,frame,sizeof(frame),frameSize)&&frameSize==252);
    origin.service.rx(response.data(),response.size(),-32,500,900);assert(origin.service.endpointFree()==4);
    refused=99;assert(!origin.service.takeTraceRoute(0,B,A,811,responseMetadata,500,normalized,223,refused)&&refused==0);
    assert(origin.service.takeTraceRoute(0,B,A,811,responseMetadata,500,normalized,sizeof(normalized),refused)&&refused==224);
    assert(!origin.service.takeTraceRoute(0,B,A,811,responseMetadata,500,normalized,sizeof(normalized),refused));

    TraceNode bounded(A);bounded.request(820,B,3);bounded.poll(300);bounded.done(301);
    meshtastic_RouteDiscovery tooMany=asymmetric;tooMany.route_count=8;tooMany.route[7]=0x55556666;
    auto invalid=traceFrame(bounded,B,A,821,820,false,tooMany);bounded.service.rx(invalid.data(),invalid.size(),-1,500,901);
    assert(bounded.service.endpointFree()==3);
    auto legal=traceFrame(bounded,B,A,822,820,false,directTowards(-127));bounded.service.rx(legal.data(),legal.size(),-1,501,902);
    assert(bounded.service.endpointFree()==4);
    group("actual nanopb, Data and RF boundaries preserve maximum negative-SNR asymmetric payloads and one-shot approvals");
  }
  {
    TraceNode origin(A);auto request=localTraceRequest(850,B,3);input(origin.usb,request,100);input(origin.ble,request,101);
    assert(origin.service.endpointFree()==3);origin.poll(400);assert(origin.radio.sends==1);origin.done(401);
    auto response=traceFrame(origin,B,A,851,850,false,directTowards(-4));origin.receive(response,-2,500);
    assert(traceOutput(origin.usbHost,850,B)&&traceOutput(origin.bleHost,850,B));
    unsigned usbCount=0,bleCount=0;for(const auto &out:origin.usbHost.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.decoded.portnum==70)++usbCount;
    for(const auto &out:origin.bleHost.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.decoded.portnum==70)++bleCount;
    origin.receive(response,-2,501);usbCount=bleCount=0;
    for(const auto &out:origin.usbHost.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.decoded.portnum==70)++usbCount;
    for(const auto &out:origin.bleHost.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.decoded.portnum==70)++bleCount;
    assert(usbCount==1&&bleCount==1);

    auto second=localTraceRequest(860,B,3);input(origin.usb,second,600);input(origin.ble,second,601);origin.poll(900);assert(origin.radio.sends==2);origin.done(901);
    origin.usb.resetStream();auto secondResponse=traceFrame(origin,B,A,861,860,false,directTowards(-8));origin.receive(secondResponse,-3,1000);
    assert(!traceOutput(origin.usbHost,860,B)&&traceOutput(origin.bleHost,860,B));
    meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=2;
    input(origin.usb,hello,1100);origin.usbHost.out.clear();origin.receive(secondResponse,-3,1101);
    assert(!traceOutput(origin.usbHost,860,B));
    group("coalesced USB and BLE owners share one send while disconnect and reconnect remove stale one-shot approval");
  }
  {
    TraceNode a(A);const uint32_t born=0xffff0000u;a.request(500,B,3);a.service.poll(born,0);
    // A separate exact request across wrap demonstrates unsigned expiry. The
    // first request is still queued from its earlier logical timestamp and is
    // intentionally cancelled before establishing this fixture boundary.
    assert(a.service.armEndpoint(false));assert(a.service.armEndpoint(true));
    meshtastic_ToRadio in{};in.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=in.packet;
    p.id=501;p.to=B;p.hop_limit=3;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_TRACEROUTE_APP;p.decoded.want_response=true;
    input(a.usb,in,born);a.service.poll(born+300,0);a.done(born+301);auto route=directTowards(0);
    auto late=traceFrame(a,B,A,900,501,false,route);a.receive(late,0,born+90001);assert(!traceOutput(a.usbHost,501,B));
    group("wrap-safe absolute expiry refuses a late application response");
  }
  std::cout<<"PASS traceroute suite: 22 semantic groups; virtual radios only, no hardware or RF\n";
}
