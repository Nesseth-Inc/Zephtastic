// SPDX-License-Identifier: GPL-3.0-only
// Actual Service MQTT sink and SettingsCommit consent ordering; native only.
#include "Service.h"
#include "Companion.h"
#include "SettingsCommit.h"
#include "network/MqttGatewayAdmin.h"
#include "network/MqttDownlinkMailbox.h"
#include "known_channel_fixture.h"
#include "pb_encode.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {
constexpr uint32_t Self=0x12345678,Peer=0x24680135;
using Bytes=std::vector<uint8_t>;

struct Memory final:mt::SettingsBackend {
  std::map<std::string,Bytes> files;
  std::vector<std::string> writes;
  std::string failPrefix;
  int read(const char *key,uint8_t *out,size_t capacity)override {
    auto it=files.find(key);if(it==files.end())return -ENOENT;
    if(it->second.size()>capacity)return -E2BIG;
    if(!it->second.empty())memcpy(out,it->second.data(),it->second.size());
    return int(it->second.size());
  }
  int write(const char *key,const uint8_t *data,size_t size)override {
    writes.emplace_back(key);
    if(!failPrefix.empty()&&writes.back().rfind(failPrefix,0)==0)return -EIO;
    files[writes.back()]=Bytes(data,data+size);return 0;
  }
};

struct Radio final:mt::Radio {
  int state=0,completionStatus=0,sendStatus=0;
  bool completionReady=false,busyFlag=false;
  unsigned sends=0,airtimeCalls=0,starts=0;
  bool ready()override{return true;}
  int start()override{++starts;state=1;return 0;}
  int restore()override{state=1;return 0;}
  int stop()override{state=0;return 0;}
  int health()override{return state;}
  bool busy()override{return busyFlag;}
  uint32_t airtime(size_t)override{++airtimeCalls;return 100;}
  int send(uint8_t *,size_t)override{++sends;return sendStatus;}
  bool completion(int &result)override {
    if(!completionReady)return false;
    completionReady=false;result=completionStatus;return true;
  }
};

struct Sink {
  unsigned calls=0,lastChannel=99;
  uint32_t lastFrom=0,lastId=0;
  uint32_t lastPort=0;
  uint8_t lastPayload=0;
  size_t lastLength=0;
  uint8_t lastFrame[255]{};
  static void receive(void *context,const uint8_t *frame,size_t length,
                      const bench::RouteMetadata &metadata,unsigned channel) {
    auto &self=*static_cast<Sink *>(context);++self.calls;self.lastChannel=channel;
    self.lastFrom=bench::le32(frame+4);self.lastId=bench::le32(frame+8);
    self.lastPort=metadata.portnum;self.lastPayload=metadata.payloadLength?metadata.payload[0]:0;
    self.lastLength=length;assert(length<=sizeof(self.lastFrame));memcpy(self.lastFrame,frame,length);
  }
};
struct HotObservation {unsigned calls=0;uint32_t from=0,utc=0,uptime=0;bool viaMqtt=false;
  static void receive(void *ctx,uint32_t node,uint32_t epoch,uint32_t seconds,bool mqtt){
    auto &self=*static_cast<HotObservation*>(ctx);++self.calls;self.from=node;self.utc=epoch;self.uptime=seconds;self.viaMqtt=mqtt;
  }
};

struct Fixture {
  Radio radio;
  mt::test::KnownChannelFixtureClock channelClock;
  mt::Service service{radio,true,Self,81,true,false};
  Sink sink;
  HotObservation hot;
  Fixture() {
    channelClock.bind(service);
    auto config=service.config;config.bytes[4]=1;config.setTxEnabled(true);
    assert(config.valid()&&config.provisioned()&&service.apply(config));
    assert(service.bindMqttFrameSink(&Sink::receive,&sink));
    assert(service.bindHotNodeObservationSink(&HotObservation::receive,&hot));
    assert(!service.bindMqttFrameSink(&Sink::receive,&sink));
    assert(service.start(false,0)&&service.armEndpoint(true));
  }
  Bytes data(uint8_t value,uint8_t bitfield=1) {
    uint8_t payload[]={value};uint8_t encoded[239]{};size_t size=0;
    assert(mt::PublicApplication::encode(8,payload,sizeof(payload),0,0,encoded,sizeof(encoded),size,bitfield));
    return Bytes(encoded,encoded+size);
  }
  Bytes frame(uint32_t from,uint32_t to,uint32_t id,uint8_t value,uint8_t bitfield=1) {
    mt::PrimaryChannel::Selection key{};assert(service.config.channelSelection(0,key));
    assert(service.codec.setKey(key.hash,key.bytes,key.length));mt::PrimaryChannel::clear(key);
    const auto encoded=data(value,bitfield);Bytes out(encoded.size()+16);size_t size=0;
    assert(service.codec.encodeZeroHopData(from,to,id,encoded.data(),encoded.size(),out.data(),out.size(),size));
    out.resize(size);return out;
  }
  void receive(const Bytes &frame,uint32_t now=10) {
    service.rx(frame.data(),frame.size(),0,now,0xabcdef01);
  }
  void enableDownlink() {
    service.stop();auto config=service.config;assert(config.setPrimaryDownlinkEnabled(true));
    assert(service.apply(config)&&service.start(false,0)&&service.armEndpoint(true));
  }
};

struct WireCompanionHost final:mt::Companion::Host {
  std::vector<meshtastic_FromRadio> records;
  bool send(const uint8_t *p,size_t n)override {
    if(!p||n<4||p[0]!=0x94||p[1]!=0xc3||size_t(p[2]*256u+p[3])!=n-4)return false;
    meshtastic_FromRadio record{};auto input=pb_istream_from_buffer(p+4,n-4);
    if(!pb_decode(&input,meshtastic_FromRadio_fields,&record)||input.bytes_left)return false;
    records.push_back(record);return true;
  }
  int save(const mt::Config &)override{return -ENOTSUP;}
  bool ready()const override{return true;}
};

void companionHandshake(mt::Companion &companion,uint32_t nonce,uint32_t now) {
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
  request.want_config_id=nonce;uint8_t encoded[64]{};auto output=pb_ostream_from_buffer(encoded,sizeof(encoded));
  assert(pb_encode(&output,meshtastic_ToRadio_fields,&request));
  companion.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});companion.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});
  companion.byte(uint8_t(output.bytes_written>>8),now,0,mt::Companion::WorkspaceProof{true});companion.byte(uint8_t(output.bytes_written),now,0,mt::Companion::WorkspaceProof{true});
  for(size_t i=0;i<output.bytes_written;++i)companion.byte(encoded[i],now,0,mt::Companion::WorkspaceProof{true});
}

struct CommitFixture {
  Radio radio;
  Memory backend;
  mt::test::KnownChannelFixtureClock channelClock;
  mt::Service service{radio,true,Self,81,true,false};
  mt::SettingsJournal coreJournal;
  mt::SettingsEdit core{service,coreJournal,backend};
  mt::NodeJournal nodes;
  mt::network::MqttGatewayJournal mqttJournal;
  mt::network::MqttGatewayEdit mqttEdit{service,mqttJournal,backend};
  mt::network::MqttGatewayAdmin mqttAdmin{mqttEdit,core};
  mt::SettingsCommit commit{service,radio,core,nodes,backend};
  CommitFixture() {
    channelClock.bind(service);
    assert(!coreJournal.load(backend));
    auto config=coreJournal.config();config.bytes[4]=1;config.setTxEnabled(true);
    assert(!coreJournal.save(backend,config)&&service.apply(config));
    assert(!nodes.load(backend,service.nodes,Self));
    assert(!mqttJournal.load(backend)&&mqttEdit.load()&&mqttAdmin.loaded());
    commit.mqtt(mqttAdmin);
    assert(service.start(false,0)&&service.armEndpoint(true));
    backend.writes.clear();
  }
  void openDirtyBatch() {
    assert(!core.begin(10)&&core.open());
    auto changed=service.config;assert(changed.setDisplay(45,true));
    assert(!core.save(changed,11)&&core.dirty());
    assert(!mqttAdmin.saveConfigOkToMqtt(true,true,12)&&mqttAdmin.dirty());
    assert(!mqttJournal.config().configOkToMqtt());
  }
};

struct RegionRetryHost final:mt::Companion::Host {
  CommitFixture &fixture;
  uint32_t now=0;
  unsigned rootAttempts=0;
  std::vector<meshtastic_FromRadio> records;
  explicit RegionRetryHost(CommitFixture &f):fixture(f){}
  bool send(const uint8_t *p,size_t n)override {
    if(!p||n<4||p[0]!=0x94||p[1]!=0xc3||size_t(p[2]*256u+p[3])!=n-4)return false;
    meshtastic_FromRadio record{};auto input=pb_istream_from_buffer(p+4,n-4);
    if(!pb_decode(&input,meshtastic_FromRadio_fields,&record)||input.bytes_left)return false;
    records.push_back(record);return true;
  }
  int save(const mt::Config &config)override{return fixture.core.save(config,now);}
  bool mqttAvailable()const override{return fixture.mqttAdmin.ready();}
  bool mqttRegionRootRetryPending()const override{return fixture.mqttAdmin.regionRootRetryPending();}
  bool configOkToMqtt()const override{return fixture.mqttAdmin.configOkToMqtt(true);}
  int saveMqttRegionRootTopic(const char *region,size_t length,bool consent,uint32_t at)override{
    ++rootAttempts;
    if(!fixture.mqttAdmin.ready()&&!fixture.mqttAdmin.canRetryIoError())return consent?-EACCES:0;
    fixture.mqttAdmin.runtimePolicy(true,false);
    return fixture.mqttAdmin.applyRegionRootTopic(region,length,consent,at);
  }
  bool ready()const override{return true;}
  meshtastic_Routing_Error result(uint32_t request)const {
    for(auto it=records.rbegin();it!=records.rend();++it) {
      if(it->which_payload_variant!=meshtastic_FromRadio_packet_tag||
         it->packet.which_payload_variant!=meshtastic_MeshPacket_decoded_tag||
         it->packet.decoded.portnum!=meshtastic_PortNum_ROUTING_APP||
         it->packet.decoded.request_id!=request)continue;
      meshtastic_Routing routing{};auto in=pb_istream_from_buffer(
        it->packet.decoded.payload.bytes,it->packet.decoded.payload.size);
      if(pb_decode(&in,meshtastic_Routing_fields,&routing)&&!in.bytes_left&&
         routing.which_variant==meshtastic_Routing_error_reason_tag)return routing.error_reason;
    }
    assert(false&&"missing Companion Routing result");return meshtastic_Routing_Error_NONE;
  }
};

meshtastic_AdminMessage regionConfig(const mt::Config &config,
    meshtastic_Config_LoRaConfig_RegionCode region,bool mqttConsent) {
  meshtastic_AdminMessage message{};message.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
  message.set_config.which_payload_variant=meshtastic_Config_lora_tag;
  auto &l=message.set_config.payload_variant.lora;
  l.region=region;const auto radio=config.radioRequest();
  l.use_preset=config.usePreset();l.modem_preset=static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(config.modemPreset());
  l.hop_limit=config.hopLimit();l.channel_num=config.channelNumber();l.sx126x_rx_boosted_gain=true;
  l.bandwidth=radio.bandwidth;l.spread_factor=radio.spreadingFactor;l.coding_rate=radio.codingRate;
  l.override_frequency=radio.overrideMHz;l.frequency_offset=radio.offsetMHz;
  l.tx_power=radio.powerSpecified?radio.power:2;l.tx_enabled=config.txEnabled();
  l.config_ok_to_mqtt=mqttConsent;l.ignore_mqtt=config.ignoreMqtt();
  return message;
}

void sendCompanionAdmin(mt::Companion &companion,RegionRetryHost &host,
                        const meshtastic_AdminMessage &admin,uint32_t id,uint32_t now) {
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;
  auto &packet=request.packet;packet.to=Self;packet.id=id;packet.want_ack=true;
  packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  packet.decoded.portnum=meshtastic_PortNum_ADMIN_APP;packet.decoded.want_response=true;
  auto payload=pb_ostream_from_buffer(packet.decoded.payload.bytes,sizeof(packet.decoded.payload.bytes));
  assert(pb_encode(&payload,meshtastic_AdminMessage_fields,&admin));packet.decoded.payload.size=payload.bytes_written;
  uint8_t bytes[768]{};auto output=pb_ostream_from_buffer(bytes,sizeof(bytes));
  assert(pb_encode(&output,meshtastic_ToRadio_fields,&request));
  host.now=now;host.records.clear();companion.packet(bytes,output.bytes_written,now,mt::Companion::WorkspaceProof{true});
}

void serviceUplinkCases() {
  Fixture f;
  const auto remote=f.frame(Peer,0xffffffffu,101,'r');
  f.receive(remote);assert(f.sink.calls==1&&f.sink.lastChannel==0&&f.sink.lastFrom==Peer&&
    f.sink.lastId==101&&f.sink.lastPort==8&&f.sink.lastPayload=='r'&&f.sink.lastLength==remote.size());

  // A local broadcast echo follows Service's real RX path but is not an uplink.
  const auto echo=f.frame(Self,0xffffffffu,102,'e');f.receive(echo,20);
  assert(f.sink.calls==1);

  // Channel-zero unicast is the stock PKI/private-message boundary. With no
  // private runtime available, it must stop before the ordinary MQTT sink.
  auto privateFrame=f.frame(Peer,Self,103,'p');privateFrame[13]=0;f.receive(privateFrame,30);
  assert(f.sink.calls==1&&f.sink.lastId==101);

  // Service invokes the local-origin sink only after a clean terminal TX result.
  const auto app=f.data('t');
  assert(!f.service.submitPublicApplication(0,0xffffffffu,104,app.data(),app.size(),0,false,300));
  f.service.poll(10000,1);assert(f.radio.sends==1&&f.service.active()&&f.sink.calls==1);
  f.radio.completionReady=true;f.radio.completionStatus=0;f.service.poll(11000,2);
  assert(!f.service.active()&&f.service.sent==1&&f.sink.calls==2&&f.sink.lastFrom==Self&&
    f.sink.lastId==104&&f.sink.lastPort==8&&f.sink.lastPayload=='t');

  Fixture failed;
  const auto failedApp=failed.data('x');
  assert(!failed.service.submitPublicApplication(0,0xffffffffu,105,failedApp.data(),failedApp.size(),0,false,300));
  failed.service.poll(10000,1);assert(failed.radio.sends==1&&failed.service.active()&&failed.sink.calls==0);
  failed.radio.completionReady=true;failed.radio.completionStatus=-EIO;failed.service.poll(11000,2);
  assert(failed.service.faulted()&&failed.sink.calls==0&&failed.service.sent==0);
  puts("PASS Service MQTT sink: accepted RX, local echo/private-channel suppression, successful TX completion only");
}

void downlinkConsentPersistenceCases() {
  Fixture f;
  auto config=f.service.config;
  assert(!config.anyChannelDownlinkEnabled());
  assert(config.setPrimaryDownlinkEnabled(true)&&config.channelDownlinkEnabled(0));
  mt::ChannelRecord secondary;
  assert(secondary.set(2,"Sub",nullptr,0,0,false,false,true));
  assert(config.setSecondaryChannel(1,secondary)&&config.channelDownlinkEnabled(1));
  assert(config.anyChannelDownlinkEnabled()&&config.valid());
  assert(config.setIgnoreMqtt(true)&&config.ignoreMqtt());
  mt::Config reboot;
  assert(mt::Config::decode(config.bytes,sizeof(config.bytes),reboot)&&reboot.same(config));
  mt::ChannelRecord primaryRead,secondaryRead;
  assert(reboot.channelRecord(0,primaryRead)&&primaryRead.downlink());
  assert(reboot.channelRecord(1,secondaryRead)&&secondaryRead.downlink());
  assert(reboot.promoteChannel(1)&&reboot.primaryDownlinkEnabled());
  assert(reboot.channelRecord(1,secondaryRead)&&secondaryRead.downlink());

  auto old=f.service.config;old.bytes[2]=16;old.bytes[123]=old.bytes[124]=0;old.bytes[606]=1;old.seal();
  mt::Config migrated;
  assert(mt::Config::decode(old.bytes,sizeof(old.bytes),migrated)&&migrated.valid()&&
         migrated.bytes[2]==mt::Config::Version&&!migrated.anyChannelDownlinkEnabled()&&
         !migrated.ignoreMqtt()&&migrated.bytes[606]==1&&migrated.gnssSettings().enabled);
  auto invalid=migrated;invalid.bytes[123]=2;invalid.seal();mt::Config unchanged;
  assert(!mt::Config::decode(invalid.bytes,sizeof(invalid.bytes),unchanged));
  invalid=migrated;invalid.bytes[124]=2;invalid.seal();
  assert(!mt::Config::decode(invalid.bytes,sizeof(invalid.bytes),unchanged));
  puts("PASS MQTT downlink opt-in and ignore_mqtt persist by channel, promote atomically, default off, and migrate V16 off");
}

void channelBroadcastReachesPhone(bool regionSet) {
  // Python's pinned stock protobuf + public LongFast key, hop 0, bitfield present.
  // Same encoded ServiceEnvelope is used by the isolated TLS broker.
  static constexpr uint8_t wire[]={
    0x0a, 0x2f, 0x0d, 0x11, 0x02, 0xe7, 0x24, 0x15, 0xff, 0xff, 0xff, 0xff,
    0x18, 0x08, 0x2a, 0x1c, 0x89, 0x24, 0x1d, 0xc2, 0xc6, 0x41, 0xc1, 0xef,
    0xd1, 0xbf, 0xdd, 0x17, 0x4d, 0xad, 0xab, 0x3b, 0xba, 0xc3, 0x6a, 0xc2,
    0x0c, 0x3e, 0x1c, 0x55, 0xf5, 0xf2, 0xc7, 0xf8, 0x35, 0x05, 0xec, 0xd0,
    0x6b, 0x12, 0x08, 0x4c, 0x6f, 0x6e, 0x67, 0x46, 0x61, 0x73, 0x74, 0x1a,
    0x09, 0x21, 0x32, 0x34, 0x65, 0x37, 0x30, 0x32, 0x31, 0x31,
  };
  mt::mqtt::MqttServiceEnvelopeDecoder::Decoded envelope{};
  assert(mt::mqtt::MqttServiceEnvelopeDecoder::decode(wire,sizeof(wire),&envelope));
  Fixture f;f.service.stop();
  auto config=f.service.config;assert(config.setPrimaryDownlinkEnabled(true));config.setTxEnabled(false);
  if(!regionSet){config.bytes[4]=0;config.seal();assert(!config.provisioned()&&config.valid());}
  assert(f.service.apply(config));
  WireCompanionHost host;mt::companion::CompanionWorkspace workspace;
  mt::Companion companion(f.service,host,81,workspace,0);
  auto deliver=[](void *ctx,const uint8_t *p,size_t n,uint32_t now){
    auto &owner=*static_cast<mt::Companion*>(ctx);
    owner.receive(p,n,0,0,now,now,false,0,false,mt::Companion::WorkspaceProof{true});
  };
  assert(f.service.bindMqttLocalRxSink(deliver,&companion));
  if(regionSet)assert(f.service.start(false,0));
  assert(f.service.receiving()==regionSet&&!f.service.endpointArmed());
  const unsigned starts=f.radio.starts,airtime=f.radio.airtimeCalls;
  companionHandshake(companion,0x503,10);assert(companion.handshakes==1);host.records.clear();
  mt::mqtt::MqttDownlinkAdmission::Context c{};
  c.mqttEnabled=c.transportReady=c.brokerConfigured=c.encryptionEnabled=c.tlsEnabled=c.tlsTrustReady=true;
  auto original=envelope;
  using Decision=mt::mqtt::MqttDownlinkAdmission::Decision;
  if(!regionSet){
    // The wire origin bit is not proof. Only a Service-admitted, borrowed
    // frame may pass to Companion while the physical receiver is stopped.
    auto unauth=f.frame(Peer,UINT32_MAX,707,'u');unauth[12]|=0x10;
    f.service.rx(unauth.data(),unauth.size(),0,89,1,false);
    companion.receive(unauth.data(),unauth.size(),0,0,89,89,false,0,false,mt::Companion::WorkspaceProof{true});
    assert(host.records.empty()&&!f.service.received);
    auto stopped=config;stopped.bytes[4]=1;stopped.seal();assert(f.service.apply(stopped));
    assert(f.service.processMqttDownlink(envelope,c,"!12345678",89,7)==Decision::ReceiveUnavailable);
    assert(f.service.apply(config));
    auto guard=envelope;guard.packet.to=0x12340056;
    assert(f.service.processMqttDownlink(guard,c,"!12345678",90,7)==Decision::ReceiveUnavailable);
    guard=envelope;strcpy(guard.channelId,"PKI");guard.channelIdLength=3;
    guard.packet.channel=0;guard.packet.to=Self;
    assert(f.service.processMqttDownlink(guard,c,"!12345678",91,7)==Decision::PrivateRuntimeUnavailable);
    auto ungranted=config;assert(ungranted.setPrimaryDownlinkEnabled(false)&&f.service.apply(ungranted));
    assert(f.service.processMqttDownlink(envelope,c,"!12345678",92,7)==Decision::NoDownlinkConsent);
    uint32_t ignored=envelope.packet.from;
    assert(config.setIgnoredSenders(&ignored,1)&&f.service.apply(config));
    assert(f.service.processMqttDownlink(envelope,c,"!12345678",93,7)==Decision::Ignored);
    assert(config.setIgnoredSenders(nullptr,0)&&f.service.apply(config));
  }
  c.tlsTrustReady=false;f.service.processMqttDownlink(envelope,c,"!12345678",100,7);
  c.tlsTrustReady=true;envelope.packet.channel^=1;f.service.processMqttDownlink(envelope,c,"!12345678",101,7);
  assert(host.records.empty());
  for(unsigned i=0;i<2;++i)f.service.processMqttDownlink(original,c,"!12345678",102+i,7);
  unsigned matches=0;
  const char text[]="ZT private TLS fixture";
  for(const auto &record:host.records)if(record.which_payload_variant==meshtastic_FromRadio_packet_tag){
    const auto &pkt=record.packet;
    assert(pkt.id==original.packet.id&&pkt.from==0x24e70211&&pkt.to==UINT32_MAX&&pkt.via_mqtt&&
      pkt.decoded.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP&&pkt.decoded.payload.size==sizeof(text)-1&&
      !memcmp(pkt.decoded.payload.bytes,text,sizeof(text)-1));++matches;
  }
  f.service.poll(30000,8);
  assert(matches==1&&!f.sink.calls&&!f.radio.sends&&f.radio.starts==starts);
  assert(f.service.receiving()==regionSet&&!f.service.endpointArmed());
  if(!regionSet)assert(f.radio.airtimeCalls==airtime&&!f.service.route.pending()&&!f.service.endpointControls());
  puts(regionSet?"PASS stock zero-hop TLS channel broadcast reaches phone once, with RF TX disarmed":
    "PASS region UNSET MQTT channel reaches phone once, with consent, trust, PKI and transit isolation and no RF activity");
}

void downlinkServiceReceiveCases() {
  Fixture f;
  auto wire=f.frame(Peer,0xffffffffu,701,'m');
  mt::mqtt::MqttServiceEnvelopeDecoder::Decoded envelope{};
  envelope.kind=mt::mqtt::MqttServiceEnvelopeDecoder::PacketKind::kEncrypted;
  mt::ChannelRecord channel;assert(f.service.config.channelRecord(0,channel));
  const char *channelName=channel.name()[0]?channel.name():f.service.config.modemName();
  strcpy(envelope.channelId,channelName);envelope.channelIdLength=strlen(channelName);
  strcpy(envelope.gatewayId,"!abcdef01");envelope.gatewayIdLength=9;
  char topic[160]{};int topicSize=snprintf(topic,sizeof(topic),"lab/root/2/e/%s/!abcdef01",channelName);
  assert(topicSize>0&&mt::mqtt::MqttDownlinkAdmission::topicMatches("lab/root",topic,size_t(topicSize),envelope));
  const auto channelLength=envelope.channelIdLength;
  strcpy(envelope.channelId,"bad/name");envelope.channelIdLength=8;
  assert(!mt::mqtt::MqttDownlinkAdmission::topicMatches("lab/root",topic,size_t(topicSize),envelope));
  strcpy(envelope.channelId,channelName);envelope.channelIdLength=channelLength;
  topic[topicSize-1]='2';assert(!mt::mqtt::MqttDownlinkAdmission::topicMatches("lab/root",topic,size_t(topicSize),envelope));
  topic[topicSize-1]='1';
  auto &p=envelope.packet;p.from=Peer;p.to=0xffffffffu;p.id=701;
  p.hop_limit=p.hop_start=3;p.channel=wire[13];p.via_mqtt=false;
  p.which_payload_variant=meshtastic_MeshPacket_encrypted_tag;
  p.encrypted.size=pb_size_t(wire.size()-16);memcpy(p.encrypted.bytes,wire.data()+16,wire.size()-16);
  mt::mqtt::MqttDownlinkAdmission::Context context{};context.mqttEnabled=true;
  context.brokerConfigured=true;context.transportReady=true;context.encryptionEnabled=true;
  assert(f.service.processMqttDownlink(envelope,context,"!12345678",30,7)==
         mt::mqtt::MqttDownlinkAdmission::Decision::NoDownlinkConsent);
  f.enableDownlink();
  auto preflight=context;preflight.mqttEnabled=false;
  assert(f.service.processMqttDownlink(envelope,preflight,"!12345678",31,8)==
         mt::mqtt::MqttDownlinkAdmission::Decision::Disabled);
  preflight=context;preflight.brokerConfigured=false;
  assert(f.service.processMqttDownlink(envelope,preflight,"!12345678",31,8)==
         mt::mqtt::MqttDownlinkAdmission::Decision::Unconfigured);
  preflight=context;preflight.tlsEnabled=true;
  assert(f.service.processMqttDownlink(envelope,preflight,"!12345678",31,8)==
         mt::mqtt::MqttDownlinkAdmission::Decision::TransportUnavailable);
  preflight.tlsTrustReady=true;preflight.channelActive=true;preflight.channelDownlinkEnabled=true;
  preflight.channelKeyBound=true;preflight.channelIndex=0;preflight.channelHash=wire[13];
  preflight.localNode=Self;preflight.localChannelId=channelName;
  preflight.localGatewayId="!12345678";preflight.localGatewayIdLength=9;
  preflight.receivePathReady=true;preflight.encryptionEnabled=true;
  assert(mt::mqtt::MqttDownlinkAdmission::evaluate(preflight,envelope)==
         mt::mqtt::MqttDownlinkAdmission::Decision::Allow);
  const uint8_t channelHash=envelope.packet.channel;
  envelope.packet.channel=uint8_t(channelHash+1);
  assert(f.service.processMqttDownlink(envelope,context,"!12345678",31,8)==
         mt::mqtt::MqttDownlinkAdmission::Decision::KeyMismatch);
  envelope.packet.channel=channelHash;
  envelope.packet.encrypted.size=pb_size_t(sizeof(envelope.packet.encrypted.bytes)+1);
  assert(f.service.processMqttDownlink(envelope,context,"!12345678",31,8)==
         mt::mqtt::MqttDownlinkAdmission::Decision::UnsupportedPacket);
  envelope.packet.encrypted.size=pb_size_t(wire.size()-16);
  context.encryptionEnabled=false;
  assert(f.service.processMqttDownlink(envelope,context,"!12345678",31,8)==
         mt::mqtt::MqttDownlinkAdmission::Decision::UnsupportedPacket);
  context.encryptionEnabled=true;
  assert(f.service.processMqttDownlink(envelope,context,"!12345678",31,8,1792660000u,31)==
         mt::mqtt::MqttDownlinkAdmission::Decision::Allow);
  assert(f.service.received==0&&f.sink.calls==0); // Broker ingress is not RF RX or an uplink source.
  assert(f.hot.calls==1&&f.hot.from==Peer&&f.hot.utc==1792660000u&&f.hot.uptime==31&&f.hot.viaMqtt);

  // A valid ordinary MQTT packet addressed to this node follows the endpoint
  // receive path. It is not a transit route admission, but processMqttDownlink
  // must preserve the established successful result.
  const auto localWire=f.frame(Peer,Self,703,'l');
  p.to=Self;p.id=703;p.channel=localWire[13];
  p.encrypted.size=pb_size_t(localWire.size()-16);
  memcpy(p.encrypted.bytes,localWire.data()+16,localWire.size()-16);
  assert(f.service.processMqttDownlink(envelope,context,"!12345678",32,9,1792660001u,32)==
         mt::mqtt::MqttDownlinkAdmission::Decision::Allow);
  assert(f.service.received==0&&f.sink.calls==0&&f.hot.calls==2&&f.hot.utc==1792660001u&&f.hot.viaMqtt);
  strcpy(envelope.gatewayId,"!12345678");
  assert(f.service.processMqttDownlink(envelope,context,"!12345678",32,9)==
         mt::mqtt::MqttDownlinkAdmission::Decision::GatewayEcho);
  assert(f.service.received==0);
  // A distinct RF packet can retain MQTT origin and must update the latest
  // origin bit even though it arrived through the radio interface.
  auto rfMqttOrigin=f.frame(Peer,UINT32_MAX,704,'m');rfMqttOrigin[12]|=0x10u;
  f.service.rx(rfMqttOrigin.data(),rfMqttOrigin.size(),0,33,10,true,nullptr,false,1792660002u,33);
  assert(f.service.received==1&&f.sink.calls==0&&f.hot.calls==3&&f.hot.utc==1792660002u&&f.hot.viaMqtt);
  bench::wipeKeyBuffer(&envelope,sizeof(envelope));bench::wipeKeyBuffer(wire.data(),wire.size());bench::wipeKeyBuffer(rfMqttOrigin.data(),rfMqttOrigin.size());

  Fixture plain;plain.enableDownlink();auto data=plain.data('p');
  mt::mqtt::MqttServiceEnvelopeDecoder::Decoded decoded{};
  decoded.kind=mt::mqtt::MqttServiceEnvelopeDecoder::PacketKind::kDecoded;
  mt::ChannelRecord plainChannel;assert(plain.service.config.channelRecord(0,plainChannel));
  const char *plainName=plainChannel.name()[0]?plainChannel.name():plain.service.config.modemName();
  strcpy(decoded.channelId,plainName);decoded.channelIdLength=strlen(plainName);
  strcpy(decoded.gatewayId,"!abcdef01");decoded.gatewayIdLength=9;
  auto &body=decoded.packet;body.from=Peer;body.to=0xffffffffu;body.id=702;
  body.channel=0;body.hop_limit=body.hop_start=3;
  body.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  auto input=pb_istream_from_buffer(data.data(),data.size());
  assert(pb_decode(&input,meshtastic_Data_fields,&body.decoded)&&!input.bytes_left);
  auto plaintextContext=context;plaintextContext.encryptionEnabled=false;
  assert(plain.service.processMqttDownlink(decoded,plaintextContext,"!12345678",40,10)==
         mt::mqtt::MqttDownlinkAdmission::Decision::UnsupportedPacket&&plain.service.received==0);
  assert(plain.service.processMqttDownlink(decoded,context,"!12345678",41,11)==
         mt::mqtt::MqttDownlinkAdmission::Decision::UnsupportedPacket&&plain.service.received==0);
  bench::wipeKeyBuffer(&decoded,sizeof(decoded));bench::wipeKeyBuffer(data.data(),data.size());
  puts("PASS MQTT encrypted-only receive admission, consent, gateway echo, and normal RX path without uplink reflection");
}

void mqttSelfEchoAcknowledgementCases() {
  Fixture f;f.enableDownlink();
  constexpr uint32_t packetId=7601;
  const auto app=f.data('e');
  assert(!f.service.submitPublicApplication(0,Peer,packetId,app.data(),app.size(),0,true,9500));
  f.service.poll(10000,1);assert(f.radio.sends==1&&f.service.active());
  f.radio.completionReady=true;f.radio.completionStatus=0;f.service.poll(11000,2);
  assert(!f.service.active()&&f.service.sent==1&&f.service.endpointFree()==3);
  const unsigned sendsBefore=f.radio.sends,sinkBefore=f.sink.calls;
  const unsigned receivedBefore=f.service.received,controlsBefore=f.service.endpointControls();

  assert(f.sink.lastId==packetId&&f.sink.lastLength>=17);
  Bytes outbound(f.sink.lastFrame,f.sink.lastFrame+f.sink.lastLength);
  mt::mqtt::MqttServiceEnvelopeDecoder::Decoded echo{};
  echo.kind=mt::mqtt::MqttServiceEnvelopeDecoder::PacketKind::kEncrypted;
  mt::ChannelRecord channel;assert(f.service.config.channelRecord(0,channel));
  const char *channelName=channel.name()[0]?channel.name():f.service.config.modemName();
  strcpy(echo.channelId,channelName);echo.channelIdLength=strlen(channelName);
  strcpy(echo.gatewayId,"!12345678");echo.gatewayIdLength=9;
  auto &p=echo.packet;p.from=Self;p.to=Peer;p.id=packetId;
  p.hop_limit=uint8_t(outbound[12]&7u);p.hop_start=uint8_t((outbound[12]>>5)&7u);
  p.next_hop=outbound[14];p.relay_node=outbound[15];
  p.want_ack=true;p.channel=outbound[13];p.which_payload_variant=meshtastic_MeshPacket_encrypted_tag;
  p.encrypted.size=pb_size_t(outbound.size()-16);
  memcpy(p.encrypted.bytes,outbound.data()+16,outbound.size()-16);
  mt::mqtt::MqttDownlinkAdmission::Context context{};
  context.mqttEnabled=true;context.brokerConfigured=true;context.transportReady=true;
  context.encryptionEnabled=true;

  auto wrongId=echo;wrongId.packet.id++;
  assert(f.service.processMqttDownlink(wrongId,context,"!12345678",12000,3)==
         mt::mqtt::MqttDownlinkAdmission::Decision::GatewayEcho);
  auto foreignSource=echo;foreignSource.packet.from=Peer;
  assert(f.service.processMqttDownlink(foreignSource,context,"!12345678",12001,4)==
         mt::mqtt::MqttDownlinkAdmission::Decision::GatewayEcho);
  auto foreignGateway=echo;strcpy(foreignGateway.gatewayId,"!abcdef01");
  assert(f.service.processMqttDownlink(foreignGateway,context,"!12345678",12002,5)==
         mt::mqtt::MqttDownlinkAdmission::Decision::LocalSource);
  auto wrongCiphertext=echo;wrongCiphertext.packet.encrypted.bytes[0]^=1;
  assert(f.service.processMqttDownlink(wrongCiphertext,context,"!12345678",12003,6)==
         mt::mqtt::MqttDownlinkAdmission::Decision::GatewayEcho);
  mt::Endpoint::Result result;
  assert(!f.service.takeMessageResult(0,result));
  assert(f.radio.sends==sendsBefore&&f.sink.calls==sinkBefore&&
         f.service.received==receivedBefore&&f.service.endpointControls()==controlsBefore);

  assert(f.service.processMqttDownlink(echo,context,"!12345678",12004,7)==
         mt::mqtt::MqttDownlinkAdmission::Decision::GatewayEcho);
  WireCompanionHost companionHost;mt::companion::CompanionWorkspace workspace;mt::Companion companion(f.service,companionHost,81,workspace);
  companionHandshake(companion,0x55aa,12004);assert(companion.handshakes==1);
  companionHost.records.clear();companion.poll(mt::Companion::WorkspaceProof{true});
  unsigned routingAcks=0,matchingNotifications=0;
  for(const auto &record:companionHost.records) {
    if(record.which_payload_variant==meshtastic_FromRadio_packet_tag&&
       record.packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
       record.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP) {
      ++routingAcks;assert(record.packet.decoded.request_id==packetId);
      meshtastic_Routing routing{};auto payload=pb_istream_from_buffer(
          record.packet.decoded.payload.bytes,record.packet.decoded.payload.size);
      assert(pb_decode(&payload,meshtastic_Routing_fields,&routing)&&!payload.bytes_left);
      assert(routing.which_variant==meshtastic_Routing_error_reason_tag&&
             routing.error_reason==meshtastic_Routing_Error_NONE);
    }
    if(record.which_payload_variant==meshtastic_FromRadio_clientNotification_tag&&
       record.clientNotification.reply_id==packetId) {
      ++matchingNotifications;assert(strstr(record.clientNotification.message,
        "Own MQTT echo matched locally; destination delivery unconfirmed")!=nullptr);
    }
  }
  assert(routingAcks==1&&matchingNotifications==1);
  assert(f.service.endpointFree()==4); // The exact reliable job retired once.
  assert(f.radio.sends==sendsBefore&&f.sink.calls==sinkBefore&&
         f.service.received==receivedBefore&&f.service.endpointControls()==controlsBefore);
  assert(f.service.processMqttDownlink(echo,context,"!12345678",12005,8)==
         mt::mqtt::MqttDownlinkAdmission::Decision::GatewayEcho);
  companionHost.records.clear();companion.poll(mt::Companion::WorkspaceProof{true}); // duplicate echo is idempotent
  unsigned duplicateAcks=0;
  for(const auto &record:companionHost.records)if(
      record.which_payload_variant==meshtastic_FromRadio_packet_tag&&
      record.packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
      record.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP)++duplicateAcks;
  assert(duplicateAcks==0);
  assert(f.radio.sends==sendsBefore&&f.service.endpointControls()==controlsBefore);

  Fixture nonReliable;nonReliable.enableDownlink();
  constexpr uint32_t noAckId=7602;const auto noAckApp=nonReliable.data('n');
  assert(!nonReliable.service.submitPublicApplication(0,Peer,noAckId,noAckApp.data(),noAckApp.size(),0,false,9500));
  nonReliable.service.poll(10000,1);nonReliable.radio.completionReady=true;
  nonReliable.radio.completionStatus=0;nonReliable.service.poll(11000,2);
  assert(nonReliable.service.takeMessageResult(0,result)&&result.id==noAckId&&
         result.kind==mt::Endpoint::Kind::Local);
  assert(nonReliable.sink.lastId==noAckId&&nonReliable.sink.lastLength>=17);
  Bytes noAckFrame(nonReliable.sink.lastFrame,
                   nonReliable.sink.lastFrame+nonReliable.sink.lastLength);
  mt::mqtt::MqttServiceEnvelopeDecoder::Decoded noAckEcho=echo;
  noAckEcho.packet.id=noAckId;noAckEcho.packet.want_ack=false;
  noAckEcho.packet.encrypted.size=pb_size_t(noAckFrame.size()-16);
  memcpy(noAckEcho.packet.encrypted.bytes,noAckFrame.data()+16,noAckFrame.size()-16);
  assert(nonReliable.service.processMqttDownlink(noAckEcho,context,"!12345678",12006,9)==
         mt::mqtt::MqttDownlinkAdmission::Decision::GatewayEcho);
  assert(!nonReliable.service.takeMessageResult(0,result)&&nonReliable.radio.sends==1);
  bench::wipeKeyBuffer(&echo,sizeof(echo));bench::wipeKeyBuffer(&wrongId,sizeof(wrongId));
  bench::wipeKeyBuffer(&foreignSource,sizeof(foreignSource));bench::wipeKeyBuffer(&foreignGateway,sizeof(foreignGateway));
  bench::wipeKeyBuffer(&wrongCiphertext,sizeof(wrongCiphertext));bench::wipeKeyBuffer(&noAckEcho,sizeof(noAckEcho));
  bench::wipeKeyBuffer(outbound.data(),outbound.size());bench::wipeKeyBuffer(noAckFrame.data(),noAckFrame.size());
  puts("PASS MQTT self-gateway echo locally completes exact reliable send once without RF or RX reinjection");
}

void downlinkMailboxCases() {
  mt::mqtt::DownlinkMailbox<2>::Slot slots[2]{};
  mt::mqtt::DownlinkMailbox<2> mailbox(slots);
  const uint8_t first[]={1,2,3},second[]={4,5};
  assert(mailbox.queued()==0);
  assert(mailbox.push("msh/2/e/A/!12345678",19,first,sizeof(first)));
  assert(mailbox.push("msh/2/e/B/!abcdef01",19,second,sizeof(second)));
  assert(!mailbox.push("msh/2/e/C/!abcdef02",19,first,sizeof(first))&&mailbox.queued()==2);
  mt::mqtt::DownlinkMessage out;
  assert(mailbox.pop(out)&&out.topicLength==19&&out.payloadLength==3&&out.payload[2]==3);
  assert(mailbox.pop(out)&&out.topicLength==19&&out.payloadLength==2&&out.payload[0]==4);
  assert(!mailbox.pop(out)&&mailbox.queued()==0);
  assert(!mailbox.push("x",1,first,0));
  puts("PASS bounded MQTT receive mailbox capacity, FIFO, exact copy and overflow refusal");
}

void consentFailureOrdering() {
  {
    CommitFixture f;f.openDirtyBatch();f.backend.failPrefix="mtbase/";
    assert(f.commit.commit(100)==-EIO);
    assert(f.backend.writes.size()==1&&f.backend.writes[0].rfind("mtbase/",0)==0);
    assert(f.mqttAdmin.dirty()&&!f.mqttJournal.config().configOkToMqtt());
    puts("PASS core journal failure prevents persistent MQTT consent write in shared open batch");
  }
  {
    CommitFixture f;f.openDirtyBatch();f.backend.failPrefix="mtmqtt/";
    assert(f.commit.commit(100)==-EIO);
    assert(f.backend.writes.size()==2&&f.backend.writes[0].rfind("mtbase/",0)==0&&
           f.backend.writes[1].rfind("mtmqtt/",0)==0);
    assert(f.backend.files.count(f.backend.writes[0])==1);
    assert(f.mqttJournal.state()==mt::network::MqttGatewayJournal::State::IoError);
    assert(!f.mqttJournal.config().configOkToMqtt());
    const size_t failedWrites=f.backend.writes.size();
    assert(f.mqttAdmin.dirty()&&f.mqttAdmin.commit(101)==-EACCES&&f.mqttAdmin.dirty());
    assert(f.commit.commit(102)!=0&&f.mqttAdmin.dirty()&&f.backend.writes.size()==failedWrites);
    puts("PASS core persists first; poisoned dirty MQTT consent remains visible and retries fail without rewriting");
  }
  {
    CommitFixture f;
    f.backend.files["mtmqtt/state0"]=Bytes(mt::network::MqttGatewayJournal::RecordSize,0xa5);
    assert(f.mqttJournal.load(f.backend)==-EINVAL&&!f.mqttJournal.ready()&&!f.mqttAdmin.dirty());
    assert(!f.core.begin(10));auto changed=f.service.config;assert(changed.setDisplay(45,true));
    assert(!f.core.save(changed,11));const size_t before=f.backend.writes.size();
    assert(!f.commit.commit(100)&&!f.mqttAdmin.dirty());
    for(size_t i=before;i<f.backend.writes.size();++i)assert(f.backend.writes[i].rfind("mtmqtt/",0)!=0);
    puts("PASS clean corrupt MQTT journal does not block unrelated core settings commit or trigger repair write");
  }
}

void mqttModuleReadbackCases() {
  CommitFixture f;
  const size_t initialWrites=f.backend.writes.size();
  assert(!f.commit.commit(10));
  assert(f.backend.writes.size()==initialWrites&&!f.core.open()&&!f.mqttAdmin.dirty());

  assert(!f.core.begin(20)&&f.core.open());
  auto coreConfig=f.service.config;assert(coreConfig.setDisplay(45,true));
  assert(!f.core.save(coreConfig,21)&&f.core.dirty());
  meshtastic_ModuleConfig_MQTTConfig input =
      meshtastic_ModuleConfig_MQTTConfig_init_zero;
  strcpy(input.address, "broker.proxy.invalid");
  strcpy(input.username, "synthetic-user");
  strcpy(input.password, "synthetic-password");
  strcpy(input.root, "lab/readback");
  input.enabled = true;
  input.tls_enabled = true; // TLS is delegated only because proxy mode is explicit.
  input.proxy_to_client_enabled = true;
  input.map_reporting_enabled = true;
  input.has_map_report_settings = true;
  input.map_report_settings.publish_interval_secs = 7200;
  input.map_report_settings.position_precision = 14;
  input.map_report_settings.should_report_location = true;

  // This is the production MqttGatewayAdmin SET path with direct runtime and
  // device TLS trust unavailable. Its policy and consent stay staged until
  // the shared SettingsCommit completes both journal writes.
  assert(f.mqttAdmin.save(input, true, input.proxy_to_client_enabled, false, 22) == 0);
  f.mqttAdmin.runtimePolicy(input.proxy_to_client_enabled, false);
  assert(f.mqttAdmin.saveConfigOkToMqtt(true, true, 23) == 0);
  assert(f.mqttAdmin.dirty()&&!f.mqttJournal.config().enabled()&&
         !f.mqttJournal.config().configOkToMqtt());
  assert(f.backend.writes.empty());
  assert(f.commit.commit(30) == 0);
  assert(!f.core.open()&&!f.mqttAdmin.dirty()&&f.mqttJournal.config().enabled()&&
         f.mqttJournal.config().proxyEnabled()&&f.mqttJournal.config().configOkToMqtt());

  // Exercise the production MQTT ModuleConfig wrapper used by ClientHost GET,
  // compiled once with and once without CONFIG_MT_MQTT_GATEWAY.
  meshtastic_ModuleConfig output{};
  assert(f.mqttAdmin.moduleConfig(output, meshtastic_ModuleConfig_mqtt_tag, true));
  assert(output.which_payload_variant == meshtastic_ModuleConfig_mqtt_tag);
  const auto &actual = output.payload_variant.mqtt;
  assert(actual.enabled && actual.proxy_to_client_enabled && actual.tls_enabled &&
         actual.map_reporting_enabled && actual.has_map_report_settings &&
         actual.map_report_settings.publish_interval_secs == 7200 &&
         actual.map_report_settings.position_precision == 14 &&
         actual.map_report_settings.should_report_location &&
         !strcmp(actual.address, input.address) &&
         !strcmp(actual.username, input.username) &&
         !strcmp(actual.password, input.password) && !strcmp(actual.root, input.root));
  assert(f.mqttJournal.config().valid()); // readback is of the saved settings.

  mt::network::MqttGatewayJournal restoredJournal;
  assert(restoredJournal.load(f.backend)==0);
  mt::network::MqttGatewayEdit restoredEdit(f.service,restoredJournal,f.backend);
  mt::network::MqttGatewayAdmin restoredAdmin(restoredEdit,f.core);
  assert(restoredEdit.load()&&restoredAdmin.loaded());
  meshtastic_ModuleConfig durable{};
  assert(restoredAdmin.moduleConfig(durable,meshtastic_ModuleConfig_mqtt_tag,true));
  assert(durable.payload_variant.mqtt.enabled&&durable.payload_variant.mqtt.proxy_to_client_enabled&&
         !strcmp(durable.payload_variant.mqtt.password,input.password));

  meshtastic_ModuleConfig untouched{};
  untouched.which_payload_variant = meshtastic_ModuleConfig_telemetry_tag;
  assert(!f.mqttAdmin.moduleConfig(untouched, meshtastic_ModuleConfig_mqtt_tag, false));
  assert(untouched.which_payload_variant == meshtastic_ModuleConfig_telemetry_tag);
  assert(!f.mqttAdmin.moduleConfig(untouched, meshtastic_ModuleConfig_telemetry_tag, true));
  assert(untouched.which_payload_variant == meshtastic_ModuleConfig_telemetry_tag);
  puts("PASS staged MQTT proxy SET/consent stays inactive until common commit, then durable GET/readback on both profiles");
}

void regionRootStagesWithCoreEdit() {
  CommitFixture f;
  meshtastic_ModuleConfig_MQTTConfig automatic{};
  strcpy(automatic.address,"mqtt.meshtastic.org");
  strcpy(automatic.username,"fixture-user");strcpy(automatic.password,"fixture-pass");
  strcpy(automatic.root,"msh/EU_868");
  automatic.enabled=true;automatic.tls_enabled=true;automatic.proxy_to_client_enabled=true;
  assert(!f.mqttAdmin.save(automatic,true,false,false,1));
  assert(!f.commit.commit(2));
  f.backend.writes.clear();

  f.service.stop();
  assert(!f.core.begin(10));
  auto unset=f.service.config;unset.bytes[4]=0;unset.setTxEnabled(unset.txEnabled());
  assert(unset.valid()&&!unset.provisioned());
  assert(!f.core.save(unset,11)&&f.core.dirty());
  assert(!f.mqttAdmin.applyRegionRootTopic("UNSET",5,true,12));
  assert(f.mqttAdmin.dirty()&&f.backend.writes.empty());
  meshtastic_ModuleConfig_MQTTConfig durable{};
  assert(mt::network::MqttGatewayConfigMapping::toConfig(f.mqttJournal.config(),durable));
  assert(!strcmp(durable.root,"msh/EU_868"));

  f.backend.failPrefix="mtbase/";
  assert(f.commit.commit(15)==-EIO);
  assert(f.mqttAdmin.dirty()&&f.core.open()&&f.backend.writes.size()==1);
  assert(f.backend.writes[0].rfind("mtbase/",0)==0);
  assert(mt::network::MqttGatewayConfigMapping::toConfig(f.mqttJournal.config(),durable));
  assert(!strcmp(durable.root,"msh/EU_868"));
  // The failed core journal is poisoned by its existing fail-closed rule;
  // do not attempt another commit on that fixture. A fresh fixture below
  // proves the successful common-commit path.

  CommitFixture committed;
  assert(!committed.mqttAdmin.save(automatic,true,false,false,1));
  assert(!committed.commit.commit(2));
  committed.backend.writes.clear();committed.service.stop();
  assert(!committed.core.begin(10));
  auto committedUnset=committed.service.config;committedUnset.bytes[4]=0;
  committedUnset.setTxEnabled(committedUnset.txEnabled());
  assert(committedUnset.valid()&&!committedUnset.provisioned());
  assert(!committed.core.save(committedUnset,11)&&committed.core.dirty());
  assert(!committed.mqttAdmin.applyRegionRootTopic("UNSET",5,true,12));
  assert(committed.mqttAdmin.dirty()&&committed.backend.writes.empty());
  assert(!committed.commit.commit(20));
  assert(!committed.mqttAdmin.dirty()&&!committed.core.open());
  assert(mt::network::MqttGatewayConfigMapping::toConfig(committed.mqttJournal.config(),durable));
  assert(!strcmp(durable.root,"msh/UNSET")&&durable.enabled&&durable.tls_enabled&&
         durable.proxy_to_client_enabled&&!strcmp(durable.address,automatic.address)&&
         !strcmp(durable.username,automatic.username)&&!strcmp(durable.password,automatic.password)&&
         committed.mqttJournal.config().configOkToMqtt());

  // The generated root is updated only for the default broker and a stock
  // automatic root. A custom root is preserved without a journal write.
  automatic={};
  strcpy(automatic.address,"broker.example");strcpy(automatic.root,"msh/custom");
  assert(!committed.mqttAdmin.save(automatic,true,false,false,30));
  committed.backend.writes.clear();
  assert(!committed.mqttAdmin.applyRegionRootTopic("US",2,true,31));
  assert(!committed.mqttAdmin.dirty()&&committed.backend.writes.empty());
  assert(mt::network::MqttGatewayConfigMapping::toConfig(committed.mqttJournal.config(),durable));
  assert(!strcmp(durable.address,"broker.example")&&!strcmp(durable.root,"msh/custom"));
  puts("PASS accepted region-root policy stages with core edit and leaves custom MQTT ownership untouched");
}

void companionRegionRootWriteFailureRetriesOnIdenticalRegion() {
  CommitFixture f;
  meshtastic_ModuleConfig_MQTTConfig automatic{};
  strcpy(automatic.address,"mqtt.meshtastic.org");strcpy(automatic.username,"fixture-user");
  strcpy(automatic.password,"fixture-pass");strcpy(automatic.root,"msh/EU_868");
  automatic.enabled=true;automatic.tls_enabled=true;automatic.proxy_to_client_enabled=true;
  assert(!f.mqttAdmin.save(automatic,true,true,false,1));
  assert(!f.mqttAdmin.saveConfigOkToMqtt(true,true,2));
  assert(!f.commit.commit(3));
  f.backend.writes.clear();

  RegionRetryHost host(f);mt::companion::CompanionWorkspace workspace;mt::Companion companion(f.service,host,81,workspace);
  companionHandshake(companion,0x12601,10);assert(companion.handshakes==1);host.records.clear();
  auto target=regionConfig(f.service.config,meshtastic_Config_LoRaConfig_RegionCode_UNSET,true);
  target.set_config.payload_variant.lora.tx_enabled=false;
  f.backend.failPrefix="mtmqtt/";
  sendCompanionAdmin(companion,host,target,201,20);
  assert(host.result(201)==meshtastic_Routing_Error_NO_INTERFACE);
  assert(f.service.config.bytes[4]==0&&f.mqttJournal.state()==mt::network::MqttGatewayJournal::State::IoError);
  assert(f.mqttAdmin.regionRootRetryPending());
  assert(!f.mqttAdmin.dirty()); // The failed candidate never became active or staged.
  meshtastic_ModuleConfig_MQTTConfig durable{};
  assert(mt::network::MqttGatewayConfigMapping::toConfig(f.mqttJournal.config(),durable));
  assert(!strcmp(durable.root,"msh/EU_868"));

  // A second identical request with the write still failing remains an error;
  // it must not ACK the already-persisted LoRa region as MQTT-reconciled.
  sendCompanionAdmin(companion,host,target,202,30);
  assert(host.result(202)==meshtastic_Routing_Error_NO_INTERFACE);
  assert(f.mqttJournal.state()==mt::network::MqttGatewayJournal::State::IoError&&!f.mqttAdmin.dirty());
  assert(f.mqttAdmin.regionRootRetryPending());
  assert(mt::network::MqttGatewayConfigMapping::toConfig(f.mqttJournal.config(),durable));
  assert(!strcmp(durable.root,"msh/EU_868"));

  f.backend.failPrefix.clear();
  sendCompanionAdmin(companion,host,target,203,40);
  assert(host.result(203)==meshtastic_Routing_Error_NONE);
  assert(host.rootAttempts==3&&f.service.config.bytes[4]==0&&f.mqttAdmin.ready()&&
         !f.mqttAdmin.dirty()&&!f.mqttAdmin.regionRootRetryPending()&&
         f.mqttJournal.state()==mt::network::MqttGatewayJournal::State::Loaded);
  assert(mt::network::MqttGatewayConfigMapping::toConfig(f.mqttJournal.config(),durable));
  assert(!strcmp(durable.root,"msh/UNSET")&&f.mqttAdmin.configOkToMqtt(true)&&
         durable.enabled&&durable.tls_enabled&&durable.proxy_to_client_enabled&&
         !strcmp(durable.address,automatic.address)&&!strcmp(durable.username,automatic.username)&&
         !strcmp(durable.password,automatic.password));
  assert(!f.service.receiving()&&!f.service.endpointArmed());
  puts("PASS actual Companion region setter reports MQTT write failure and identical retry reconciles root only after durable retry");
}

void companionNonRegionSettersDoNotReconcileMqttRoot() {
  CommitFixture f;
  meshtastic_ModuleConfig_MQTTConfig automatic{};
  strcpy(automatic.address,"mqtt.meshtastic.org");strcpy(automatic.root,"msh");
  automatic.enabled=true;automatic.tls_enabled=true;automatic.proxy_to_client_enabled=true;
  assert(!f.mqttAdmin.save(automatic,true,true,false,1));
  assert(!f.commit.commit(2));f.backend.writes.clear();
  RegionRetryHost host(f);mt::companion::CompanionWorkspace workspace;mt::Companion companion(f.service,host,81,workspace);
  companionHandshake(companion,0x12602,10);host.records.clear();

  auto identical=regionConfig(f.service.config,meshtastic_Config_LoRaConfig_RegionCode_US,false);
  sendCompanionAdmin(companion,host,identical,211,20);
  assert(host.result(211)==meshtastic_Routing_Error_NONE&&host.rootAttempts==0);
  assert(f.backend.writes.empty());
  meshtastic_ModuleConfig_MQTTConfig durable{};
  assert(mt::network::MqttGatewayConfigMapping::toConfig(f.mqttJournal.config(),durable));
  assert(!strcmp(durable.root,"msh"));

  auto hopOnly=regionConfig(f.service.config,meshtastic_Config_LoRaConfig_RegionCode_US,false);
  hopOnly.set_config.payload_variant.lora.hop_limit=uint32_t(f.service.config.hopLimit()+1);
  sendCompanionAdmin(companion,host,hopOnly,212,30);
  assert(host.result(212)==meshtastic_Routing_Error_NONE&&host.rootAttempts==0);
  assert(!f.backend.writes.empty());
  for(const auto &key:f.backend.writes)assert(key.rfind("mtmqtt/",0)!=0);
  assert(mt::network::MqttGatewayConfigMapping::toConfig(f.mqttJournal.config(),durable));
  assert(!strcmp(durable.root,"msh"));
  puts("PASS identical and non-region Companion setters do not rewrite managed MQTT roots");
}

void unrelatedMqttIoErrorDoesNotCreateRegionRetry() {
  CommitFixture f;
  meshtastic_ModuleConfig_MQTTConfig automatic{};
  strcpy(automatic.address,"mqtt.meshtastic.org");strcpy(automatic.root,"msh");
  automatic.enabled=true;automatic.tls_enabled=true;automatic.proxy_to_client_enabled=true;
  assert(!f.mqttAdmin.save(automatic,true,true,false,1));
  assert(!f.commit.commit(2));f.backend.writes.clear();
  meshtastic_ModuleConfig_MQTTConfig changed=automatic;strcpy(changed.root,"msh/custom");
  f.backend.failPrefix="mtmqtt/";
  assert(f.mqttAdmin.save(changed,true,true,false,3)==-EIO);
  assert(f.mqttJournal.state()==mt::network::MqttGatewayJournal::State::IoError);
  assert(!f.mqttAdmin.regionRootRetryPending());
  f.backend.failPrefix.clear();f.backend.writes.clear();

  RegionRetryHost host(f);mt::companion::CompanionWorkspace workspace;mt::Companion companion(f.service,host,81,workspace);
  companionHandshake(companion,0x12603,10);host.records.clear();
  auto identical=regionConfig(f.service.config,meshtastic_Config_LoRaConfig_RegionCode_US,false);
  sendCompanionAdmin(companion,host,identical,221,20);
  assert(host.result(221)==meshtastic_Routing_Error_NONE);
  assert(host.rootAttempts==0&&f.backend.writes.empty()&&!f.mqttAdmin.regionRootRetryPending());
  puts("PASS unrelated MQTT journal I/O failure does not schedule region-root reconciliation");
}
}

int main() {
  serviceUplinkCases();
  downlinkConsentPersistenceCases();
  channelBroadcastReachesPhone(true);
  channelBroadcastReachesPhone(false);
  downlinkServiceReceiveCases();
  mqttSelfEchoAcknowledgementCases();
  downlinkMailboxCases();
  consentFailureOrdering();
  mqttModuleReadbackCases();
  regionRootStagesWithCoreEdit();
  companionRegionRootWriteFailureRetriesOnIdenticalRegion();
  companionNonRegionSettersDoNotReconcileMqttRoot();
  unrelatedMqttIoErrorDoesNotCreateRegionRetry();
  puts("PASS MQTT gateway Service/SettingsCommit native fixture; no hardware");
}
