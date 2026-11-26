// SPDX-License-Identifier: GPL-3.0-only
// Native policy checks for encrypted PKI MQTT ingress. No radio or crypto.
#include "network/MqttDownlinkAdmission.h"
#include "MqttServiceEnvelopeCodec.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using mt::mqtt::MqttDownlinkAdmission;
using mt::mqtt::MqttServiceEnvelopeDecoder;
using mt::mqtt::ServiceEnvelopeCodec;

namespace {
constexpr uint32_t Local=0x12345678u,Peer=0x24680135u;

struct Fixture {
  meshtastic_MeshPacket packet=meshtastic_MeshPacket_init_zero;
  MqttServiceEnvelopeDecoder::Decoded envelope{};
  MqttDownlinkAdmission::Context context{};

  Fixture() {
    packet.from=Peer;packet.to=Local;packet.id=0x76543210u;
    packet.channel=0;packet.hop_limit=2;packet.hop_start=4;
    packet.which_payload_variant=meshtastic_MeshPacket_encrypted_tag;
    packet.encrypted.size=13;
    for(size_t i=0;i<packet.encrypted.size;++i)packet.encrypted.bytes[i]=uint8_t(0x50+i);
    // Remote auth annotations are claims. The private runtime establishes
    // identity after AEAD; these fields must not be used as preflight proof.
    context.mqttEnabled=true;context.brokerConfigured=true;context.transportReady=true;
    context.channelDownlinkEnabled=true; // stock any-channel downlink consent
    context.receivePathReady=true;context.privateRuntimeReady=true;
    context.pinnedPrivatePeer=true;context.localNode=Local;
    context.localGatewayId="!12345678";context.localGatewayIdLength=9;
    encode("PKI","!abcdef01");
  }

  void encode(const char *channel,const char *gateway) {
    uint8_t wire[ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes]{};size_t size=0;
    assert(ServiceEnvelopeCodec::encode(&packet,channel,strlen(channel),gateway,
      strlen(gateway),wire,sizeof(wire),&size));
    envelope={};
    assert(MqttServiceEnvelopeDecoder::decode(wire,size,&envelope));
    assert(envelope.packet.from==packet.from&&envelope.packet.to==packet.to&&
      envelope.packet.id==packet.id&&envelope.kind==MqttServiceEnvelopeDecoder::PacketKind::kEncrypted);
  }

  MqttDownlinkAdmission::Decision evaluate() const {
    return MqttDownlinkAdmission::evaluate(context,envelope);
  }
};

const char *name(MqttDownlinkAdmission::Decision d) {
  using D=MqttDownlinkAdmission::Decision;
  switch(d) {
    case D::Allow:return "Allow";case D::Disabled:return "Disabled";
    case D::Unconfigured:return "Unconfigured";case D::TransportUnavailable:return "TransportUnavailable";
    case D::Ignored:return "Ignored";case D::NoDownlinkConsent:return "NoDownlinkConsent";
    case D::UnsupportedChannel:return "UnsupportedChannel";case D::ChannelMismatch:return "ChannelMismatch";
    case D::KeyMismatch:return "KeyMismatch";case D::GatewayEcho:return "GatewayEcho";
    case D::LocalSource:return "LocalSource";case D::ReceiveUnavailable:return "ReceiveUnavailable";
    case D::InvalidPacket:return "InvalidPacket";case D::UnsupportedPacket:return "UnsupportedPacket";
    case D::PrivateRuntimeUnavailable:return "PrivateRuntimeUnavailable";
    case D::UnpinnedPrivatePeer:return "UnpinnedPrivatePeer";
    case D::PrivateDestinationMismatch:return "PrivateDestinationMismatch";
    case D::PrivateTransitUnavailable:return "PrivateTransitUnavailable";
    case D::UnknownPrivateTransitPeer:return "UnknownPrivateTransitPeer";
  }
  return "UnknownDecision";
}

void require(MqttDownlinkAdmission::Decision expected,const Fixture &f,int line) {
  const auto actual=f.evaluate();
  if(actual!=expected) {
    const auto &p=f.envelope.packet;const auto &c=f.context;
    std::fprintf(stderr,"admission mismatch case=line-%d expected=%s(%u) actual=%s(%u) channel=%.*s gateway=%.*s from=%08x to=%08x id=%u packetChannel=%u payloadTag=%u cipher=%u local=%08x receive=%u privateReady=%u pinned=%u consent=%u mqtt=%u broker=%u transport=%u tls=%u tlsTrust=%u proxy=%u proxyEnabled=%u ignore=%u\n",
      line,name(expected),unsigned(expected),name(actual),unsigned(actual),
      int(f.envelope.channelIdLength),f.envelope.channelId,
      int(f.envelope.gatewayIdLength),f.envelope.gatewayId,p.from,p.to,p.id,
      unsigned(p.channel),unsigned(p.which_payload_variant),unsigned(p.encrypted.size),c.localNode,
      unsigned(c.receivePathReady),unsigned(c.privateRuntimeReady),unsigned(c.pinnedPrivatePeer),
      unsigned(c.channelDownlinkEnabled),unsigned(c.mqttEnabled),unsigned(c.brokerConfigured),
      unsigned(c.transportReady),unsigned(c.tlsEnabled),unsigned(c.tlsTrustReady),
      unsigned(c.phoneProxyMode),unsigned(c.proxyModeEnabled),unsigned(c.ignoreMqtt));
    std::abort();
  }
}
}
#define REQUIRE(expected, fixture) require((expected),(fixture),__LINE__)

int main() {
  Fixture f;
  static const char topic[]="lab/root/2/e/PKI/!abcdef01";
  assert(MqttDownlinkAdmission::topicMatches("lab/root",topic,sizeof(topic)-1,f.envelope));
  assert(!MqttDownlinkAdmission::topicMatches("other",topic,sizeof(topic)-1,f.envelope));
  auto common=f.context;f.context.mqttEnabled=false;
  REQUIRE(MqttDownlinkAdmission::Decision::Disabled,f);
  f.context=common;f.context.brokerConfigured=false;
  REQUIRE(MqttDownlinkAdmission::Decision::Unconfigured,f);
  f.context=common;f.context.transportReady=false;
  REQUIRE(MqttDownlinkAdmission::Decision::TransportUnavailable,f);
  f.context=common;
  REQUIRE(MqttDownlinkAdmission::Decision::Allow,f);

  auto c=f.context;c.channelDownlinkEnabled=false;f.context=c;
  REQUIRE(MqttDownlinkAdmission::Decision::NoDownlinkConsent,f);
  f.context.channelDownlinkEnabled=true;
  f.context.pinnedPrivatePeer=false;
  REQUIRE(MqttDownlinkAdmission::Decision::UnpinnedPrivatePeer,f);
  // Claimed pki_encrypted and a supplied public key do not replace the pin.
  f.envelope.packet.pki_encrypted=true;f.envelope.packet.public_key.size=32;
  memset(f.envelope.packet.public_key.bytes,0xa5,32);
  REQUIRE(MqttDownlinkAdmission::Decision::UnpinnedPrivatePeer,f);
  f.context.pinnedPrivatePeer=true;
  f.envelope.packet.via_mqtt=true;
  REQUIRE(MqttDownlinkAdmission::Decision::Allow,f);
  f.envelope.packet.via_mqtt=false;f.envelope.packet.pki_encrypted=false;
  f.envelope.packet.public_key={};

  f.context.ignoreMqtt=true;REQUIRE(MqttDownlinkAdmission::Decision::Ignored,f);
  f.context.ignoreMqtt=false;
  f.context.tlsEnabled=true;f.context.tlsTrustReady=false;
  REQUIRE(MqttDownlinkAdmission::Decision::TransportUnavailable,f);
  f.context.tlsTrustReady=true;
  REQUIRE(MqttDownlinkAdmission::Decision::Allow,f);
  f.context.phoneProxyMode=true;f.context.proxyModeEnabled=false;
  REQUIRE(MqttDownlinkAdmission::Decision::TransportUnavailable,f);
  f.context.proxyModeEnabled=true;f.context.tlsEnabled=false;f.context.tlsTrustReady=false;
  REQUIRE(MqttDownlinkAdmission::Decision::Allow,f);
  f.context.phoneProxyMode=false;

  f.envelope.packet.from=Local;REQUIRE(MqttDownlinkAdmission::Decision::LocalSource,f);
  f.envelope.packet.from=Peer;
  f.envelope.packet.to=0xffffffffu;
  REQUIRE(MqttDownlinkAdmission::Decision::InvalidPacket,f); // Broadcast is malformed for PKI DM.
  f.envelope.packet.to=0xabcdef01u;
  REQUIRE(MqttDownlinkAdmission::Decision::PrivateTransitUnavailable,f);
  f.context.privateTransitReady=true;
  REQUIRE(MqttDownlinkAdmission::Decision::UnknownPrivateTransitPeer,f);
  f.context.privateTransitSourceKnown=true;
  REQUIRE(MqttDownlinkAdmission::Decision::UnknownPrivateTransitPeer,f);
  f.context.privateTransitDestinationKnown=true;
  REQUIRE(MqttDownlinkAdmission::Decision::Allow,f);
  // Claimed PKI/public-key/via_mqtt annotations remain irrelevant to transit.
  f.envelope.packet.pki_encrypted=false;f.envelope.packet.via_mqtt=true;
  f.envelope.packet.public_key.size=32;memset(f.envelope.packet.public_key.bytes,0xa5,32);
  REQUIRE(MqttDownlinkAdmission::Decision::Allow,f);
  f.envelope.packet.pki_encrypted=true;f.envelope.packet.via_mqtt=false;
  f.envelope.packet.public_key={};
  f.context.privateTransitSourceKnown=false;
  REQUIRE(MqttDownlinkAdmission::Decision::UnknownPrivateTransitPeer,f);
  f.context.privateTransitSourceKnown=true;f.context.privateTransitDestinationKnown=false;
  REQUIRE(MqttDownlinkAdmission::Decision::UnknownPrivateTransitPeer,f);
  f.context.privateTransitDestinationKnown=true;f.context.privateTransitReady=false;
  f.envelope.packet.to=Local;
  REQUIRE(MqttDownlinkAdmission::Decision::Allow,f); // Local AEAD path unchanged.
  f.envelope.packet.to=0xabcdef01u;f.context.privateTransitReady=true;
  f.envelope.packet.to=Local;
  f.context.privateRuntimeReady=false;
  REQUIRE(MqttDownlinkAdmission::Decision::PrivateRuntimeUnavailable,f);
  f.context.privateRuntimeReady=true;f.context.receivePathReady=false;
  REQUIRE(MqttDownlinkAdmission::Decision::PrivateRuntimeUnavailable,f);
  f.context.receivePathReady=true;

  f.envelope.packet.id=0;REQUIRE(MqttDownlinkAdmission::Decision::InvalidPacket,f);
  f.envelope.packet.id=1;f.envelope.packet.hop_start=1;f.envelope.packet.hop_limit=2;
  REQUIRE(MqttDownlinkAdmission::Decision::InvalidPacket,f);
  f.envelope.packet.hop_start=8;f.envelope.packet.hop_limit=1;
  REQUIRE(MqttDownlinkAdmission::Decision::InvalidPacket,f);
  f.envelope.packet.hop_start=4;f.envelope.packet.hop_limit=2;
  f.envelope.packet.encrypted.size=12;
  REQUIRE(MqttDownlinkAdmission::Decision::InvalidPacket,f);
  f.envelope.packet.encrypted.size=240;
  REQUIRE(MqttDownlinkAdmission::Decision::InvalidPacket,f);
  f.envelope.packet.encrypted.size=13;
  f.envelope.packet.channel=1;
  REQUIRE(MqttDownlinkAdmission::Decision::InvalidPacket,f);
  f.envelope.packet.channel=0;
  f.envelope.packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  f.envelope.kind=MqttServiceEnvelopeDecoder::PacketKind::kDecoded;
  REQUIRE(MqttDownlinkAdmission::Decision::InvalidPacket,f);
  f.envelope.packet.which_payload_variant=meshtastic_MeshPacket_encrypted_tag;
  f.envelope.kind=MqttServiceEnvelopeDecoder::PacketKind::kEncrypted;

  f.encode("PKI","!12345678");
  REQUIRE(MqttDownlinkAdmission::Decision::GatewayEcho,f);
  f.encode("other","!abcdef01");
  REQUIRE(MqttDownlinkAdmission::Decision::ChannelMismatch,f);
  f.encode("PKI","!ABCDEF01");
  REQUIRE(MqttDownlinkAdmission::Decision::InvalidPacket,f);
  puts("PASS stock PKI downlink admission: any-channel consent, local pinned recipient, claims ignored, transport guards and malformed envelope rejection");
  return 0;
}
#undef REQUIRE
