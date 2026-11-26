// SPDX-License-Identifier: GPL-3.0-only
#include "MqttServiceEnvelopeCodec.h"
#include "MtCodec.h"
#include "MtKeyCommand.h"
#include "RelayCore.h"
#include <initializer_list>
#include "network/MqttChannelUplink.h"
#include "network/MqttBootSnapshot.h"
#include "network/MqttTopicLevel.h"
#include "network/MqttPacketEligibility.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

using mt::mqtt::ServiceEnvelopeCodec;
using mt::network::MqttPacketEligibility;
using mt::network::MqttChannelUplink;

struct SinkCapture {
  char topic[160]{};
  char channel[64]{};
  char gateway[64]{};
  meshtastic_MeshPacket packet{};
  unsigned calls = 0;
};

bool capture(void *opaque, const char *topic, const meshtastic_MeshPacket *packet,
             const char *channel, const char *gateway) {
  auto &out = *static_cast<SinkCapture *>(opaque);
  ++out.calls;
  snprintf(out.topic, sizeof(out.topic), "%s", topic);
  snprintf(out.channel, sizeof(out.channel), "%s", channel);
  snprintf(out.gateway, sizeof(out.gateway), "%s", gateway);
  out.packet = *packet;
  return true;
}

void checkBridgeEnvelope(const SinkCapture &captured) {
  uint8_t wire[ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes]{};
  size_t written=0;
  assert(ServiceEnvelopeCodec::encode(&captured.packet,captured.channel,strlen(captured.channel),
      captured.gateway,strlen(captured.gateway),wire,sizeof(wire),&written));
  meshtastic_ServiceEnvelope envelope=meshtastic_ServiceEnvelope_init_zero;
  auto input=pb_istream_from_buffer(wire,written);
  assert(pb_decode(&input,meshtastic_ServiceEnvelope_fields,&envelope)&&!input.bytes_left);
  assert(!strcmp(envelope.channel_id,captured.channel)&&!strcmp(envelope.gateway_id,captured.gateway));
  assert(envelope.packet->which_payload_variant==captured.packet.which_payload_variant);
  if(captured.packet.which_payload_variant==meshtastic_MeshPacket_encrypted_tag) {
    assert(envelope.packet->encrypted.size==captured.packet.encrypted.size);
    assert(!memcmp(envelope.packet->encrypted.bytes,captured.packet.encrypted.bytes,
                   captured.packet.encrypted.size));
  }
  pb_release(meshtastic_ServiceEnvelope_fields,&envelope);
}

namespace {
struct Flow {
  MqttPacketEligibility::Context context{};
  meshtastic_MeshPacket cipher{};
  meshtastic_MeshPacket plain{};
  uint8_t canonical[239]{};
  size_t canonicalSize = 0;

  Flow() {
    context.mqttEnabled = context.transportReady = true;
    context.anyChannelUplinkEnabled = context.thisChannelUplinkEnabled = true;
    context.channelActive = true;
    context.channelIndex = 2;
    context.channelHash = 0x37;
    context.channelRole = 1;
    context.localNode = 0x12345678;
    cipher.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
    cipher.from = plain.from = 0x23456789;
    cipher.to = plain.to = 0xffffffffu;
    cipher.id = plain.id = 0x76543210;
    cipher.channel = 0x37; // raw channel hash differs from channel slot
    plain.channel = 2;
    cipher.encrypted.size = 4;
    const uint8_t encrypted[] = {0xa1, 0xb2, 0xc3, 0xd4};
    memcpy(cipher.encrypted.bytes, encrypted, sizeof(encrypted));
    plain.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    plain.decoded.portnum = static_cast<meshtastic_PortNum>(8);
    const uint8_t payload[] = {'s','e','c','r','e','t'};
    plain.decoded.payload.size = sizeof(payload);
    memcpy(plain.decoded.payload.bytes, payload, sizeof(payload));
    plain.decoded.has_bitfield = true;
    plain.decoded.bitfield = 1;
    assert(mt::PublicApplication::encode(plain.decoded.portnum, payload,
        sizeof(payload), 0, 0, canonical, sizeof(canonical), canonicalSize, 1));
  }

  MqttPacketEligibility::Decision send(bool encryptedMode) const {
    MqttPacketEligibility::Context c = context;
    c.encryptionEnabled = encryptedMode;
    return MqttPacketEligibility::evaluate(c, &cipher, &plain, canonical, canonicalSize);
  }

  void envelope(const meshtastic_MeshPacket &selected, const char *channelId) const {
    uint8_t wire[ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes]{};
    size_t size = 0;
    assert(ServiceEnvelopeCodec::encode(&selected, channelId, strlen(channelId),
                                        "!12345678", 9, wire, sizeof(wire), &size));
    meshtastic_ServiceEnvelope out = meshtastic_ServiceEnvelope_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(wire, size);
    assert(pb_decode(&stream, meshtastic_ServiceEnvelope_fields, &out));
    assert(stream.bytes_left == 0 && out.packet && out.channel_id && out.gateway_id);
    assert(strcmp(out.channel_id, channelId) == 0);
    assert(strcmp(out.gateway_id, "!12345678") == 0);
    assert(out.packet->id == selected.id && out.packet->channel == selected.channel);
    assert(out.packet->which_payload_variant == selected.which_payload_variant);
    if (selected.which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
      assert(out.packet->encrypted.size == selected.encrypted.size);
      assert(memcmp(out.packet->encrypted.bytes, selected.encrypted.bytes, selected.encrypted.size) == 0);
      // The only payload serialized is the ciphertext; the decoded application bytes are absent.
      static const uint8_t secret[] = {'s','e','c','r','e','t'};
      for (size_t i = 0; i + sizeof(secret) <= size; ++i)
        assert(memcmp(wire + i, secret, sizeof(secret)) != 0);
    }
    pb_release(meshtastic_ServiceEnvelope_fields, &out);
  }
};

struct BridgeFixture {
  mt::Config config{};
  bench::MtCodec codec{};
  uint8_t key[16]{};
  uint8_t data[239]{};
  uint8_t frame[255]{};
  size_t dataSize = 0, frameSize = 0;
  bench::RouteMetadata metadata{};
  MqttPacketEligibility::Context context{};

  BridgeFixture(uint32_t port=1, uint8_t permission=1, bool secondary=false) {
    for (unsigned i=0;i<sizeof(key);++i) key[i]=uint8_t(0x31+i);
    assert(codec.setKey(0x37,key,sizeof(key)));
    config.bytes[4]=1;
    auto node=config.node();node.setPublic();config.node(node);
    assert(config.setPrimaryName("field-ops"));
    if (secondary) {
      mt::ChannelRecord record;
      assert(record.set(2,"trail-team",key,sizeof(key)));
      assert(config.setSecondaryChannel(2,record));
    }
    mt::PublicApplication::encode(port,reinterpret_cast<const uint8_t *>("data"),4,
      0,0,data,sizeof(data),dataSize,permission);
    // Ports added by MqttDataPolicy may be outside PublicApplication's smaller
    // allowlist, so encode those stock Data records directly with nanopb.
    if (port==1||port==3||port==4||port==67) {
      meshtastic_Data d=meshtastic_Data_init_zero;
      d.portnum=static_cast<meshtastic_PortNum>(port);d.has_bitfield=true;d.bitfield=permission;
      d.payload.size=4;memcpy(d.payload.bytes,"data",4);
      auto stream=pb_ostream_from_buffer(data,sizeof(data));
      assert(pb_encode(&stream,meshtastic_Data_fields,&d));dataSize=stream.bytes_written;
    }
    assert(codec.encodeZeroHopData(0x23456789,0xffffffffu,0x76543210,
      data,dataSize,frame,sizeof(frame),frameSize));
    assert(codec.decode(frame,frameSize,metadata));
    context.mqttEnabled=context.transportReady=true;
    context.anyChannelUplinkEnabled=context.thisChannelUplinkEnabled=true;
    context.channelActive=true;context.channelIndex=2;context.channelRole=2;
    context.channelHash=0x37;
    context.localNode=0x12345678;
  }
  void permission(uint8_t value) {
    meshtastic_Data decoded{};auto in=pb_istream_from_buffer(data,dataSize);
    assert(pb_decode(&in,meshtastic_Data_fields,&decoded)&&!in.bytes_left);
    decoded.has_bitfield=true;decoded.bitfield=value;
    auto out=pb_ostream_from_buffer(data,sizeof(data));
    assert(pb_encode(&out,meshtastic_Data_fields,&decoded));dataSize=out.bytes_written;
    assert(codec.encodeZeroHopData(0x23456789,0xffffffffu,0x76543210,
      data,dataSize,frame,sizeof(frame),frameSize));
    assert(codec.decode(frame,frameSize,metadata));
  }
  bool publish(SinkCapture &sink, const char *root="", unsigned index=2) {
    return MqttChannelUplink::publish(codec,config,index,frame,frameSize,metadata,
      context,root,"!12345678",capture,&sink);
  }
};

void testBridge() {
  char topic[160]{};
  assert(MqttChannelUplink::topic("", "field-ops", "!12345678", topic, sizeof(topic)));
  assert(!strcmp(topic,"msh/2/e/field-ops/!12345678"));
  assert(MqttChannelUplink::topic("custom", "trail-team", "!abcdef01", topic, sizeof(topic)));
  assert(!strcmp(topic,"custom/2/e/trail-team/!abcdef01"));
  assert(!MqttChannelUplink::topic("mesh+", "field-ops", "!abcdef01", topic, sizeof(topic)));
  assert(!MqttChannelUplink::topic("mesh/#", "field-ops", "!abcdef01", topic, sizeof(topic)));
  assert(!MqttChannelUplink::topic("custom", "bad/name", "!abcdef01", topic, sizeof(topic)));

  BridgeFixture plain(1,1,true);
  SinkCapture first{};
  assert(plain.publish(first,"",2)&&first.calls==1);
  assert(!strcmp(first.topic,"msh/2/e/trail-team/!12345678"));
  assert(!strcmp(first.channel,"trail-team")&&first.packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag);
  checkBridgeEnvelope(first);
  plain.context.encryptionEnabled=true;
  SinkCapture encrypted{};
  assert(plain.publish(encrypted,"root",2)&&encrypted.calls==1);
  assert(!strcmp(encrypted.topic,"root/2/e/trail-team/!12345678"));
  assert(encrypted.packet.which_payload_variant==meshtastic_MeshPacket_encrypted_tag);
  assert(encrypted.packet.encrypted.size==plain.frameSize-16);
  assert(!memcmp(encrypted.packet.encrypted.bytes,plain.frame+16,plain.frameSize-16));
  checkBridgeEnvelope(encrypted);
  BridgeFixture names(4,1,true);
  meshtastic_User user{};strcpy(user.id,"!00000099");strcpy(user.long_name,"Field");
  meshtastic_Data message{};message.portnum=meshtastic_PortNum_NODEINFO_APP;
  message.has_bitfield=true;message.bitfield=1;
  auto body=pb_ostream_from_buffer(message.payload.bytes,sizeof(message.payload.bytes));
  assert(pb_encode(&body,meshtastic_User_fields,&user));message.payload.size=body.bytes_written;
  auto envelope=pb_ostream_from_buffer(names.data,sizeof(names.data));
  assert(pb_encode(&envelope,meshtastic_Data_fields,&message)); names.dataSize=envelope.bytes_written;
  assert(names.codec.encodeZeroHopData(0x23456789,UINT32_MAX,0x76543210,names.data,names.dataSize,
    names.frame,sizeof(names.frame),names.frameSize)&&names.codec.decode(names.frame,names.frameSize,names.metadata));
  SinkCapture normalized{};
  assert(names.publish(normalized,"root",2));
  const auto &delivered=normalized.packet.decoded.payload;
  auto read=pb_istream_from_buffer(delivered.bytes,delivered.size);
  assert(pb_decode(&read,meshtastic_User_fields,&user)&&!strcmp(user.id,"!23456789")&&!strcmp(user.long_name,"Field"));
  names.context.encryptionEnabled=true;
  SinkCapture wire{};
  assert(names.publish(wire,"root",2)&&wire.packet.encrypted.size==names.frameSize-16&&
    !memcmp(wire.packet.encrypted.bytes,names.frame+16,names.frameSize-16));
  // Service owns signature verification. Its decision must agree with the
  // complete signed frame; a decoded broker publication may not rewrite it.
  strcpy(user.id,"!00000099");user.public_key.size=32;memset(user.public_key.bytes,0x2a,32);
  body=pb_ostream_from_buffer(message.payload.bytes,sizeof(message.payload.bytes));
  assert(pb_encode(&body,meshtastic_User_fields,&user));message.payload.size=body.bytes_written;
  message.xeddsa_signature.size=64;memset(message.xeddsa_signature.bytes,0x35,64);
  envelope=pb_ostream_from_buffer(names.data,sizeof(names.data));
  assert(pb_encode(&envelope,meshtastic_Data_fields,&message));names.dataSize=envelope.bytes_written;
  assert(names.codec.encodeSignedNodeInfo(0x23456789,UINT32_MAX,0x76543210,names.data,names.dataSize,
    names.frame,sizeof(names.frame),names.frameSize)&&names.codec.decode(names.frame,names.frameSize,names.metadata));
  names.context.encryptionEnabled=false;
  SinkCapture signedPub{};
  assert(!names.publish(signedPub,"root",2));
  names.metadata.signatureVerified=true;
  assert(names.publish(signedPub,"root",2)&&signedPub.packet.decoded.xeddsa_signature.size==64&&
    !memcmp(signedPub.packet.decoded.xeddsa_signature.bytes,message.xeddsa_signature.bytes,64)&&
    signedPub.packet.decoded.payload.size==message.payload.size&&
    !memcmp(signedPub.packet.decoded.payload.bytes,message.payload.bytes,message.payload.size));
  names.metadata.signature[0]^=1;
  assert(!names.publish(signedPub,"root",2));
  static const uint8_t clear[]={'d','a','t','a'};
  assert(encrypted.packet.encrypted.size<sizeof(clear)||
         memcmp(encrypted.packet.encrypted.bytes,clear,sizeof(clear))!=0);
  SinkCapture wrongChannel{};
  assert(!plain.publish(wrongChannel,"root",1)&&wrongChannel.calls==0);

  BridgeFixture optedOut(1,1,true);
  SinkCapture baseline{},denied{};assert(optedOut.publish(baseline));
  optedOut.permission(0);assert(!optedOut.publish(denied)&&!denied.calls);
  BridgeFixture viaMqtt(1,1,true);
  assert(viaMqtt.publish(baseline));viaMqtt.frame[12]|=0x10;
  assert(!viaMqtt.publish(denied)&&!denied.calls);
  BridgeFixture badHeader(1,1,true);
  assert(badHeader.publish(baseline));badHeader.frame[12]|=0x80;
  assert(badHeader.publish(baseline)); // valid hop_start=4, hop_limit=0
  badHeader.frame[12]=uint8_t(3|(1<<5)); // hop_limit exceeds hop_start
  assert(!badHeader.publish(denied)&&!denied.calls);
  BridgeFixture malformed(1,1,true);
  assert(malformed.publish(baseline));
  // Unknown Data field is parseable by the generic decoder but cannot be
  // reconstructed byte-for-byte as an allowed canonical application record.
  malformed.data[malformed.dataSize++]=0x58;malformed.data[malformed.dataSize++]=0;
  assert(malformed.codec.encodeZeroHopData(0x23456789,0xffffffffu,0x76543210,
    malformed.data,malformed.dataSize,malformed.frame,sizeof(malformed.frame),malformed.frameSize));
  assert(malformed.codec.decode(malformed.frame,malformed.frameSize,malformed.metadata));
  assert(!malformed.publish(denied)&&!denied.calls);
  BridgeFixture mismatch(1,1,true);
  assert(mismatch.publish(baseline));mismatch.metadata.payload[0]^=1;
  assert(!mismatch.publish(denied)&&!denied.calls);
  BridgeFixture telemetry(67,1,true);
  SinkCapture telemetryOut{};
  assert(telemetry.publish(telemetryOut)&&telemetryOut.calls==1);
}

void testBootSnapshotAndDuplicateWindow() {
  assert(mt::mqtt::MqttTopicLevel::validCString("trail-team"));
  assert(mt::mqtt::MqttTopicLevel::valid("caf\xc3\xa9",5));
  assert(!mt::mqtt::MqttTopicLevel::validCString(""));
  assert(!mt::mqtt::MqttTopicLevel::validCString("a/b"));
  assert(!mt::mqtt::MqttTopicLevel::validCString("a+"));
  assert(!mt::mqtt::MqttTopicLevel::validCString("a#"));
  const char nulLevel[]={'a','\0','b'};
  const char controlLevel[]={'a',1,'b'};
  const char delLevel[]={'a',char(0x7f),'b'};
  const char c1Level[]={'a',char(0xc2),char(0x80),'b'};
  const char strayContinuation[]={char(0x80)};
  const char overlong[]={char(0xc0),char(0xaf)};
  const char surrogate[]={char(0xed),char(0xa0),char(0x80)};
  const char tooHigh[]={char(0xf4),char(0x90),char(0x80),char(0x80)};
  const char truncated[]={char(0xe2),char(0x82)};
  assert(!mt::mqtt::MqttTopicLevel::valid(nulLevel,sizeof(nulLevel)));
  assert(!mt::mqtt::MqttTopicLevel::valid(controlLevel,sizeof(controlLevel)));
  assert(!mt::mqtt::MqttTopicLevel::valid(delLevel,sizeof(delLevel)));
  assert(!mt::mqtt::MqttTopicLevel::valid(c1Level,sizeof(c1Level)));
  assert(!mt::mqtt::MqttTopicLevel::valid(strayContinuation,sizeof(strayContinuation)));
  assert(!mt::mqtt::MqttTopicLevel::valid(overlong,sizeof(overlong)));
  assert(!mt::mqtt::MqttTopicLevel::valid(surrogate,sizeof(surrogate)));
  assert(!mt::mqtt::MqttTopicLevel::valid(tooHigh,sizeof(tooHigh)));
  assert(!mt::mqtt::MqttTopicLevel::valid(truncated,sizeof(truncated)));
  char maxLevel[mt::mqtt::MqttTopicLevel::MaxBytes+1];memset(maxLevel,'x',sizeof(maxLevel));
  assert(mt::mqtt::MqttTopicLevel::valid(maxLevel,mt::mqtt::MqttTopicLevel::MaxBytes));
  assert(!mt::mqtt::MqttTopicLevel::valid(maxLevel,sizeof(maxLevel)));

  mt::mqtt::RestartPolicy restart;restart.boot(5);
  restart.saved(0,5,5,false);assert(!restart.pending()&&!restart.due(0,false,false));
  restart.saved(-EIO,5,6,true);assert(!restart.pending());
  restart.saved(0,5,5,true);assert(restart.pending());
  assert(!restart.due(0,true,true)&&!restart.due(-EIO,false,false)&&!restart.due(0,false,true));
  assert(restart.due(0,false,false));
  restart.saved(0,5,5,false);assert(!restart.pending()); // staged value restored
  restart.saved(0,5,5,true);restart.observed(6,false); // MQTT committed before a later journal failure
  assert(restart.pending()&&!restart.due(-EIO,false,false));
  restart.saved(0,6,6,false);assert(restart.due(0,false,false)); // no-op cannot erase durable change
  mt::mqtt::RestartPolicy durable;durable.boot(0xffffffffu);
  durable.saved(0,0xffffffffu,0,false);assert(durable.due(0,false,false));

  char host[64]{};uint16_t port=0;
  assert(mt::mqtt::BootSnapshot::broker("broker.example",false,host,sizeof(host),port));
  assert(!strcmp(host,"broker.example")&&port==1883);
  assert(mt::mqtt::BootSnapshot::broker("broker.example",true,host,sizeof(host),port)&&port==8883);
  assert(mt::mqtt::BootSnapshot::broker("broker.example:65535",false,host,sizeof(host),port)&&port==65535);
  assert(mt::mqtt::BootSnapshot::broker("broker.example:1",true,host,sizeof(host),port)&&port==1);
  assert(!mt::mqtt::BootSnapshot::broker("broker.example:0",false,host,sizeof(host),port));
  assert(!mt::mqtt::BootSnapshot::broker("broker.example:65536",false,host,sizeof(host),port));
  assert(!mt::mqtt::BootSnapshot::broker("broker.example:999999999999999999999999",false,host,sizeof(host),port));
  assert(!mt::mqtt::BootSnapshot::broker("broker.example:",false,host,sizeof(host),port));
  assert(!mt::mqtt::BootSnapshot::broker("broker.example:18x3",false,host,sizeof(host),port));
  assert(!mt::mqtt::BootSnapshot::broker("bad/host",false,host,sizeof(host),port));
  char tiny[4]{};
  assert(!mt::mqtt::BootSnapshot::broker("broker.example",false,tiny,sizeof(tiny),port));

  meshtastic_ModuleConfig_MQTTConfig request=meshtastic_ModuleConfig_MQTTConfig_init_zero;
  request.enabled=true;strcpy(request.address,"broker.example");strcpy(request.root,"mesh");
  mt::network::MqttGatewaySettings settings;
  assert(settings.assign(request));
  mt::Config channels;assert(channels.valid()&&!channels.anyChannelDownlinkEnabled());
  mt::mqtt::BootSnapshot boot;
  assert(!boot.capture(settings,0x12345678,false,true,channels)); // runtime not admitted
  assert(boot.capture(settings,0x12345678,true,true,channels));
  assert(!strcmp(boot.settings().host,"broker.example")&&boot.settings().port==1883);
  assert(boot.settings().subscriptionTopicCount==0&&boot.downlinkSubscriptionsMatch(channels));
  assert(!strcmp(boot.gateway(),"!12345678")&&!strcmp(boot.root(),"mesh"));
  assert(channels.setPrimaryDownlinkEnabled(true));
  mt::mqtt::BootSnapshot consented;
  assert(consented.capture(settings,0x12345678,true,true,channels));
  mt::ChannelRecord primary;assert(channels.channelRecord(0,primary));
  const char *channelName=primary.name()[0]?primary.name():channels.modemName();
  char expectedTopic[80]{};snprintf(expectedTopic,sizeof(expectedTopic),"mesh/2/e/%s/+",channelName);
  assert(consented.settings().subscriptionTopicCount==2&&
         !strcmp(consented.settings().subscriptionTopics[0],expectedTopic)&&
         !strcmp(consented.settings().subscriptionTopics[1],"mesh/2/e/PKI/+")&&
         consented.downlinkSubscriptionsMatch(channels));
  auto revoked=channels;assert(revoked.setPrimaryDownlinkEnabled(false));
  assert(!consented.downlinkSubscriptionsMatch(revoked));

  // Seven secondary channels plus primary can each install an ordinary
  // filter; the shared PKI filter is the ninth bounded subscription.
  mt::Config allChannels;
  allChannels.bytes[4]=1;
  auto allNode=allChannels.node();allNode.setPublic();allChannels.node(allNode);
  assert(allChannels.setPrimaryName("primary"));
  assert(allChannels.setPrimaryDownlinkEnabled(true));
  uint8_t channelKey[16]{};
  for(unsigned i=1;i<8;++i) {
    char name[8]{};snprintf(name,sizeof(name),"ch%u",i);
    mt::ChannelRecord record;
    assert(record.set(2,name,channelKey,sizeof(channelKey),0,false,false,true));
    assert(allChannels.setSecondaryChannel(i,record));
  }
  mt::mqtt::BootSnapshot allConsent;
  assert(allConsent.capture(settings,0x12345678,true,true,allChannels));
  assert(allConsent.subscriptionTopicCount()==mt::mqtt::Settings::MaxSubscriptionTopics);
  assert(allConsent.settings().subscriptionTopicCount==9);
  assert(!strcmp(allConsent.subscriptionTopic(8),"mesh/2/e/PKI/+"));
  assert(allConsent.downlinkSubscriptionsMatch(allChannels));

  // If a consented channel itself is named PKI, retain one exact filter.
  mt::Config pkiNamed;
  pkiNamed.bytes[4]=1;
  auto pkiNode=pkiNamed.node();pkiNode.setPublic();pkiNamed.node(pkiNode);
  assert(pkiNamed.setPrimaryName("PKI"));
  assert(pkiNamed.setPrimaryDownlinkEnabled(true));
  mt::mqtt::BootSnapshot pkiNamedSnapshot;
  assert(pkiNamedSnapshot.capture(settings,0x12345678,true,true,pkiNamed));
  assert(pkiNamedSnapshot.subscriptionTopicCount()==1);
  assert(!strcmp(pkiNamedSnapshot.subscriptionTopic(0),"mesh/2/e/PKI/+"));
  assert(pkiNamedSnapshot.downlinkSubscriptionsMatch(pkiNamed));
  for(const char *unsafeName:{"bad/name","bad+name","bad#name"}) {
    auto unsafeChannels=channels;mt::ChannelRecord secondary,primaryRecord;
    assert(unsafeChannels.channelRecord(0,primaryRecord));
    assert(secondary.set(2,unsafeName,primaryRecord.key(),primaryRecord.keySize(),0,false,false,true));
    assert(unsafeChannels.setSecondaryChannel(1,secondary));
    mt::mqtt::BootSnapshot unsafeSnapshot;
    assert(!unsafeSnapshot.capture(settings,0x12345678,true,true,unsafeChannels));
  }
  mt::mqtt::BootSnapshot formatted;
  assert(formatted.capture(settings,0xabcdef01,true,true,channels));
  assert(!strcmp(formatted.gateway(),"!abcdef01"));
  request.tls_enabled=true;strcpy(request.address,"broker.example:65535");
  assert(settings.assign(request));
  mt::mqtt::BootSnapshot secure;
  assert(!secure.capture(settings,0x12345678,true,false,channels)); // TLS trust not ready
  assert(secure.capture(settings,0x12345678,true,true,channels)&&secure.settings().port==65535);
  request=meshtastic_ModuleConfig_MQTTConfig_init_zero;
  request.enabled=true;strcpy(request.address,"broker.example");
  mt::network::MqttGatewaySettings noRootSettings;
  assert(noRootSettings.assign(request));
  mt::mqtt::BootSnapshot badNode;
  assert(!badNode.capture(noRootSettings,1,true,true,channels));
  assert(!badNode.capture(noRootSettings,0xfffffffeu,true,true,channels));

  mt::mqtt::UplinkWindow window;
  constexpr uint64_t before32=uint64_t(UINT32_MAX)-10;
  assert(!window.contains(7,9,2,before32)); // unadmitted packets are not remembered
  window.accepted(7,9,2,before32);
  assert(window.contains(7,9,2,before32));
  assert(window.contains(7,9,2,uint64_t(UINT32_MAX)+100)); // crosses 32-bit tick wrap
  assert(window.contains(7,9,2,before32+59999));
  assert(!window.contains(7,9,2,before32+60000)); // exact expiry boundary
  assert(!window.contains(7,9,3,before32+1));
  assert(!window.contains(8,9,2,before32+1));
  assert(!window.contains(7,10,2,before32+1));
  assert(!window.contains(7,9,2,before32-1)); // backward clock is not treated as fresh
  constexpr uint64_t above32=(uint64_t(1)<<48)+123;
  window.accepted(11,12,4,above32);
  assert(window.contains(11,12,4,above32+59999));
  assert(!window.contains(11,12,4,above32+60000));
}
}

int main() {
  Flow f;
  assert(f.send(false) == MqttPacketEligibility::Decision::CanonicalDecodedPublic);
  f.envelope(f.plain, "field-ops");
  assert(f.send(true) == MqttPacketEligibility::Decision::OriginalEncryptedEnvelope);
  f.envelope(f.cipher, "field-ops");

  // A channel-specific ServiceEnvelope cannot silently inherit another channel's id.
  uint8_t wire[ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes]{};
  size_t n = 0;
  assert(ServiceEnvelopeCodec::encode(&f.cipher, "channel-b", 9, "!12345678", 9,
                                      wire, sizeof(wire), &n));
  meshtastic_ServiceEnvelope env = meshtastic_ServiceEnvelope_init_zero;
  pb_istream_t input = pb_istream_from_buffer(wire, n);
  assert(pb_decode(&input, meshtastic_ServiceEnvelope_fields, &env));
  assert(strcmp(env.channel_id, "channel-b") == 0);
  pb_release(meshtastic_ServiceEnvelope_fields, &env);

  Flow denied;
  denied.plain.decoded.bitfield = 0; // explicit uplink permission bit absent
  assert(denied.send(false) == MqttPacketEligibility::Decision::Deny);
  Flow loop;
  loop.cipher.via_mqtt = true;
  assert(loop.send(false) == MqttPacketEligibility::Decision::Deny);
  Flow pki;
  pki.cipher.pki_encrypted = true;
  assert(pki.send(true) == MqttPacketEligibility::Decision::Deny);
  Flow json;
  json.context.jsonEnabled = true;
  // Stock keeps protobuf uplink admission independent of this legacy flag.
  assert(json.send(false) == MqttPacketEligibility::Decision::CanonicalDecodedPublic);
  Flow proxy;
  proxy.context.proxyEnabled = true;
  assert(proxy.send(false) == MqttPacketEligibility::Decision::Deny);
  Flow map;
  map.context.mapReportingEnabled = true;
  assert(map.send(false) == MqttPacketEligibility::Decision::Deny);
  Flow canonical;
  canonical.canonical[canonical.canonicalSize++] = 0x58;
  canonical.canonical[canonical.canonicalSize++] = 0;
  assert(canonical.send(false) == MqttPacketEligibility::Decision::Deny);
  testBridge();
  testBootSnapshotAndDuplicateWindow();
  puts("PASS integrated MQTT uplink decision and stock ServiceEnvelope contracts");
}
