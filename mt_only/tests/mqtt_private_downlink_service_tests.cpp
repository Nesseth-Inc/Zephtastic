// SPDX-License-Identifier: GPL-3.0-only
// Actual Service/Companion wiring with a synthetic PrivateRuntime. This proves
// admission, pause/callback ownership and fanout sequencing, not PKI crypto.
#define CONFIG_MT_MQTT_GATEWAY 1
#include "Service.h"
#include "Companion.h"
#include "MqttServiceEnvelopeCodec.h"
#include "known_channel_fixture.h"
#include "pb_encode.h"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
constexpr uint32_t Self=0x12345678u,Peer=0x24680135u,Other=0x13572468u;
using Bytes=std::vector<uint8_t>;

struct Radio final:mt::Radio {
  int state=0,restoreStatus=0;
  unsigned stopCalls=0,restoreCalls=0;
  bool ready()override{return true;}
  int start()override{state=1;return 0;}
  int restore()override{++restoreCalls;if(!restoreStatus)state=1;return restoreStatus;}
  int stop()override{state=0;return 0;}
  int stopForStorage()override{++stopCalls;state=0;return 0;}
  int health()override{return state;}
  bool busy()override{return false;}
  uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *,size_t)override{return -ENOTSUP;}
  bool completion(int &)override{return false;}
};

struct Provider final:mt::pki::PrivateRuntime {
  mt::Service *service=nullptr;Radio *radio=nullptr;bool readyFlag=true,authenticate=true;
  unsigned receiveCalls=0,eventCalls[2]{},deliveredMask=0;
  uint32_t local=Self;
  bool eventActive=false;mt::pki::PrivateEvent eventValue{};
  uint8_t eventFrame[255]{};size_t eventFrameSize=0;
  uint8_t lastFrame[255]{};size_t lastFrameSize=0;
  std::vector<uint32_t> seen;
  explicit Provider(uint32_t node):local(node){}
  bool acceptsPeerKey(uint32_t node,const uint8_t *key,size_t size)const override {
    if(node!=Peer)return true;
    if(!key||size!=32)return false;
    for(size_t i=0;i<size;++i)if(key[i]!=0x42)return false;
    return true;
  }
  uint32_t peer()const override{return Peer;}
  bool ready(uint32_t node)const override{return readyFlag&&node==local;}
  bool publicIdentity(uint8_t out[32])const override{memset(out,0x42,32);return true;}
  bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,uint32_t,uint32_t,
                 bool,uint32_t,bench::Frame &)override{return false;}
  void freeze()override{readyFlag=false;clearEvent();}
  bool matchesPeer(uint32_t node)const override{return readyFlag&&node==Peer;}
  bool revoke(uint32_t node)override{return node==Peer;}
  void clearEvent()override{eventActive=false;deliveredMask=0;
    bench::wipeKeyBuffer(&eventValue,sizeof(eventValue));bench::wipeKeyBuffer(eventFrame,sizeof(eventFrame));eventFrameSize=0;}
  void sweep(mt::Endpoint &)override{}
  int submit(mt::Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,
             bool,const uint8_t *,size_t,bool,uint32_t)override{return -ENOTSUP;}
  void receive(mt::Endpoint &,bench::MtCodec &,const uint8_t *frame,size_t size,
               uint32_t,uint32_t,bool)override {
    ++receiveCalls;
    assert(service&&radio&&service->privateStoragePaused()&&radio->health()==0);
    assert(frame&&size>=29&&size<=255&&frame[13]==0&&(frame[12]&0x10u));
    assert(bench::le32(frame)==local&&bench::le32(frame+4)>1&&bench::le32(frame+8));
    lastFrameSize=size;memcpy(lastFrame,frame,size);
    const uint32_t id=bench::le32(frame+8);
    if(!authenticate||std::find(seen.begin(),seen.end(),id)!=seen.end())return;
    seen.push_back(id);clearEvent();
    meshtastic_Data data=meshtastic_Data_init_zero;
    data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
    static const uint8_t text[]="synthetic authenticated text";
    data.payload.size=sizeof(text)-1;memcpy(data.payload.bytes,text,sizeof(text)-1);
    data.has_bitfield=true;data.bitfield=1;
    auto stream=pb_ostream_from_buffer(eventValue.data,sizeof(eventValue.data));
    assert(pb_encode(&stream,meshtastic_Data_fields,&data));
    eventValue.from=bench::le32(frame+4);eventValue.to=local;eventValue.id=id;
    memset(eventValue.publicKey,0x42,sizeof(eventValue.publicKey));eventValue.size=stream.bytes_written;
    memcpy(eventFrame,frame,size);eventFrameSize=size;eventActive=true;
  }
  void proof(mt::Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t)override{}
  bool event(uint8_t owner,const uint8_t *frame,size_t size,mt::pki::PrivateEvent &out)override {
    if(owner>=2)return false;++eventCalls[owner];
    if(!eventActive||!frame||size!=eventFrameSize||memcmp(frame,eventFrame,size)||
       (deliveredMask&(1u<<owner)))return false;
    deliveredMask|=1u<<owner;out=eventValue;
    if(deliveredMask==3u)clearEvent();
    return true;
  }
};

struct Host final:mt::Companion::Host {
  std::vector<meshtastic_FromRadio> records;unsigned previews=0,radioPeerObservations=0;
  MqttProxyPolicy proxyPolicy{};
  bool send(const uint8_t *bytes,size_t size)override {
    if(!bytes||size<4||bytes[0]!=0x94||bytes[1]!=0xc3||
       size_t(bytes[2]*256u+bytes[3])!=size-4)return false;
    meshtastic_FromRadio record{};auto input=pb_istream_from_buffer(bytes+4,size-4);
    if(!pb_decode(&input,meshtastic_FromRadio_fields,&record)||input.bytes_left)return false;
    records.push_back(record);return true;
  }
  int save(const mt::Config &)override{return -ENOTSUP;}
  bool ready()const override{return true;}
  bool mqttProxyPolicy(MqttProxyPolicy &out)const override{out=proxyPolicy;return true;}
  void observeRadioPeer(uint32_t,unsigned,bool,float,uint32_t)override{++radioPeerObservations;}
  void preview(uint32_t,uint32_t,uint32_t,unsigned,const uint8_t *,size_t,
               int16_t,int8_t,uint32_t,bool)override{++previews;}
};
struct HotObservation {unsigned calls=0;uint32_t from=0,utc=0,uptime=0;bool viaMqtt=false;
  static void receive(void *ctx,uint32_t node,uint32_t epoch,uint32_t seconds,bool mqtt){
    auto &self=*static_cast<HotObservation*>(ctx);++self.calls;self.from=node;self.utc=epoch;self.uptime=seconds;self.viaMqtt=mqtt;
  }
};

void connect(mt::Companion &companion,uint32_t nonce,uint32_t now) {
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
  request.want_config_id=nonce;uint8_t encoded[64]{};
  auto output=pb_ostream_from_buffer(encoded,sizeof(encoded));
  assert(pb_encode(&output,meshtastic_ToRadio_fields,&request));
  companion.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});companion.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});
  companion.byte(uint8_t(output.bytes_written>>8),now,0,mt::Companion::WorkspaceProof{true});
  companion.byte(uint8_t(output.bytes_written),now,0,mt::Companion::WorkspaceProof{true});
  for(size_t i=0;i<output.bytes_written;++i)companion.byte(encoded[i],now,0,mt::Companion::WorkspaceProof{true});
}

struct Fanout {
  mt::Companion *usb=nullptr,*ble=nullptr;mt::Service *service=nullptr;Radio *radio=nullptr;
  mt::companion::CompanionWorkspace *workspace=nullptr;
  unsigned calls=0;bool healthyAtCallback=false;
  static void receive(void *context,const uint8_t *frame,size_t size,uint32_t now) {
    auto &self=*static_cast<Fanout *>(context);++self.calls;
    self.healthyAtCallback=self.service&&self.radio&&self.service->receiving()&&
      !self.service->faulted()&&self.radio->health()==1;
    // Service must invoke this synchronous callback after the packet parser
    // releases shared protobuf scratch. Verify that callback code can acquire
    // the workspace, that recursive use still refuses, then release it before
    // fanout enters either Companion.
    assert(self.workspace);
    {
      auto callbackLease=self.workspace->acquire(mt::Companion::WorkspaceProof{true});
      assert(callbackLease);
      auto nested=self.workspace->acquire(mt::Companion::WorkspaceProof{true});
      assert(!nested);
    }
    // This fixture represents the synchronous private MQTT callback, not an
    // RF dequeue. Production passes the same explicit origin bit.
    self.usb->receive(frame,size,0,0,now,now,false,0,false,mt::Companion::WorkspaceProof{true});
    self.ble->receive(frame,size,0,0,now,now,false,0,false,mt::Companion::WorkspaceProof{true});
  }
};

struct TransitCapture {
  unsigned pkiCalls=0,ordinaryCalls=0;
  uint32_t localNode=0,lastFrom=0,lastTo=0,lastId=0;
  size_t lastSize=0;
  uint8_t last[255]{};
  bool enabled=true,validTime=false;uint32_t stamp=0;
  static bool pki(void *context,const uint8_t *frame,size_t size,uint32_t localNode,uint32_t stamp,bool validTime) {
    auto &self=*static_cast<TransitCapture *>(context);if(!self.enabled)return false;
    self.stamp=stamp;self.validTime=validTime;++self.pkiCalls;
    assert(frame&&size>=29&&size<=sizeof(self.last));
    self.localNode=localNode;self.lastFrom=bench::le32(frame+4);self.lastTo=bench::le32(frame);
    self.lastId=bench::le32(frame+8);self.lastSize=size;memcpy(self.last,frame,size);return true;
  }
  static void ordinary(void *context,const uint8_t *frame,size_t size,
                       const bench::RouteMetadata &metadata,unsigned channel) {
    auto &self=*static_cast<TransitCapture *>(context);++self.ordinaryCalls;
    assert(frame&&size>=17&&size<=sizeof(self.last)&&metadata.channelDecoded&&channel<8);
    self.lastFrom=bench::le32(frame+4);self.lastTo=bench::le32(frame);
    self.lastId=bench::le32(frame+8);self.lastSize=size;memcpy(self.last,frame,size);
  }
};

struct Fixture {
  Radio radio;Provider provider{Self};mt::test::KnownChannelFixtureClock channelClock;
  mt::Service service{radio,true,Self,81,true,false,&provider};
  HotObservation hot;
  Host usbHost,bleHost;mt::companion::CompanionWorkspace workspace;mt::Companion usb{service,usbHost,81,workspace,0},ble{service,bleHost,81,workspace,1};
  Fanout fanout{&usb,&ble,&service,&radio,&workspace};
  TransitCapture transit;
  Fixture(bool knownOnly=true,bool downlink=true) {
    provider.service=&service;provider.radio=&radio;channelClock.bind(service);
    assert(service.bindHotNodeObservationSink(HotObservation::receive,&hot));
    auto config=service.config;config.bytes[4]=1;
    assert(config.setDeviceRole(mt::DeviceRolePolicy::Role::CLIENT,knownOnly ?
      mt::DeviceRolePolicy::RebroadcastMode::KNOWN_ONLY : mt::DeviceRolePolicy::RebroadcastMode::ALL));
    assert(config.setPrimaryDownlinkEnabled(downlink));
    assert(config.valid()&&config.provisioned()&&service.apply(config));
    assert(service.bindMqttLocalRxSink(&Fanout::receive,&fanout));
    assert(service.bindMqttPkiTransitSink(&TransitCapture::pki,&transit));
    assert(service.bindMqttFrameSink(&TransitCapture::ordinary,&transit));
    assert(service.start(false,0));
    usbHost.proxyPolicy.mqttEnabled=true;
    usbHost.proxyPolicy.proxyToClientEnabled=true;
    usbHost.proxyPolicy.brokerConfigured=true;
    usbHost.proxyPolicy.tlsEnabled=true;
    usbHost.proxyPolicy.encryptionEnabled=true;
    usbHost.proxyPolicy.root="lab/root";
    connect(usb,0x1111,1);connect(ble,0x2222,1);
    usbHost.records.clear();bleHost.records.clear();usbHost.previews=bleHost.previews=0;
  }
  Bytes envelopeWire(uint32_t from,uint32_t id,const char *gateway="!abcdef01",
                     uint32_t to=Self,bool transit=false) {
    meshtastic_MeshPacket packet=meshtastic_MeshPacket_init_zero;
    packet.from=from;packet.to=to;packet.id=id;packet.channel=0;
    packet.hop_limit=2;packet.hop_start=4;packet.want_ack=true;
    packet.next_hop=transit?0:0x42;packet.relay_node=transit?0:0x43;
    packet.which_payload_variant=meshtastic_MeshPacket_encrypted_tag;
    packet.encrypted.size=24;
    for(size_t i=0;i<packet.encrypted.size;++i)packet.encrypted.bytes[i]=uint8_t(0xa0+i);
    // This flag/key are deliberately untrusted metadata; the fake provider
    // decides whether to emit an event only to test Service sequencing.
    packet.pki_encrypted=true;packet.public_key.size=32;memset(packet.public_key.bytes,0x77,32);
    uint8_t wire[mt::mqtt::ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes]{};size_t wireSize=0;
    assert(mt::mqtt::ServiceEnvelopeCodec::encode(&packet,"PKI",3,gateway,strlen(gateway),
      wire,sizeof(wire),&wireSize));
    return Bytes(wire,wire+wireSize);
  }
  mt::mqtt::MqttServiceEnvelopeDecoder::Decoded envelope(uint32_t from,uint32_t id,
                                                         const char *gateway="!abcdef01",
                                                         uint32_t to=Self,bool transit=false) {
    const auto wire=envelopeWire(from,id,gateway,to,transit);
    mt::mqtt::MqttServiceEnvelopeDecoder::Decoded decoded{};
    assert(mt::mqtt::MqttServiceEnvelopeDecoder::decode(wire.data(),wire.size(),&decoded));
    return decoded;
  }
  Bytes opaqueFrame(uint32_t from,uint32_t to,uint32_t id,uint8_t channel=0) {
    Bytes frame(16+24);
    for(unsigned i=0;i<4;++i){frame[i]=uint8_t(to>>(8*i));frame[4+i]=uint8_t(from>>(8*i));
      frame[8+i]=uint8_t(id>>(8*i));}
    frame[12]=uint8_t(2u|(4u<<5));frame[13]=channel;frame[14]=0;frame[15]=0;
    for(size_t i=16;i<frame.size();++i)frame[i]=uint8_t(0xa0+i);
    return frame;
  }
  Bytes ordinaryFrame(uint32_t from,uint32_t to,uint32_t id,uint8_t value) {
    mt::PrimaryChannel::Selection key{};assert(service.config.channelSelection(0,key));
    assert(service.codec.setKey(key.hash,key.bytes,key.length));mt::PrimaryChannel::clear(key);
    uint8_t payload[]={value},encoded[239]{};size_t encodedSize=0;
    assert(mt::PublicApplication::encode(8,payload,
      sizeof(payload),0,0,encoded,sizeof(encoded),encodedSize,1));
    Bytes out(encodedSize+16);size_t frameSize=0;
    assert(service.codec.encodeZeroHopData(from,to,id,encoded,encodedSize,
      out.data(),out.size(),frameSize));
    out.resize(frameSize);out[12]=uint8_t(2u|(4u<<5));return out;
  }
  void sendProxyPrivate(uint32_t id) {
    const auto payload=envelopeWire(Peer,id);
    meshtastic_ToRadio request=meshtastic_ToRadio_init_zero;
    request.which_payload_variant=meshtastic_ToRadio_mqttClientProxyMessage_tag;
    auto &proxy=request.mqttClientProxyMessage;
    strcpy(proxy.topic,"lab/root/2/e/PKI/!abcdef01");
    proxy.which_payload_variant=meshtastic_MqttClientProxyMessage_data_tag;
    proxy.payload_variant.data.size=pb_size_t(payload.size());
    memcpy(proxy.payload_variant.data.bytes,payload.data(),payload.size());
    proxy.retained=false;
    uint8_t wire[512]{};auto output=pb_ostream_from_buffer(wire,sizeof(wire));
    assert(pb_encode(&output,meshtastic_ToRadio_fields,&request));
    usb.packet(wire,output.bytes_written,200+id,mt::Companion::WorkspaceProof{true});
  }
  mt::mqtt::MqttDownlinkAdmission::Decision downlink(
      const mt::mqtt::MqttServiceEnvelopeDecoder::Decoded &decoded,uint32_t now=100) {
    mt::mqtt::MqttDownlinkAdmission::Context context{};
    context.mqttEnabled=true;context.brokerConfigured=true;context.transportReady=true;
    return service.processMqttDownlink(decoded,context,"!12345678",now,7);
  }
};

void seedKnownUser(mt::Service &service,uint32_t node) {
  meshtastic_User user=meshtastic_User_init_zero;
  std::snprintf(user.id,sizeof(user.id),"!%08x",unsigned(node));
  std::snprintf(user.long_name,sizeof(user.long_name),"peer-%08x",unsigned(node));
  std::snprintf(user.short_name,sizeof(user.short_name),"peer");
  user.hw_model=meshtastic_HardwareModel_SEEED_XIAO_S3;
  user.role=meshtastic_Config_DeviceConfig_Role_CLIENT;
  uint8_t payload[128]{};auto output=pb_ostream_from_buffer(payload,sizeof(payload));
  assert(pb_encode(&output,meshtastic_User_fields,&user));
  assert(mt::NodeUser::accept(service.nodes,Self,node,payload,output.bytes_written,-20,2)>=0);
}

unsigned privateMessages(const Host &host,uint32_t id) {
  unsigned found=0;
  for(const auto &record:host.records)if(record.which_payload_variant==meshtastic_FromRadio_packet_tag&&
      record.packet.id==id&&record.packet.pki_encrypted) {
    ++found;const auto &packet=record.packet;
    assert(packet.from==Peer&&packet.to==Self&&packet.via_mqtt&&packet.public_key.size==32);
    assert(std::all_of(packet.public_key.bytes,packet.public_key.bytes+32,[](uint8_t b){return b==0x42;}));
    assert(packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
      packet.decoded.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP);
    static const uint8_t text[]="synthetic authenticated text";
    assert(packet.decoded.payload.size==sizeof(text)-1&&
      !memcmp(packet.decoded.payload.bytes,text,sizeof(text)-1));
  }
  return found;
}

void authenticatedLocalDeliveryAndKnownOnlyBypass() {
  Fixture f;
  assert(f.service.config.rebroadcastMode()==mt::DeviceRolePolicy::RebroadcastMode::KNOWN_ONLY);
  assert(f.service.nodes.find(Peer)<0); // The pinned peer has no ordinary NodeDB user row.
  const auto input=f.envelope(Peer,501);
  assert(f.downlink(input)==mt::mqtt::MqttDownlinkAdmission::Decision::Allow);
  assert(f.provider.receiveCalls==1&&f.fanout.calls==1&&f.fanout.healthyAtCallback);
  assert(f.radio.stopCalls==1&&f.radio.restoreCalls==1&&f.radio.health()==1&&
         f.service.receiving()&&!f.service.faulted());
  assert(f.provider.lastFrameSize==input.packet.encrypted.size+16&&
         bench::le32(f.provider.lastFrame)==Self&&bench::le32(f.provider.lastFrame+4)==Peer&&
         bench::le32(f.provider.lastFrame+8)==501&&f.provider.lastFrame[13]==0&&
         f.provider.lastFrame[12]==uint8_t(2|8|0x10|(4<<5))&&
         f.provider.lastFrame[14]==0x42&&f.provider.lastFrame[15]==0x43&&
         !memcmp(f.provider.lastFrame+16,input.packet.encrypted.bytes,input.packet.encrypted.size));
  assert(privateMessages(f.usbHost,501)==1&&privateMessages(f.bleHost,501)==1);
  assert(f.provider.deliveredMask==0&&!f.provider.eventActive);
  const unsigned before=f.usbHost.records.size();
  f.usb.receive(f.provider.lastFrame,f.provider.lastFrameSize,0,0,101,101,mt::Companion::WorkspaceProof{true});
  assert(f.usbHost.records.size()==before); // One owner cannot take the same event twice.
  puts("PASS Service PKI downlink reaches fake-authenticated local event across KnownOnly and both Companion owners");
}

void failedAuthDuplicateAndUnpinnedAreNotDelivered() {
  Fixture f;
  f.provider.authenticate=false;
  auto bad=f.envelope(Peer,502);
  assert(f.downlink(bad,110)==mt::mqtt::MqttDownlinkAdmission::Decision::Allow);
  assert(f.provider.receiveCalls==1&&f.fanout.calls==1&&
         privateMessages(f.usbHost,502)==0&&privateMessages(f.bleHost,502)==0&&
         f.radio.health()==1&&!f.service.faulted());
  f.provider.authenticate=true;
  auto fresh=f.envelope(Peer,503);
  assert(f.downlink(fresh,120)==mt::mqtt::MqttDownlinkAdmission::Decision::Allow);
  assert(privateMessages(f.usbHost,503)==1&&privateMessages(f.bleHost,503)==1);
  const auto usbCount=f.usbHost.records.size(),bleCount=f.bleHost.records.size();
  assert(f.downlink(fresh,130)==mt::mqtt::MqttDownlinkAdmission::Decision::Allow);
  assert(f.provider.receiveCalls==3&&f.fanout.calls==3&&
         f.usbHost.records.size()==usbCount&&f.bleHost.records.size()==bleCount);
  auto unpinned=f.envelope(Other,504);
  assert(f.downlink(unpinned,140)==mt::mqtt::MqttDownlinkAdmission::Decision::UnpinnedPrivatePeer);
  assert(f.provider.receiveCalls==3&&f.fanout.calls==3);
  puts("PASS fake auth failure, duplicate event, and unpinned source produce no authenticated Companion delivery");
}

void noConsentAndRestoreFailureFailClosed() {
  Fixture noConsent(true,false);auto input=noConsent.envelope(Peer,505);
  assert(noConsent.downlink(input)==mt::mqtt::MqttDownlinkAdmission::Decision::NoDownlinkConsent);
  assert(!noConsent.provider.receiveCalls&&!noConsent.fanout.calls);
  Fixture restoreFail;restoreFail.radio.restoreStatus=-EIO;
  auto restoreInput=restoreFail.envelope(Peer,506);
  assert(restoreFail.downlink(restoreInput)==mt::mqtt::MqttDownlinkAdmission::Decision::Allow);
  assert(restoreFail.provider.receiveCalls==1&&restoreFail.fanout.calls==1&&
         restoreFail.service.faulted()&&!restoreFail.service.receiving()&&
         privateMessages(restoreFail.usbHost,506)==0&&privateMessages(restoreFail.bleHost,506)==0);
  puts("PASS existing stock downlink opt-in and radio restore failure suppress private event fanout");
}

void nonlocalPkiDownlinkRoutesOpaqueOnlyForTwoKnownPeers() {
  Fixture f;seedKnownUser(f.service,Peer);seedKnownUser(f.service,Other);
  assert(f.service.armEndpoint(true));
  const auto input=f.envelope(Peer,601,"!abcdef01",Other,true);
  const auto decision=f.downlink(input);
  if(decision!=mt::mqtt::MqttDownlinkAdmission::Decision::Allow)
    std::fprintf(stderr,"nonlocal admission decision=%u sourceSlot=%d destinationSlot=%d sourceIgnored=%u destinationIgnored=%u armed=%u mode=%u pending=%zu\n",
      unsigned(decision),f.service.nodes.find(Peer),f.service.nodes.find(Other),
      unsigned(f.service.nodes.ignored(Peer)),unsigned(f.service.nodes.ignored(Other)),
      unsigned(f.service.endpointArmed()),unsigned(f.service.config.rebroadcastMode()),
      f.service.route.pending());
  assert(decision==mt::mqtt::MqttDownlinkAdmission::Decision::Allow);
  assert(f.service.route.pending()==1&&f.provider.receiveCalls==0&&f.fanout.calls==0&&
         f.transit.pkiCalls==0&&f.transit.ordinaryCalls==0);
  const auto pending=f.service.route.pending();
  assert(f.downlink(input,101)==mt::mqtt::MqttDownlinkAdmission::Decision::InvalidPacket);
  assert(f.service.route.pending()==pending&&f.provider.receiveCalls==0&&f.transit.pkiCalls==0);

  const auto unknownDestination=f.envelope(Peer,602,"!abcdef01",0x11223344u,true);
  assert(f.downlink(unknownDestination,102)==
         mt::mqtt::MqttDownlinkAdmission::Decision::UnknownPrivateTransitPeer);
  assert(f.service.route.pending()==pending);
  assert(f.service.nodes.edit(Other,Self,mt::NodeCache::Edit::Ignore)==1);
  const auto ignoredDestination=f.envelope(Peer,603,"!abcdef01",Other,true);
  assert(f.downlink(ignoredDestination,103)==
         mt::mqtt::MqttDownlinkAdmission::Decision::UnknownPrivateTransitPeer);
  assert(f.service.route.pending()==pending);

  assert(f.service.nodes.edit(Other,Self,mt::NodeCache::Edit::Unignore)==1);
  const auto unknownSource=f.envelope(0x11223344u,604,"!abcdef01",Other,true);
  assert(f.downlink(unknownSource,104)==
         mt::mqtt::MqttDownlinkAdmission::Decision::UnknownPrivateTransitPeer);
  assert(f.service.route.pending()==pending);
  assert(f.service.armEndpoint(false));
  const auto relayDisabled=f.envelope(Peer,605,"!abcdef01",Other,true);
  assert(f.downlink(relayDisabled,105)==
         mt::mqtt::MqttDownlinkAdmission::Decision::PrivateTransitUnavailable);
  puts("PASS nonlocal PKI MQTT requires two known eligible peers and enters opaque RouteCore transit without local crypto delivery");
}

void pkiTransitPublicationIndependentOfRfQueue() {
  Fixture f;seedKnownUser(f.service,Peer);seedKnownUser(f.service,Other);
  assert(f.service.armEndpoint(true));
  assert(f.service.config.channelCandidates(0)==0); // No ordinary hash-zero ambiguity.
  for(uint32_t id=701;id<709;++id) {
    const auto frame=f.opaqueFrame(Peer,Other,id);
    f.service.rx(frame.data(),frame.size(),0,int(id),0xabc001u);
  }
  assert(f.service.route.pending()==8&&f.transit.pkiCalls==8&&
         f.transit.localNode==Self&&f.transit.lastFrom==Peer&&f.transit.lastTo==Other&&
         f.transit.lastId==708&&f.transit.lastSize==40);
  const auto duplicate=f.opaqueFrame(Peer,Other,701);
  f.service.rx(duplicate.data(),duplicate.size(),0,710,0xabc001u);
  assert(f.service.route.pending()==8&&f.transit.pkiCalls==8);
  const auto full=f.opaqueFrame(Peer,Other,709);
  f.service.rx(full.data(),full.size(),0,711,0xabc001u);
  assert(f.service.route.pending()==8&&f.transit.pkiCalls==9);
  f.service.rx(duplicate.data(),duplicate.size(),0,712,0);
  assert(f.transit.pkiCalls==10);

  Fixture passive(false,false);
  assert(passive.service.armEndpoint(false)); // No RF forwarding and no NodeDB key is necessary.
  auto observed=passive.opaqueFrame(Peer,Other,723);observed[12]=0;
  passive.transit.enabled=false;
  passive.service.rx(observed.data(),observed.size(),0,722,0);
  passive.transit.enabled=true;
  passive.service.rx(observed.data(),observed.size(),0,723,0,true,nullptr,false,1792660000);
  assert(passive.transit.pkiCalls==1 && passive.transit.validTime && passive.transit.stamp==1792660000 && !passive.service.route.pending());
  passive.service.rx(observed.data(),observed.size(),0,724,0);
  assert(passive.transit.pkiCalls==1);
  observed=passive.opaqueFrame(Peer,Other,724);
  passive.service.rx(observed.data(),observed.size(),0,725,0,true,nullptr,false,0,77);
  assert(passive.transit.pkiCalls==2&&!passive.transit.validTime&&passive.transit.stamp==77);
  Fixture ignored;seedKnownUser(ignored.service,Peer);seedKnownUser(ignored.service,Other);
  assert(ignored.service.armEndpoint(true));
  assert(ignored.service.nodes.edit(Other,Self,mt::NodeCache::Edit::Ignore)==1);
  const auto ignoredDestination=ignored.opaqueFrame(Peer,Other,720);
  ignored.service.rx(ignoredDestination.data(),ignoredDestination.size(),0,720,0xabc002u);
  assert(ignored.transit.pkiCalls==0);
  assert(ignored.service.nodes.edit(Other,Self,mt::NodeCache::Edit::Unignore)==1);
  assert(ignored.service.nodes.edit(Peer,Self,mt::NodeCache::Edit::Ignore)==1);
  const auto ignoredSource=ignored.opaqueFrame(Peer,Other,721);
  ignored.service.rx(ignoredSource.data(),ignoredSource.size(),0,721,0xabc002u);
  assert(ignored.transit.pkiCalls==0);

  Fixture mqttOrigin;seedKnownUser(mqttOrigin.service,Peer);seedKnownUser(mqttOrigin.service,Other);
  assert(mqttOrigin.service.armEndpoint(true));
  auto mqttFrame=mqttOrigin.opaqueFrame(Peer,Other,730);mqttFrame[12]|=0x10u;
  mqttOrigin.service.rx(mqttFrame.data(),mqttFrame.size(),0,730,0xabc003u,false);
  assert(mqttOrigin.service.route.pending()==1&&mqttOrigin.transit.pkiCalls==0);
  puts("PASS encrypted RF-origin uplink survives disarmed/zero hop/full relay queues; eight recent IDs, MQTT loops and ignores are enforced");
}

void acceptedOrdinaryUnicastTransitUsesExistingChannelSink() {
  Fixture f;seedKnownUser(f.service,Peer);seedKnownUser(f.service,Other);
  assert(f.service.armEndpoint(true));
  const auto ordinary=f.ordinaryFrame(Peer,Other,741,'o');
  f.service.rx(ordinary.data(),ordinary.size(),0,741,0xabc004u);
  if(!(f.service.route.pending()==1&&f.transit.ordinaryCalls==1&&
       f.transit.lastFrom==Peer&&f.transit.lastTo==Other&&f.transit.lastId==741&&
       f.transit.pkiCalls==0))
    std::fprintf(stderr,"ordinary transit pending=%zu sink=%u pki=%u from=%08x to=%08x id=%u channel=%u candidates=%u rejected=%u\n",
      f.service.route.pending(),f.transit.ordinaryCalls,f.transit.pkiCalls,
      f.transit.lastFrom,f.transit.lastTo,f.transit.lastId,unsigned(ordinary[13]),
      unsigned(f.service.config.channelCandidates(ordinary[13])),f.service.rejected);
  assert(f.service.route.pending()==1&&f.transit.ordinaryCalls==1&&
         f.transit.lastFrom==Peer&&f.transit.lastTo==Other&&f.transit.lastId==741&&
         f.transit.pkiCalls==0);
  const auto duplicateBefore=f.service.route.stats.duplicates;
  f.service.rx(ordinary.data(),ordinary.size(),0,742,0xabc004u);
  assert(f.transit.ordinaryCalls==1&&f.service.route.stats.duplicates>duplicateBefore);
  // MQTT ingress can traverse the same accepted route, but must not loop back
  // through the ordinary uplink sink.
  auto echoed=f.ordinaryFrame(Peer,Other,742,'m');echoed[12]|=0x10u;
  bool accepted=false;f.service.rx(echoed.data(),echoed.size(),0,743,0xabc004u,false,&accepted);
  assert(accepted&&f.transit.ordinaryCalls==1&&f.transit.pkiCalls==0);
  puts("PASS existing ordinary decoded MqttFrameSink receives route-admitted unicast only once and never republishes MQTT origin");
}

void connectedUsbProxyReentersReceiveAndUnwindsInput() {
  Fixture f;
  assert(f.usb.handshakes==1&&f.ble.handshakes==1);
  const uint32_t rfReceivedBefore=f.service.received;
  f.sendProxyPrivate(507);
  assert(f.provider.receiveCalls==1&&f.fanout.calls==1&&f.fanout.healthyAtCallback);
  assert(privateMessages(f.usbHost,507)==1&&privateMessages(f.bleHost,507)==1);
  assert(f.usb.receivedMessages==1&&f.ble.receivedMessages==1);
  assert(f.service.received==rfReceivedBefore&&f.usbHost.radioPeerObservations==0&&
         f.bleHost.radioPeerObservations==0);
  assert(f.usb.parseErrors==0&&f.usb.encodeErrors==0&&f.usb.unsupported==0&&f.usb.outputDrops==0);

  // A second canonical ToRadio command through the same connected Companion
  // proves the outer proxy handler returned and its borrowed input was cleared.
  f.sendProxyPrivate(508);
  assert(f.provider.receiveCalls==2&&f.fanout.calls==2&&f.fanout.healthyAtCallback);
  assert(privateMessages(f.usbHost,508)==1&&privateMessages(f.bleHost,508)==1);
  assert(f.usb.receivedMessages==2&&f.ble.receivedMessages==2);
  assert(f.service.received==rfReceivedBefore&&f.usbHost.radioPeerObservations==0&&
         f.bleHost.radioPeerObservations==0);
  assert(f.usb.parseErrors==0&&f.usb.encodeErrors==0&&f.usb.unsupported==0&&f.usb.outputDrops==0);
  puts("PASS connected USB ToRadio MQTT proxy synchronously reenters same Companion receive, fans out once to USB/BLE, and accepts next input cleanly");
}

void pinnedPeerKeyMismatchStillBlocksEndpointAndMetadata() {
  Fixture f;assert(f.service.armEndpoint(true));
  mt::NodeCache::Entry cached{};cached.identity(Peer,81,0);
  assert(cached.text(cached.bytes+cached.Name,40,"Synthetic peer",15)&&
         cached.text(cached.bytes+cached.Short,5,"PRV",4));
  assert(f.service.nodes.accept(cached,Self,1,0)>=0);

  meshtastic_User user=meshtastic_User_init_zero;
  std::snprintf(user.id,sizeof(user.id),"!%08x",Peer);
  std::strcpy(user.long_name,"Synthetic peer");std::strcpy(user.short_name,"PRV");
  user.public_key.size=32;std::memset(user.public_key.bytes,0x77,32);
  uint8_t userBytes[128]{};auto userStream=pb_ostream_from_buffer(userBytes,sizeof(userBytes));
  assert(pb_encode(&userStream,meshtastic_User_fields,&user));
  meshtastic_Data data=meshtastic_Data_init_zero;data.portnum=meshtastic_PortNum_NODEINFO_APP;
  data.has_bitfield=true;data.payload.size=pb_size_t(userStream.bytes_written);
  std::memcpy(data.payload.bytes,userBytes,userStream.bytes_written);
  uint8_t dataBytes[239]{},frame[255]{};auto dataStream=pb_ostream_from_buffer(dataBytes,sizeof(dataBytes));
  assert(pb_encode(&dataStream,meshtastic_Data_fields,&data));size_t frameSize=0;
  assert(f.service.codec.encodeZeroHopData(Peer,Self,749,dataBytes,dataStream.bytes_written,
      frame,sizeof(frame),frameSize));
  bench::RouteMetadata metadata{};
  assert(f.service.codec.decode(frame,frameSize,metadata)&&metadata.nodeInfoValid&&metadata.publicKeyLength==32);
  assert(!f.service.privatePeerMetadata(Peer,metadata));
  const auto received=f.service.received;
  f.service.rx(frame,frameSize,0,749,0xabc007u,true,nullptr,false,1792660000u,10);
  assert(f.service.received==received+1&&f.hot.calls==0&&f.usbHost.radioPeerObservations==0&&
         f.bleHost.radioPeerObservations==0&&f.usbHost.previews==0&&f.bleHost.previews==0);
  puts("PASS pinned peer-key mismatch remains an endpoint rejection and does not update hot metadata");
}
}

int main() {
  authenticatedLocalDeliveryAndKnownOnlyBypass();
  failedAuthDuplicateAndUnpinnedAreNotDelivered();
  noConsentAndRestoreFailureFailClosed();
  connectedUsbProxyReentersReceiveAndUnwindsInput();
  nonlocalPkiDownlinkRoutesOpaqueOnlyForTwoKnownPeers();
  pkiTransitPublicationIndependentOfRfQueue();
  acceptedOrdinaryUnicastTransitUsesExistingChannelSink();
  pinnedPeerKeyMismatchStillBlocksEndpointAndMetadata();
  return 0;
}
