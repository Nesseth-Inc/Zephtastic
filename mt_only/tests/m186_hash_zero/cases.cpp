// SPDX-License-Identifier: GPL-3.0-only
// Focused computed keyed-hash-zero regression cases against frozen production
// Config, PrimaryChannel, MtCodec, Service, Endpoint and RoutingCore code.
#include <cerrno>
// EKEYREJECTED is the Zephyr/Linux errno extension (129); Apple's libc omits it.
#ifndef EKEYREJECTED
#define EKEYREJECTED 129
#endif
#include "Companion.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
constexpr uint32_t Self=0x12345678u,Peer=0x24680135u,Next=0x33445566u;
static void group(const char *name){std::cout<<"PASS "<<name<<'\n';}
static uint32_t read32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static void write32(uint8_t *p,uint32_t value){for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));}

struct ZeroKey {uint8_t bytes[16]{};};
static ZeroKey keyForNameHashZero(const char *name){
  ZeroKey key;uint8_t nameXor=0;for(size_t i=0;name[i];++i)nameXor^=uint8_t(name[i]);
  key.bytes[0]=nameXor;return key;
}
static mt::Config keyedZeroConfig(const ZeroKey &key){
  mt::Config cfg;cfg.bytes[4]=1;
  // Keep the donor's actual LongFast name. The test derives key[0] from its
  // bytes; no hand-authored zero-hash header is used as setup evidence.
  auto record=cfg.node();record.setKey(0,key.bytes,sizeof(key.bytes));cfg.node(record);cfg.seal();
  mt::PrimaryChannel::Selection selected{};
  assert(cfg.valid()&&cfg.provisioned()&&cfg.channelConfigured());
  assert(mt::PrimaryChannel::normalize(key.bytes,sizeof(key.bytes),selected,"LongFast"));
  assert(selected.hash==0&&selected.length==sizeof(key.bytes)&&!selected.licensedPlaintext);
  mt::PrimaryChannel::clear(selected);return cfg;
}
static std::vector<uint8_t> canonicalText(){
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
  data.has_bitfield=true;data.payload.size=4;memcpy(data.payload.bytes,"zero",4);
  uint8_t out[239]{};auto stream=pb_ostream_from_buffer(out,sizeof(out));
  assert(pb_encode(&stream,meshtastic_Data_fields,&data));return {out,out+stream.bytes_written};
}
static std::vector<uint8_t> encryptedUnicast(const ZeroKey &key,const std::vector<uint8_t> &data,
                                             uint32_t from=Peer,uint32_t to=Self,uint32_t id=41){
  bench::MtCodec codec;assert(codec.setKey(0,key.bytes,sizeof(key.bytes)));
  uint8_t frame[255]{};size_t n=0;
  assert(codec.encodeZeroHopData(from,to,id,data.data(),data.size(),frame,sizeof(frame),n));
  frame[12]=uint8_t(1u|(1u<<5));return {frame,frame+n};
}

struct Radio final:mt::Radio {
  int state=0;unsigned sends=0;std::vector<std::vector<uint8_t>> frames;
  bool ready()override{return true;}int start()override{state=1;return 0;}
  int stop()override{state=0;return 0;}int restore()override{state=1;return 0;}
  int stopForStorage()override{state=0;return 0;}int health()override{return state;}
  bool busy()override{return false;}uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *p,size_t n)override{++sends;frames.emplace_back(p,p+n);return 0;}
  bool completion(int &result)override{result=0;return false;}
};
struct Probe final:mt::pki::PrivateRuntime {
  uint32_t local=Self;mt::pki::ReceiveClassification next=mt::pki::ReceiveClassification::Hold;
  unsigned classifyCalls=0,ordinaryReceiveCalls=0;
  uint32_t peer()const override{return Peer;}
  bool ready(uint32_t node)const override{return node==local;}
  bool publicReady(uint32_t node)const override{return node==local;}
  bool publicIdentity(uint8_t out[32])const override{memset(out,0x33,32);return true;}
  bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,uint32_t,uint32_t,
                 bool,uint32_t,bench::Frame &)override{return false;}
  void freeze()override{}bool matchesPeer(uint32_t)const override{return false;}
  bool revoke(uint32_t)override{return false;}void clearEvent()override{}
  void sweep(mt::Endpoint &)override{}
  int submit(mt::Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,
      bool,const uint8_t *,size_t,bool,uint32_t)override{return -ENOTSUP;}
  void receive(mt::Endpoint &,bench::MtCodec &,const uint8_t *,size_t,uint32_t,uint32_t,bool)override{
    ++ordinaryReceiveCalls;
  }
  mt::pki::ReceiveClassification receiveClassified(mt::Endpoint &,bench::MtCodec &,
      const uint8_t *,size_t,uint32_t,uint32_t,bool)override{++classifyCalls;return next;}
  void proof(mt::Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t)override{}
  bool event(uint8_t,const uint8_t *,size_t,mt::pki::PrivateEvent &)override{return false;}
};
struct Host final:mt::Companion::Host {
  std::vector<meshtastic_FromRadio> rows;
  bool ready()const override{return true;}int save(const mt::Config &)override{return -ENOTSUP;}
  bool send(const uint8_t *p,size_t n)override{
    if(!p||n<4)return false;meshtastic_FromRadio row{};
    auto stream=pb_istream_from_buffer(p+4,n-4);
    if(!pb_decode(&stream,meshtastic_FromRadio_fields,&row)||stream.bytes_left)return false;
    rows.push_back(row);return true;
  }
  unsigned packets()const {
    unsigned count=0;
    for(const auto &row:rows) {
      if(row.which_payload_variant==meshtastic_FromRadio_packet_tag)++count;
    }
    return count;
  }
};
static void hello(mt::Companion &client){
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
  request.want_config_id=1;uint8_t wire[512]{};auto stream=pb_ostream_from_buffer(wire,sizeof(wire));
  assert(pb_encode(&stream,meshtastic_ToRadio_fields,&request));
  client.packet(wire,stream.bytes_written,1,mt::Companion::WorkspaceProof{true});
}
struct ServiceFixture {
  ZeroKey key=keyForNameHashZero("LongFast");
  Radio radio;Probe probe;mt::test::KnownChannelFixtureClock channelClock;
  mt::Service service{radio,true,Self,81,true,false,&probe};Host usb,ble;
  mt::companion::CompanionWorkspace workspace;
  mt::Companion c{service,usb,81,workspace,0},bc{service,ble,81,workspace,1};
  ServiceFixture(){
    auto cfg=keyedZeroConfig(key);channelClock.bind(service);
    assert(service.apply(cfg)&&service.start(false,0)&&service.armEndpoint(true));
    hello(c);hello(bc);usb.rows.clear();ble.rows.clear();
  }
  void receive(const std::vector<uint8_t> &frame,uint32_t now=100){
    service.rx(frame.data(),frame.size(),-80,now,71);
    c.receive(frame.data(),frame.size(),-80,3,now,mt::Companion::WorkspaceProof{true});
    bc.receive(frame.data(),frame.size(),-80,3,now,mt::Companion::WorkspaceProof{true});
  }
};

static void configAndCodecCases(){
  const auto key=keyForNameHashZero("LongFast");auto cfg=keyedZeroConfig(key);
  mt::Config decoded;assert(mt::Config::decode(cfg.bytes,cfg.Size,decoded)&&cfg.same(decoded));
  mt::PrimaryChannel::Selection selection{};assert(decoded.channelSelection(0,selection));
  assert(selection.hash==0&&!selection.licensedPlaintext&&decoded.channelCandidates(0)==1);
  mt::PrimaryChannel::clear(selection);

  auto data=canonicalText();bench::MtCodec unconfigured;
  uint8_t wire[255]{};size_t length=0;
  assert(!unconfigured.encodePositionQuery(Self,Peer,42,wire,sizeof(wire),length)&&length==0);
  bench::MtCodec encrypted;assert(encrypted.setKey(0,key.bytes,sizeof(key.bytes))&&
      encrypted.hasEncryptedKey()&&encrypted.usableConfigured()&&!encrypted.licensedPlaintext());
  assert(encrypted.encodeTelemetryQuery(Self,Peer,43,wire,sizeof(wire),length)&&wire[13]==0);
  bench::RouteMetadata metadata{};
  assert(!encrypted.decode(wire,length,metadata)&&!metadata.channelDecoded);
  assert(encrypted.decode(wire,length,metadata,true)&&metadata.channelDecoded&&
      metadata.portnum==meshtastic_PortNum_TELEMETRY_APP&&metadata.wantResponse&&read32(wire+8)==43);
  auto frame=encryptedUnicast(key,data);
  assert(encrypted.decode(frame.data(),frame.size(),metadata,true)&&metadata.channelDecoded&&
      metadata.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP&&metadata.payloadLength==4);
  assert(!encrypted.decode(frame.data(),frame.size(),metadata));

  bench::MtCodec licensed;assert(licensed.setLicensedPlaintext(0)&&licensed.usableConfigured()&&
      !licensed.hasEncryptedKey()&&licensed.licensedPlaintext());
  uint8_t plainFrame[255]{};size_t plainLength=0;
  assert(licensed.encodeZeroHopData(Peer,Self,44,data.data(),data.size(),plainFrame,
      sizeof(plainFrame),plainLength));plainFrame[12]=uint8_t(1u|(1u<<5));
  assert(licensed.decode(plainFrame,plainLength,metadata)&&metadata.channelDecoded&&
      metadata.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP);
  bench::MtCodec empty;assert(!empty.usableConfigured()&&!empty.hasEncryptedKey()&&!empty.licensedPlaintext());
  assert(!empty.decode(frame.data(),frame.size(),metadata));
  group("computed LongFast key hash zero roundtrips; encrypted, licensed plaintext and unconfigured modes stay distinct; codec zero-unicast opt-in is explicit");
}

static void signatureResolvedChannelContextCases(){
  ServiceFixture f;const auto payload=canonicalText();
  const auto frame=encryptedUnicast(f.key,payload);

  // A standalone codec opt-in has no selected-channel proof. The verifier's
  // default must reject, and even an explicit caller opt-in cannot promote
  // metadata that lacks Service's resolved channel context.
  bench::RouteMetadata unowned{};
  assert(f.service.codec.decode(frame.data(),frame.size(),unowned,true)&&
         unowned.channelDecoded&&!unowned.channelContext);
  bench::MtSignature verifier;
  assert(verifier.apply(frame.data(),frame.size(),unowned,100)==bench::MtSignature::Result::Rejected&&
         unowned.signatureRejected&&!unowned.signatureVerified);
  assert(verifier.apply(frame.data(),frame.size(),unowned,100,true)==bench::MtSignature::Result::Rejected&&
         unowned.signatureRejected&&!unowned.signatureVerified);

  // Service assigns channelContext only after a unique configured-key decode.
  // With the explicit owner authorization, the verifier classifies this
  // unsigned frame as unsigned; it never labels it verified or PKI-decrypted.
  bench::RouteMetadata resolved{};unsigned channel=99;
  assert(f.service.decodeChannel(frame.data(),frame.size(),resolved,channel,true)&&
         channel==0&&resolved.channelDecoded&&resolved.channelContext==1);
  assert(verifier.apply(frame.data(),frame.size(),resolved,100,true)==bench::MtSignature::Result::Unsigned&&
         !resolved.signatureRejected&&!resolved.signatureVerified&&!resolved.signatureLength);
  group("hash-zero unicast signature verifier requires explicit owner opt-in plus Service-resolved channel context");
}

static void servicePkiFirstAndCompanionFanout(){
  for(const auto outcome:{mt::pki::ReceiveClassification::AuthenticatedConsumed,
                          mt::pki::ReceiveClassification::Hold,
                          mt::pki::ReceiveClassification::Unauthenticated}){
    ServiceFixture f;f.probe.next=outcome;const auto payload=canonicalText();
    auto frame=encryptedUnicast(f.key,payload);f.receive(frame);
    assert(f.probe.classifyCalls==1&&f.probe.ordinaryReceiveCalls==0);
    if(outcome==mt::pki::ReceiveClassification::Unauthenticated){
      if(f.usb.packets()!=1||f.ble.packets()!=1){
        bench::RouteMetadata m{};unsigned slot=0;
        const bool shared=f.service.sharedChannelZeroReceive(frame.data(),frame.size());
        const bool decoded=f.service.decodeChannel(frame.data(),frame.size(),m,slot);
        std::cerr<<"FANOUT receiving="<<f.service.receiving()<<" shared="<<shared
          <<" decoded="<<decoded<<" channel="<<slot<<" context="<<m.channelContext
          <<" ambiguous="<<m.channelAmbiguous<<" port="<<m.portnum
          <<" usbRows="<<f.usb.rows.size()<<" bleRows="<<f.ble.rows.size()
          <<" usbPackets="<<f.usb.packets()<<" blePackets="<<f.ble.packets()
          <<" configured="<<unsigned(f.service.config.channelCandidates(0))
          <<" health="<<f.radio.health()
          <<" received="<<f.service.received<<" rejected="<<f.service.rejected<<'\n';
        bench::RouteMetadata candidate{};unsigned candidateSlot=0;
        std::cerr<<"EXPLICIT decode="<<f.service.decodeChannel(frame.data(),frame.size(),candidate,candidateSlot,true)
          <<" backendFailed="<<candidate.backendFailed<<" decodeRejected="<<candidate.decodeRejected
          <<" port="<<candidate.portnum<<" context="<<unsigned(candidate.channelContext)<<'\n';
      }
      assert(f.usb.packets()==1&&f.ble.packets()==1);
      const auto &u=f.usb.rows.back().packet;const auto &b=f.ble.rows.back().packet;
      assert(u.from==Peer&&b.from==Peer&&u.id==41&&b.id==41&&
             u.channel==0&&b.channel==0&&!u.pki_encrypted&&!b.pki_encrypted);
    }else assert(f.usb.packets()==0&&f.ble.packets()==0);
  }
  group("actual Service classifies hash-zero local unicast PKI-first; consumed/held results never fall through and only healthy unauthenticated negative reaches exact USB/BLE shared fanout");
  ServiceFixture f;f.probe.next=mt::pki::ReceiveClassification::Unauthenticated;
  const auto payload=canonicalText();auto corrupt=encryptedUnicast(f.key,payload);corrupt.back()^=0x80;
  f.receive(corrupt);assert(f.probe.classifyCalls==1&&f.usb.packets()==0&&f.ble.packets()==0);
  group("PKI-negative malformed shared ciphertext is not reinterpreted as local packet delivery");
}

static void endpointAndRoutingCases(){
  mt::Endpoint endpoint;uint8_t localFrame[20]{};auto put=[&](unsigned at,uint32_t v){
    for(unsigned i=0;i<4;++i)localFrame[at+i]=uint8_t(v>>(8*i));};
  put(0,Peer);put(4,Self);put(8,700);localFrame[12]=uint8_t(2|(2<<5));
  localFrame[13]=0;localFrame[15]=uint8_t(Self);localFrame[16]=0x08;localFrame[17]=0x01;
  assert(endpoint.submit(0,localFrame,18,false,1,true,true,true,nullptr,false,
      false,false,false,true)==0);
  assert(endpoint.repeatSignedNodeInfo(0,700,Peer,2,false,true,2,true,0)==0);
  assert(endpoint.repeatSignedNodeInfo(0,700,Peer,2,false,true,3,true,1)==-EINVAL);
  group("Endpoint explicit expected-hash presence matches a legitimate signed job at hash zero and rejects a nonzero mismatch");

  bench::RoutingCore route;route.configureContinuous(uint8_t(Self),8);route.configureIdentity(Self);
  const uint8_t zero=0;assert(route.setChannelHashes(&zero,1));
  uint8_t transit[18]{};write32(transit,Next);write32(transit+4,Peer);write32(transit+8,801);
  transit[12]=uint8_t(2|(2<<5));transit[13]=0;transit[16]=0x08;transit[17]=0x01;
  bench::RouteMetadata m{};m.channelDecoded=true;m.channelContext=1;m.portnum=1;m.encodedDataSize=2;
  assert(route.ingestWithMetadata(bench::Protocol::Meshtastic,transit,sizeof(transit),m,100,0));
  assert(route.stats.accepted==1&&route.routingStats.decoded==1&&route.routingStats.opaque==0);
  // Clearing route/history ownership deliberately preserves lifetime counters.
  const auto accepted=route.stats.accepted;
  route.clearRoutingState();m.channelContext=0;
  assert(!route.ingestWithMetadata(bench::Protocol::Meshtastic,transit,sizeof(transit),m,200,0));
  assert(route.stats.accepted==accepted&&route.routingStats.opaque==0);
  route.clearRoutingState();m.channelContext=1;m.channelAmbiguous=true;
  const auto rejected=route.routingStats.decodeRejected;
  assert(!route.ingestWithMetadata(bench::Protocol::Meshtastic,transit,sizeof(transit),m,300,0));
  assert(route.stats.accepted==accepted&&route.routingStats.opaque==0&&route.routingStats.decodeRejected==rejected+1);
  group("RoutingCore explicit configured hash set accepts zero only with selected channel context; missing context and ambiguous-key metadata are rejected before opaque relay");
}
} // namespace

int main(){
#ifdef NDEBUG
#error Regression cases require active assertions.
#endif
  std::cout.setf(std::ios::unitbuf);
  configAndCodecCases();signatureResolvedChannelContextCases();
  servicePkiFirstAndCompanionFanout();endpointAndRoutingCases();
}
