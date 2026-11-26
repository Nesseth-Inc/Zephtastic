// SPDX-License-Identifier: GPL-3.0-only
// Public-unicast traceroute transit through the production normal Service.
// All radios are inert memory fixtures; this file opens no transport or device.
#include "endpoint_fixture.h"
#include "pki/PkiSigner.h"
#include "monocypher.h"

static constexpr uint32_t ORIGIN=0x12345678,RELAY=0x55667788,DEST=0x87654321;

struct TransitNode {
  uint64_t channelNow=0;
  Radio radio;mt::Service service;Host host;mt::companion::CompanionWorkspace workspace;mt::Companion companion;
  explicit TransitNode(uint32_t id,bool tx=true,uint32_t start=0):service(radio,tx,id,81,true,false),host(service),companion(service,host,81,workspace,0) {
    assert(service.channel_.bind({Fixture::channelClock,&channelNow}));
    service.channel_.supported(true);
    mt::Config config;config.bytes[4]=1;auto node=config.node();node.setPublic();config.node(node);assert(config.setDeviceRole(2,0));
    assert(service.apply(config)&&service.start(false,start));if(tx)assert(service.armEndpoint(true));
    meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=1;
    input(companion,hello,start+1);host.out.clear();
  }
  void request(uint32_t packet,uint32_t to,unsigned hop,const uint8_t *payload=nullptr,size_t size=0) {
    meshtastic_ToRadio in{};in.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=in.packet;
    p.id=packet;p.to=to;p.hop_limit=hop;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_TRACEROUTE_APP;p.decoded.want_response=true;
    p.decoded.payload.size=size;if(size)memcpy(p.decoded.payload.bytes,payload,size);input(companion,in,100);
  }
  void receive(const std::vector<uint8_t> &frame,int8_t snr,uint32_t now,uint32_t random=0) {
    service.rx(frame.data(),frame.size(),snr,now,random);
    companion.receive(frame.data(),frame.size(),-90,snr,now,mt::Companion::WorkspaceProof{true});
  }
  void poll(uint32_t now,uint32_t random=0){service.poll(now,random);companion.poll(mt::Companion::WorkspaceProof{true});}
  void done(uint32_t now){radio.done=true;service.poll(now,0);companion.poll(mt::Companion::WorkspaceProof{true});}
};

static meshtastic_RouteDiscovery decodeRoute(TransitNode &node,const std::vector<uint8_t> &frame,bench::RouteMetadata *metadata=nullptr) {
  bench::RouteMetadata decoded{};assert(node.service.codec.decode(frame.data(),frame.size(),decoded));
  assert(decoded.portnum==meshtastic_PortNum_TRACEROUTE_APP&&decoded.traceRouteValid);
  meshtastic_RouteDiscovery route{};auto in=pb_istream_from_buffer(decoded.payload,decoded.payloadLength);
  assert(pb_decode(&in,meshtastic_RouteDiscovery_fields,&route)&&in.bytes_left==0);
  if(metadata)*metadata=decoded;
  return route;
}

static void writeLe32(uint8_t *p,uint32_t value){for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));}

static std::vector<uint8_t> encodedFrame(TransitNode &node,uint32_t from,uint32_t to,uint32_t id,
  unsigned hop,unsigned start,const meshtastic_RouteDiscovery &route,bool response=false,uint32_t request=0,
  uint8_t next=0,uint8_t relay=0) {
  uint8_t payload[mt::TraceRoute::MaxPayload]{},data[239]{},frame[255]{};size_t payloadSize=0,dataSize=0,frameSize=0;
  auto payloadOut=pb_ostream_from_buffer(payload,sizeof(payload));assert(pb_encode(&payloadOut,meshtastic_RouteDiscovery_fields,&route));
  payloadSize=payloadOut.bytes_written;
  assert(response?mt::TraceRoute::responseData(payload,payloadSize,request,data,sizeof(data),dataSize):
                  mt::TraceRoute::requestData(payload,payloadSize,data,sizeof(data),dataSize));
  assert(response?node.service.codec.encodeZeroHopData(from,to,id,data,dataSize,frame,sizeof(frame),frameSize):
                  node.service.codec.encodeTraceRouteRequest(from,to,id,data,dataSize,frame,sizeof(frame),frameSize));
  frame[12]=uint8_t(hop|(start<<5));frame[14]=next;frame[15]=relay;return {frame,frame+frameSize};
}

static std::vector<uint8_t> rawDataFrame(uint32_t from,uint32_t to,uint32_t id,
  unsigned hop,unsigned start,const uint8_t *data,size_t size,uint8_t channel=8) {
  assert(data&&size&&size<=239);std::vector<uint8_t> frame(size+16);
  writeLe32(frame.data(),to);writeLe32(frame.data()+4,from);writeLe32(frame.data()+8,id);
  frame[12]=uint8_t(hop|(start<<5));frame[13]=channel;frame[15]=uint8_t(from);memcpy(frame.data()+16,data,size);
  uint8_t nonce[16]{};bench::MtCodec::nonce(from,id,nonce);
  assert(bench::mtAesCtr(nullptr,bench::MtNodeRecord::PublicKey,16,nonce,frame.data()+16,size));return frame;
}

static bool traceOutput(const Host &host,uint32_t request,meshtastic_RouteDiscovery &route) {
  for(const auto &out:host.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&
    out.packet.decoded.portnum==meshtastic_PortNum_TRACEROUTE_APP&&out.packet.decoded.request_id==request) {
    auto in=pb_istream_from_buffer(out.packet.decoded.payload.bytes,out.packet.decoded.payload.size);
    return pb_decode(&in,meshtastic_RouteDiscovery_fields,&route)&&in.bytes_left==0;
  }
  return false;
}

struct CryptPlan {unsigned calls=0,failAt=0;};
static bool countedCrypt(void *context,const uint8_t *key,size_t keySize,const uint8_t *nonce,uint8_t *bytes,size_t size) {
  auto &plan=*static_cast<CryptPlan *>(context);++plan.calls;
  return plan.calls!=plan.failAt&&bench::mtAesCtr(nullptr,key,keySize,nonce,bytes,size);
}

struct SyntheticKeys:mt::pki::KeyProvider {
  uint8_t scalar[32]{};mt::pki::IdentityBinding binding;mt::pki::KeyRef ref{1,1};
  SyntheticKeys() {
    for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(17+i*13);
    scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;binding.generation=1;
    crypto_x25519_public_key(binding.publicKey,scalar);binding.node=bench::MtSignature::nodeId(binding.publicKey);
  }
  bool active(mt::pki::KeyRef value)const override{return value.slot==ref.slot&&value.generation==ref.generation;}
  bool withPrivate(mt::pki::KeyRef value,mt::pki::PrivateKeyOperation &operation)override {
    return active(value)&&operation.apply(scalar);
  }
};
struct SyntheticEntropy:mt::pki::SigningEntropy {
  bool hedge(uint8_t out[32])override{for(unsigned i=0;i<32;++i)out[i]=uint8_t(91+i*7);return true;}
};
static std::vector<uint8_t> signedNodeInfoFrame(TransitNode &node,SyntheticKeys &keys,uint32_t to,uint32_t id) {
  SyntheticEntropy entropy;mt::pki::NodeInfoProfile profile;strcpy(profile.longName,"Synthetic transit signer");strcpy(profile.shortName,"SIGN");profile.hardwareModel=81;
  mt::pki::SigningEnvelope envelope;envelope.hasBitfield=true;mt::pki::SignedData signedData;
  assert(mt::pki::PkiSigner::bootstrap(keys,keys.ref,keys.binding,entropy,id,to,profile,envelope,signedData)==mt::pki::SigningResult::Ok);
  uint8_t frame[255]{};size_t size=0;
  assert(node.service.codec.encodeSignedNodeInfo(signedData.from,to,id,signedData.bytes,signedData.size,frame,sizeof(frame),size));
  frame[12]=uint8_t((3<<5)|3);frame[15]=uint8_t(signedData.from);return {frame,frame+size};
}

static std::vector<uint8_t> routeData(const meshtastic_RouteDiscovery &route,bool response=false,uint32_t request=0) {
  uint8_t payload[mt::TraceRoute::MaxPayload]{},data[239]{};size_t dataSize=0;
  auto output=pb_ostream_from_buffer(payload,sizeof(payload));assert(pb_encode(&output,meshtastic_RouteDiscovery_fields,&route));
  assert(response?mt::TraceRoute::responseData(payload,output.bytes_written,request,data,sizeof(data),dataSize):
                  mt::TraceRoute::requestData(payload,output.bytes_written,data,sizeof(data),dataSize));
  return {data,data+dataSize};
}

int main() {
  (void)&results;
  {
    TransitNode origin(ORIGIN),relay(RELAY),destination(DEST);
    origin.request(100,DEST,3);origin.poll(300);assert(origin.radio.sends==1);origin.done(301);
    relay.receive(origin.radio.frames[0],-5,500);relay.poll(501);assert(relay.radio.sends==1);relay.done(502);
    assert((relay.radio.frames[0][12]&7)==2&&relay.radio.frames[0][15]==uint8_t(RELAY));
    auto forward=decodeRoute(relay,relay.radio.frames[0]);
    assert(forward.route_count==1&&forward.route[0]==RELAY&&forward.snr_towards_count==1&&forward.snr_towards[0]==-20);
    destination.receive(relay.radio.frames[0],-7,700);destination.poll(1000);assert(destination.radio.sends==1);destination.done(1001);
    relay.receive(destination.radio.frames[0],-8,1200);relay.poll(1201);assert(relay.radio.sends==2);relay.done(1202);
    auto back=decodeRoute(relay,relay.radio.frames[1]);
    assert(back.route_count==1&&back.route[0]==RELAY&&back.snr_towards_count==2&&back.snr_towards[0]==-20&&back.snr_towards[1]==-28);
    assert(back.route_back_count==1&&back.route_back[0]==RELAY&&back.snr_back_count==1&&back.snr_back[0]==-32);
    origin.receive(relay.radio.frames[1],-9,1400);meshtastic_RouteDiscovery result{};
    assert(traceOutput(origin.host,100,result)&&result.route_back_count==1&&result.snr_back_count==2&&result.snr_back[1]==-36);
    assert(!relay.service.nodes.count()&&!relay.service.route.routeCount());
    group("production origin, relay, destination, return relay and origin complete a correlated Public trace without trust learning");
  }
  {
    TransitNode relay(RELAY);meshtastic_RouteDiscovery request{};
    request.route_count=1;request.route[0]=0x11112222;request.snr_towards_count=1;request.snr_towards[0]=12;
    auto frame=encodedFrame(relay,ORIGIN,DEST,200,1,3,request);relay.receive(frame,-40,1000);relay.poll(1001);
    assert(relay.radio.sends==1);auto route=decodeRoute(relay,relay.radio.frames[0]);
    assert(route.route_count==3&&route.route[0]==0x11112222&&route.route[1]==0xffffffffu&&route.route[2]==RELAY);
    assert(route.snr_towards_count==3&&route.snr_towards[0]==12&&route.snr_towards[1]==-128&&route.snr_towards[2]==-127);
    relay.done(1002);
    meshtastic_RouteDiscovery response{};response.route_count=3;response.route[0]=0x11112222;response.route[1]=0xffffffffu;response.route[2]=RELAY;
    response.snr_towards_count=4;response.snr_towards[0]=12;response.snr_towards[1]=-128;response.snr_towards[2]=-127;response.snr_towards[3]=4;
    response.route_back_count=1;response.route_back[0]=0x33334444;response.snr_back_count=1;response.snr_back[0]=-8;
    auto reverse=encodedFrame(relay,DEST,ORIGIN,201,1,3,response,true,200);relay.receive(reverse,40,2000);relay.poll(2001);
    assert(relay.radio.sends==2);auto returned=decodeRoute(relay,relay.radio.frames[1]);
    assert(returned.route_back_count==3&&returned.route_back[0]==0x33334444&&returned.route_back[1]==0xffffffffu&&returned.route_back[2]==RELAY);
    assert(returned.snr_back_count==3&&returned.snr_back[0]==-8&&returned.snr_back[1]==-128&&returned.snr_back[2]==127);
    group("pre-populated asymmetric paths preserve the opposite direction and fill pinned unknown hop and q4 SNR positions");
  }
  {
    TransitNode relay(RELAY);const uint8_t unpackedRoute[]={0x0d,0x22,0x22,0x11,0x11,0x10,0x08};
    uint8_t data[32]{};size_t dataSize=0;bench::RouteMetadata shape{};shape.portnum=70;shape.traceRouteValid=true;
    shape.wantResponse=true;shape.hasBitfield=true;shape.payloadLength=sizeof(unpackedRoute);memcpy(shape.payload,unpackedRoute,sizeof(unpackedRoute));
    assert(mt::TraceRoute::data(shape,shape.payload,shape.payloadLength,data,sizeof(data),dataSize));
    auto frame=rawDataFrame(ORIGIN,DEST,300,3,3,data,dataSize);relay.receive(frame,-3,1000);relay.poll(1001);
    assert(relay.radio.sends==1);auto route=decodeRoute(relay,relay.radio.frames[0]);
    assert(route.route_count==2&&route.route[0]==0x11112222&&route.route[1]==RELAY&&route.snr_towards_count==2&&route.snr_towards[0]==8);
    std::vector<uint8_t> noncanonical(data,data+dataSize);noncanonical.push_back(0x48);noncanonical.push_back(0x00);
    auto duplicateOuter=rawDataFrame(ORIGIN+1,DEST,301,3,3,noncanonical.data(),noncanonical.size());
    relay.receive(duplicateOuter,-3,1100);relay.poll(1101);assert(relay.radio.sends==1);
    group("schema-compatible unpacked inner arrays relay while duplicate noncanonical outer Data is refused unchanged");
  }
  {
    TransitNode relay(RELAY);meshtastic_RouteDiscovery maximum{};
    maximum.route_count=7;maximum.snr_towards_count=8;
    maximum.route_back_count=maximum.snr_back_count=6;
    for(unsigned i=0;i<7;++i)maximum.route[i]=0x10000000u+i;
    for(unsigned i=0;i<8;++i)maximum.snr_towards[i]=i&1?-127:-128;
    for(unsigned i=0;i<6;++i){maximum.route_back[i]=0x20000000u+i;maximum.snr_back[i]=i&1?-128:-127;}
    auto frame=encodedFrame(relay,DEST,ORIGIN,400,3,3,maximum,true,0x33445566);
    bench::RouteMetadata metadata{};assert(relay.service.codec.decode(frame.data(),frame.size(),metadata));
    auto oldData=routeData(maximum,true,0x33445566);
    uint8_t payload[mt::TraceRoute::MaxPayload]{},newData[239]{},rewritten[255]{};
    size_t payloadSize=0,newDataSize=0,rewrittenSize=0;
    assert(mt::TraceRoute::transit(metadata,RELAY,3,3,-32,payload,sizeof(payload),payloadSize)==mt::TraceRoute::TransitResult::Mutated);
    assert(mt::TraceRoute::responseData(payload,payloadSize,0x33445566,newData,sizeof(newData),newDataSize));
    assert(relay.service.codec.rewriteTraceRouteData(frame.data(),frame.size(),oldData.data(),oldData.size(),newData,newDataSize,
                                                     rewritten,sizeof(rewritten),rewrittenSize));
    size_t refusedSize=99;uint8_t shortFrame[255]{};
    assert(!relay.service.codec.rewriteTraceRouteData(frame.data(),frame.size(),oldData.data(),oldData.size(),newData,newDataSize,
                                                      shortFrame,newDataSize+15,refusedSize)&&refusedSize==0);
    assert(payloadSize==214&&newDataSize==226&&rewrittenSize==242);
    meshtastic_RouteDiscovery over=maximum;over.route_back_count=over.snr_back_count=7;
    over.route_back[6]=0x20000006u;over.snr_back[6]=-127;
    TransitNode admission(RELAY);auto refused=encodedFrame(admission,DEST,ORIGIN,401,3,3,over,true,0x33445566);
    admission.receive(refused,-32,1000);assert(!admission.service.route.pending()&&!admission.service.route.historyCount());
    auto valid=encodedFrame(admission,DEST,ORIGIN,402,3,3,maximum,true,0x33445567);
    admission.receive(valid,-32,1001);assert(admission.service.route.pending()==1&&admission.service.route.historyCount()==1);
    admission.poll(1002);assert(admission.radio.sends==1);auto routed=decodeRoute(admission,admission.radio.frames[0]);
    assert(routed.route_count==7&&routed.snr_towards_count==8&&routed.route_back_count==7&&routed.snr_back_count==7);
    assert(routed.route_back[6]==RELAY&&routed.snr_back[6]==-127);
    group("largest actual asymmetric response encoding, final-frame one-byte-short refusal and later valid Service admission are atomic");
  }
  {
    for(unsigned hops=4;hops<=7;++hops){TransitNode relay(RELAY);meshtastic_RouteDiscovery empty{};
      auto frame=encodedFrame(relay,ORIGIN,DEST,450+hops,hops,hops,empty);
      relay.receive(frame,-1,1000);assert(relay.service.route.pending()==1&&relay.service.route.historyCount()==1);
      relay.poll(1001);assert(relay.radio.sends==1&&(relay.radio.frames[0][12]&7)==hops-1&&(relay.radio.frames[0][12]>>5)==hops);
    }
    group("normal Service and routing preserve every newly admitted four-through-seven hop trace header");
  }
  {
    TransitNode relay(RELAY);meshtastic_RouteDiscovery empty{};
    for(const auto &spec:{std::pair<unsigned,unsigned>{0,3},{2,1},{7,6}}) {
      auto frame=encodedFrame(relay,ORIGIN+spec.first+spec.second,DEST,500+spec.first*10+spec.second,spec.first,spec.second,empty);
      relay.receive(frame,-1,1000+spec.first,0);
    }
    auto directed=encodedFrame(relay,ORIGIN+10,DEST,510,3,3,empty,false,0,0x44);relay.receive(directed,-1,1100);
    assert(!relay.service.route.pending()&&!relay.service.route.historyCount());
    auto broadcast=encodedFrame(relay,ORIGIN+11,DEST,511,3,3,empty);writeLe32(broadcast.data(),0xffffffffu);relay.receive(broadcast,-1,1101);
    auto wrongChannel=encodedFrame(relay,ORIGIN+12,DEST,512,3,3,empty);wrongChannel[13]^=1;relay.receive(wrongChannel,-1,1102);
    auto privateShape=encodedFrame(relay,ORIGIN+13,DEST,513,3,3,empty);privateShape[13]=0;relay.receive(privateShape,-1,1103);
    meshtastic_Data text{};text.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;text.payload.size=1;text.payload.bytes[0]='x';text.has_bitfield=true;
    uint8_t textData[32]{};auto textOut=pb_ostream_from_buffer(textData,sizeof(textData));assert(pb_encode(&textOut,meshtastic_Data_fields,&text));
    auto wrongPort=rawDataFrame(ORIGIN+14,DEST,514,3,3,textData,textOut.bytes_written);relay.receive(wrongPort,-1,1104);
    const uint8_t malformedData[]={0x08,0x46,0x12,0x02,0x0a};auto malformed=rawDataFrame(ORIGIN+15,DEST,515,3,3,malformedData,sizeof(malformedData));
    relay.receive(malformed,-1,1105);
    assert(relay.service.route.pending()==5&&relay.service.route.historyCount()==2&&relay.service.route.routingStats.opaque==3);
    relay.poll(2000);assert(relay.radio.sends==1&&relay.service.route.pending()==4);
    group("invalid-hop-directed-header-refusal-with-separate-decoded-and-opaque-ALL-transit-admission");
  }
  {
    TransitNode off(RELAY,false);meshtastic_RouteDiscovery empty{};auto frame=encodedFrame(off,ORIGIN,DEST,600,3,3,empty);
    off.receive(frame,-1,1000);off.poll(2000);assert(off.radio.sends==0&&!off.service.route.pending());
    TransitNode relay(RELAY);for(unsigned i=0;i<8;++i){auto f=encodedFrame(relay,ORIGIN+i,DEST,610+i,3,3,empty);relay.receive(f,-1,1000+i,0);}
    assert(relay.service.route.pending()==8);auto ninth=encodedFrame(relay,ORIGIN+8,DEST,618,3,3,empty);relay.receive(ninth,-1,1100,0);
    assert(relay.service.route.pending()==8);assert(relay.service.armEndpoint(false));assert(!relay.service.route.pending());relay.poll(2000);assert(relay.radio.sends==0);
    group("compiled TX disable, queue capacity and explicit disarm prevent sends and cancel only queued transit work");
  }
  {
    TransitNode relay(RELAY);meshtastic_RouteDiscovery empty{};
    auto low=encodedFrame(relay,ORIGIN,DEST,690,1,3,empty);relay.receive(low,-1,1000,11);
    assert(relay.service.route.pending()==1&&relay.service.route.historyCount()==1);
    auto changedTo=encodedFrame(relay,ORIGIN,DEST+1,690,2,3,empty);relay.receive(changedTo,-7,1001,0);
    meshtastic_RouteDiscovery changedPayload{};changedPayload.route_count=1;changedPayload.route[0]=0x11112222;
    auto changed=encodedFrame(relay,ORIGIN,DEST,690,2,3,changedPayload);relay.receive(changed,-7,1002,0);
    auto changedChannel=encodedFrame(relay,ORIGIN,DEST,690,2,3,empty);changedChannel[13]^=1;relay.receive(changedChannel,-7,1003,0);
    auto wrongDirection=encodedFrame(relay,ORIGIN,DEST,690,2,3,empty,true,0x1234);relay.receive(wrongDirection,-7,1004,0);
    meshtastic_Data wrongPortData{};wrongPortData.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
    wrongPortData.payload.size=1;wrongPortData.payload.bytes[0]='x';wrongPortData.has_bitfield=true;
    uint8_t wrongPortBytes[32]{};auto wrongPortOut=pb_ostream_from_buffer(wrongPortBytes,sizeof(wrongPortBytes));
    assert(pb_encode(&wrongPortOut,meshtastic_Data_fields,&wrongPortData));
    auto wrongPort=rawDataFrame(ORIGIN,DEST,690,2,3,wrongPortBytes,wrongPortOut.bytes_written);relay.receive(wrongPort,-7,1004,0);
    auto unsupported=routeData(empty);unsupported.push_back(0x38);unsupported.push_back(0x01);
    auto changedMetadata=rawDataFrame(ORIGIN,DEST,690,2,3,unsupported.data(),unsupported.size());relay.receive(changedMetadata,-7,1004,0);
    auto high=encodedFrame(relay,ORIGIN,DEST,690,2,3,empty);relay.receive(high,-7,1005,0);
    assert(relay.service.route.pending()==1&&relay.service.route.stats.upgrades==1);
    // Stock LongFast uses SF11/BW250 and a four-symbol SX126x CAD slot:
    // floor(4.5 * 2048 / 250 + 7.6) = 44 ms. The better copy preserves
    // the first reception's selected 11-slot delay.
    constexpr uint32_t firstDue=1000+11*44;
    relay.poll(firstDue-1);assert(relay.radio.sends==0);
    relay.poll(firstDue);assert(relay.radio.sends==1);
    auto upgraded=decodeRoute(relay,relay.radio.frames[0]);
    assert((relay.radio.frames[0][12]&7)==1&&upgraded.route_count==2&&
           upgraded.route[0]==mt::TraceRoute::UnknownNode&&upgraded.route[1]==RELAY&&
           upgraded.snr_towards_count==2&&upgraded.snr_towards[0]==mt::TraceRoute::UnknownSnr&&
           upgraded.snr_towards[1]==-28);
    relay.done(firstDue+1);relay.receive(relay.radio.frames[0],-9,2410,0);relay.poll(2500);
    assert(relay.radio.sends==1&&!relay.service.route.pending());
    TransitNode expiry(RELAY);auto expiryLow=encodedFrame(expiry,ORIGIN+1,DEST,691,1,3,empty);
    auto expiryHigh=encodedFrame(expiry,ORIGIN+1,DEST,691,2,3,empty);
    expiry.receive(expiryLow,-1,1000,11);expiry.receive(expiryHigh,-7,30000,0);
    assert(expiry.service.route.pending()==1&&expiry.service.route.next(31000)==nullptr&&!expiry.service.route.pending());
    group("trace-specific association upgrades one queued better-hop request while rejecting changed envelopes and downstream loops");
  }
  {
    TransitNode relay(RELAY);meshtastic_RouteDiscovery response{};
    response.route_count=1;response.route[0]=0x11112222;response.snr_towards_count=2;
    response.snr_towards[0]=-8;response.snr_towards[1]=4;
    auto low=encodedFrame(relay,DEST,ORIGIN,695,1,3,response,true,0x44556677);relay.receive(low,-2,1000,0);
    auto high=encodedFrame(relay,DEST,ORIGIN,695,2,3,response,true,0x44556677);relay.receive(high,-8,1001,0);
    relay.poll(1100);assert(relay.radio.sends==1&&relay.service.route.stats.upgrades==1);
    auto upgraded=decodeRoute(relay,relay.radio.frames[0]);
    assert(upgraded.route_count==1&&upgraded.snr_towards_count==2&&upgraded.route_back_count==2&&
           upgraded.route_back[0]==mt::TraceRoute::UnknownNode&&upgraded.route_back[1]==RELAY&&
           upgraded.snr_back[0]==mt::TraceRoute::UnknownSnr&&upgraded.snr_back[1]==-32);
    group("trace-specific association upgrades a correlated asymmetric response direction without changing the opposite route");
  }
  {
    TransitNode relay(RELAY);meshtastic_RouteDiscovery empty{};auto original=encodedFrame(relay,ORIGIN,DEST,700,3,3,empty);
    relay.receive(original,-2,1000,0);relay.receive(original,-2,1001,0);relay.poll(1002);assert(relay.radio.sends==1);
    const auto rewritten=relay.radio.frames[0];relay.done(1003);relay.receive(rewritten,-2,1100,0);relay.poll(1101);
    assert(relay.radio.sends==1);auto route=decodeRoute(relay,rewritten);assert(route.route_count==1&&route.route[0]==RELAY);
    relay.receive(original,-2,2104,0);relay.poll(2105);assert(relay.radio.sends==2);auto retry=decodeRoute(relay,relay.radio.frames[1]);
    assert(retry.route_count==1&&retry.route[0]==RELAY);
    group("duplicates and retransmitted copies rewrite exactly once while a later legitimate origin retry reuses the one-relay payload");
  }
  {
    TransitNode relay(RELAY);meshtastic_RouteDiscovery empty{};auto original=encodedFrame(relay,ORIGIN,DEST,710,3,3,empty);
    relay.receive(original,-2,1000,0);relay.poll(1001);assert(relay.radio.sends==1);relay.done(1002);
    meshtastic_RouteDiscovery loop{};loop.route_count=loop.snr_towards_count=1;loop.route[0]=RELAY;loop.snr_towards[0]=-8;
    auto canonical=encodedFrame(relay,ORIGIN,DEST,710,3,3,loop);relay.receive(canonical,-2,2103,0);
    assert(relay.service.traceLoopObservations==1);
    auto changedDestination=encodedFrame(relay,ORIGIN,DEST+1,710,3,3,loop);relay.receive(changedDestination,-2,2104,0);
    meshtastic_RouteDiscovery responseLoop{};responseLoop.snr_towards_count=1;responseLoop.snr_towards[0]=-4;
    responseLoop.route_back_count=responseLoop.snr_back_count=1;responseLoop.route_back[0]=RELAY;responseLoop.snr_back[0]=-8;
    auto changedDirection=encodedFrame(relay,ORIGIN,DEST,710,3,3,responseLoop,true,0x1234);relay.receive(changedDirection,-2,2105,0);
    assert(relay.service.traceLoopObservations==1);
    auto data=routeData(loop);
    const std::vector<std::vector<uint8_t>> suffixes={{0x25,1,0,0,0},{0x2d,1,0,0,0},{0x48,0},{0x58,1}};
    for(const auto &suffix:suffixes) {
      auto changed=data;changed.insert(changed.end(),suffix.begin(),suffix.end());
      auto frame=rawDataFrame(ORIGIN,DEST,710,3,3,changed.data(),changed.size());relay.receive(frame,-2,2200+unsigned(suffix.size()),0);
    }
    relay.poll(3000);assert(relay.radio.sends==1&&!relay.service.route.pending()&&relay.service.route.historyCount()==1&&
                            relay.service.traceLoopObservations==1);
    group("canonical and unsupported same-ID originator loop copies are validated but remain explicitly non-forwarding");
  }
  {
    TransitNode relay(RELAY);relay.radio.sendStatus=-EBUSY;meshtastic_RouteDiscovery empty{};
    auto frame=encodedFrame(relay,ORIGIN,DEST,800,3,3,empty);relay.receive(frame,-1,1000,0);relay.poll(1001);assert(relay.radio.sends==1);
    relay.radio.sendStatus=0;relay.poll(1499);assert(relay.radio.sends==1);relay.poll(1501);assert(relay.radio.sends==2);
    auto first=decodeRoute(relay,relay.radio.frames[0]),second=decodeRoute(relay,relay.radio.frames[1]);
    assert(first.route_count==1&&second.route_count==1&&first.route[0]==RELAY&&second.route[0]==RELAY);
    relay.done(1502);
    TransitNode restoreFailed(RELAY);restoreFailed.radio.sendStatus=-EBUSY;
    restoreFailed.radio.restoreSequence={-EIO,0};auto restoreFrame=encodedFrame(restoreFailed,ORIGIN,DEST,801,3,3,empty);
    restoreFailed.receive(restoreFrame,-1,2000,0);restoreFailed.poll(2001);
    assert(restoreFailed.service.faulted()&&!restoreFailed.service.receiving()&&restoreFailed.radio.restoreCalls==1&&
           restoreFailed.radio.sends==1&&!restoreFailed.service.route.pending()&&!restoreFailed.service.active());
    TransitNode completionFailed(RELAY);completionFailed.radio.doneStatus=-EIO;
    auto failedFrame=encodedFrame(completionFailed,ORIGIN,DEST,802,3,3,empty);
    completionFailed.receive(failedFrame,-1,3000,0);completionFailed.poll(3001);completionFailed.done(3002);
    assert(completionFailed.service.faulted());
    group("native busy retry rewrites once while first restore or completion failure faults without a masked recovery");
  }
  {
    TransitNode exhausted(RELAY);exhausted.radio.sendStatus=-EBUSY;meshtastic_RouteDiscovery empty{};
    auto frame=encodedFrame(exhausted,ORIGIN,DEST,825,3,3,empty);exhausted.receive(frame,-1,1000,0);
    exhausted.poll(1001);exhausted.poll(1500);assert(exhausted.radio.sends==1);
    exhausted.poll(1501);exhausted.poll(2001);
    assert(exhausted.radio.sends==3&&exhausted.radio.restoreCalls==3&&
           exhausted.service.route.historyAttempts(ORIGIN,825)==3&&
           exhausted.service.route.routingStats.attemptLimit==1&&exhausted.service.route.stats.failures==1&&
           exhausted.service.failed==1&&!exhausted.service.route.pending()&&!exhausted.service.route.retransmitting()&&
           !exhausted.service.route.hasActiveTransmission()&&!exhausted.service.active()&&
           exhausted.service.receiving()&&exhausted.service.endpointArmed()&&!exhausted.service.faulted());
    exhausted.radio.sendStatus=0;meshtastic_ToRadio text{};text.which_payload_variant=meshtastic_ToRadio_packet_tag;
    text.packet.id=826;text.packet.to=DEST;text.packet.hop_limit=3;text.packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    text.packet.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;text.packet.decoded.payload.size=5;
    memcpy(text.packet.decoded.payload.bytes,"after",5);input(exhausted.companion,text,2100);
    exhausted.poll(2300);assert(exhausted.radio.sends==4&&exhausted.service.active());exhausted.done(2401);
    assert(exhausted.service.sent==1&&results(exhausted.host,826,0)==1&&exhausted.service.receiving()&&
           exhausted.service.endpointArmed()&&!exhausted.service.faulted()&&!exhausted.service.active());

    TransitNode expired(RELAY);auto queued=encodedFrame(expired,ORIGIN+1,DEST,827,3,3,empty);
    expired.receive(queued,-1,1000,599);assert(expired.service.route.pending()==1);
    expired.poll(31000);assert(expired.radio.sends==0&&!expired.service.route.pending()&&
      !expired.service.route.retransmitting()&&!expired.service.route.hasActiveTransmission()&&
      expired.service.route.stats.expired==1&&expired.service.receiving()&&
      expired.service.endpointArmed()&&!expired.service.faulted());
    text.packet.id=828;input(expired.companion,text,31100);expired.poll(31300);
    assert(expired.radio.sends==1&&expired.service.active());expired.done(31401);
    assert(results(expired.host,828,0)==1&&expired.service.sent==1&&expired.service.receiving()&&
      expired.service.endpointArmed()&&!expired.service.faulted());
    group("healthy native-busy attempt exhaustion and ordinary queue expiry retire only traceroute work and preserve endpoints");
  }
  {
    TransitNode noTiming(RELAY);noTiming.radio.air=0;meshtastic_RouteDiscovery empty{};
    auto first=encodedFrame(noTiming,ORIGIN,DEST,850,3,3,empty);noTiming.receive(first,-1,1000,0);noTiming.poll(1001);
    assert(noTiming.radio.sends==0&&!noTiming.service.route.pending()&&noTiming.service.route.routingStats.noTiming==1&&!noTiming.service.failed);
    TransitNode excessive(RELAY);excessive.radio.air=262143;auto second=encodedFrame(excessive,ORIGIN,DEST,851,3,3,empty);
    excessive.receive(second,-1,1000,0);excessive.poll(1001);assert(excessive.radio.sends==0&&!excessive.service.route.pending()&&excessive.service.route.routingStats.noTiming==1&&!excessive.service.failed);
    TransitNode longPacket(RELAY);longPacket.radio.air=2501;auto third=encodedFrame(longPacket,ORIGIN,DEST,852,3,3,empty);
    longPacket.receive(third,-1,1000,0);longPacket.poll(1001);assert(longPacket.radio.sends==1&&longPacket.service.active());
    longPacket.done(3502);assert(longPacket.service.sent==1&&!longPacket.service.faulted());
    group("unrepresentable airtime refuses admission and representable long transit completes beyond the old cap");
  }
  {
    meshtastic_RouteDiscovery empty{},appended{};appended.route_count=appended.snr_towards_count=1;
    appended.route[0]=RELAY;appended.snr_towards[0]=-4;
    auto expected=routeData(empty),replacement=routeData(appended);
    TransitNode encoder(RELAY);auto frame=rawDataFrame(ORIGIN,DEST,900,3,3,expected.data(),expected.size());
    uint8_t key[16]{};memcpy(key,bench::MtNodeRecord::PublicKey,16);
    CryptPlan decryptPlan{0,1};bench::MtCodec decryptFailure(countedCrypt,&decryptPlan);
    assert(decryptFailure.setKey(8,key,sizeof(key)));uint8_t out[255];memset(out,0x5a,sizeof(out));size_t size=99;
    assert(!decryptFailure.rewriteTraceRouteData(frame.data(),frame.size(),expected.data(),expected.size(),replacement.data(),replacement.size(),out,sizeof(out),size));
    assert(size==0&&decryptPlan.calls==1);for(auto byte:out)assert(byte==0x5a);
    CryptPlan encryptPlan{0,2};bench::MtCodec encryptFailure(countedCrypt,&encryptPlan);
    assert(encryptFailure.setKey(8,key,sizeof(key)));memset(out,0x5a,sizeof(out));size=99;
    assert(!encryptFailure.rewriteTraceRouteData(frame.data(),frame.size(),expected.data(),expected.size(),replacement.data(),replacement.size(),out,sizeof(out),size));
    assert(size==0&&encryptPlan.calls==2);for(auto byte:out)assert(byte==0);
    auto valid=encodedFrame(encoder,ORIGIN,DEST,901,3,3,empty);encoder.receive(valid,-1,1000);
    assert(encoder.service.route.pending()==1&&encoder.service.route.historyCount()==1);
    meshtastic_Data signedShape{};signedShape.portnum=meshtastic_PortNum_TRACEROUTE_APP;
    signedShape.want_response=true;signedShape.has_bitfield=true;signedShape.xeddsa_signature.size=64;
    uint8_t signedData[239]{};auto encoded=pb_ostream_from_buffer(signedData,sizeof(signedData));
    assert(pb_encode(&encoded,meshtastic_Data_fields,&signedShape));
    auto untrusted=rawDataFrame(ORIGIN+1,DEST,902,3,3,signedData,encoded.bytes_written);encoder.receive(untrusted,-1,1001);
    assert(encoder.service.route.pending()==2&&encoder.service.route.historyCount()==1);
    group("actual canonical decrypt and replacement-encrypt backend failures are invoked, atomic, and do not poison later admission");
  }
  {
    SyntheticKeys keys;TransitNode unknown(RELAY);auto transit=signedNodeInfoFrame(unknown,keys,DEST,920);
    unknown.receive(transit,-1,1000);assert(unknown.service.signature.keyCount(1000)==1&&
      unknown.service.route.pending()==1&&unknown.service.route.historyCount()==1&&
      unknown.service.route.hasHistory(keys.binding.node,920)&&unknown.service.nodes.count()==1);
    const int slot=unknown.service.nodes.find(keys.binding.node);assert(slot>=0);
    const auto *observed=unknown.service.nodes.get(unsigned(slot));assert(observed&&observed->placeholder()&&observed->verifiedSigner());
    assert(observed->bytes[mt::NodeCache::Entry::LocalFlags]==
      (mt::NodeCache::Entry::Placeholder|mt::NodeCache::Entry::VerifiedSigner));
    assert(!observed->bytes[mt::NodeCache::Entry::KeyLength]&&!observed->bytes[mt::NodeCache::Entry::Name]&&
      !observed->favorite()&&!observed->ignored()&&!observed->manuallyVerified());
    uint32_t candidate=0;uint8_t candidateKey[32]{};
    assert(!unknown.service.peekVerifiedPeerCandidate(candidate,candidateKey));
    TransitNode known(RELAY);assert(known.service.signature.trustPublicKey(keys.binding.node,keys.binding.publicKey,100));
    auto refresh=signedNodeInfoFrame(known,keys,DEST,921);const uint32_t refreshedAt=bench::MtSignature::KeyTtlMs-10;
    known.receive(refresh,-1,refreshedAt);
    assert(known.service.signature.keyCount(bench::MtSignature::KeyTtlMs+101)==1);
    assert(known.service.signature.keyCount(refreshedAt+bench::MtSignature::KeyTtlMs+1)==0);
    TransitNode endpoint(RELAY);auto local=signedNodeInfoFrame(endpoint,keys,RELAY,922);endpoint.receive(local,-1,1000);
    assert(endpoint.service.signature.keyCount(1000)==1);
    group("stock signed NodeInfo transit learns an expiring verifier key and signer placeholder without contact or private enrollment");
  }
  {
    TransitNode relay(RELAY,true,0xfffefff0u);meshtastic_RouteDiscovery empty{};auto frame=encodedFrame(relay,ORIGIN,DEST,1000,3,3,empty);
    relay.receive(frame,-1,0xfffffff0u,0);relay.poll(0xfffffff1u);assert(relay.radio.sends==1);relay.done(0xfffffff2u);
    relay.receive(frame,-1,0x00000020u,0);relay.poll(0x00000021u);assert(relay.radio.sends==1);
    relay.receive(frame,-1,0x000f4230u,0);relay.poll(0x000f4231u);assert(relay.radio.sends==2);
    group("duplicate history and expiry remain wrap-safe without treating retransmission scheduling as a fresh rewrite");
  }
  {
    TransitNode relay(RELAY);meshtastic_RouteDiscovery empty{};auto frame=encodedFrame(relay,ORIGIN,DEST,1100,3,3,empty);
    bench::RouteMetadata before{};assert(relay.service.codec.decode(frame.data(),frame.size(),before));
    relay.receive(frame,-1,1000,0);assert(!relay.service.nodes.count()&&!relay.service.route.knownCount()&&!relay.service.route.routeCount());
    relay.poll(1001);assert(relay.radio.sends==1);auto rewritten=decodeRoute(relay,relay.radio.frames[0]);
    assert(before.request_id==0&&rewritten.route_count==1&&!relay.service.nodes.count()&&!relay.service.route.knownCount()&&!relay.service.route.routeCount());
    group("traceroute payload contents and response correlation never create contacts, known peers or next-hop routes");
  }
  std::cout<<"PASS traceroute transit suite: 17 semantic groups; virtual radios only, no hardware or RF\n";
}
