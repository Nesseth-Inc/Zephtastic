// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../MqttServiceEnvelopeDecoder.h"
#include "MqttTopicLevel.h"
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::mqtt {

// Pure preflight for the ZephCore receive path. It never allocates, routes,
// authenticates, decrypts, or transmits; Allow permits Service::rx to run the
// ordinary channel gates or the bound private runtime's authenticated receive.
class MqttDownlinkAdmission {
 public:
  enum class Decision : uint8_t {
    Allow, Disabled, Unconfigured, TransportUnavailable, Ignored,
    NoDownlinkConsent, UnsupportedChannel, ChannelMismatch, KeyMismatch,
    GatewayEcho, LocalSource, ReceiveUnavailable, InvalidPacket,
    UnsupportedPacket, PrivateRuntimeUnavailable, UnpinnedPrivatePeer,
    PrivateDestinationMismatch, PrivateTransitUnavailable,
    UnknownPrivateTransitPeer
  };
  struct Context {
    bool mqttEnabled=false,brokerConfigured=false,transportReady=false;
    bool tlsEnabled=false,tlsTrustReady=false,ignoreMqtt=false,ignoreSender=false;
    // Phone proxy mode is an explicit local trust delegation. It must be set
    // only by Companion after its persisted proxy opt-in and local session are
    // both checked; it never means that this device verified broker TLS.
    bool phoneProxyMode=false,proxyModeEnabled=false;
    // Stock uses any enabled channel's downlink grant to authorize the PKI
    // subscription and PKI ingress. For ordinary topics this remains the
    // exact matched channel's grant.
    bool channelActive=false,channelDownlinkEnabled=false,channelKeyBound=false;
    bool receivePathReady=false,encryptionEnabled=false;
    // Service sets these from the already-bound private runtime for this
    // packet's source. Neither protobuf pki_encrypted nor public_key is auth.
    bool privateRuntimeReady=false,pinnedPrivatePeer=false;
    // Nonlocal PKI remains opaque. Service derives these from current relay/TX
    // state and already-cached, non-placeholder, non-ignored NodeCache users.
    bool privateTransitReady=false,privateTransitSourceKnown=false;
    bool privateTransitDestinationKnown=false;
    uint8_t channelIndex=0,channelHash=0;
    uint32_t localNode=0;
    const char *localChannelId=nullptr,*localGatewayId=nullptr;
    size_t localGatewayIdLength=0;
  };
  static constexpr uint32_t kMaxHop=7,kBroadcast=0xffffffffu,kInvalidNode=0xfffffffeu;
  static constexpr size_t kMaxPrivateCiphertextBytes=239;

  static bool topicMatches(const char *root,const char *topic,size_t topicLength,
                           const MqttServiceEnvelopeDecoder::Decoded &e) {
    if(!topic||!topicLength||topicLength>=DownlinkTopicCapacity||!e.channelIdLength||!e.gatewayIdLength||
       !MqttTopicLevel::valid(e.channelId,e.channelIdLength))
      return false;
    char expected[DownlinkTopicCapacity]{};
    const int n=snprintf(expected,sizeof(expected),"%s/2/e/%s/%s",root&&*root?root:"msh",
                         e.channelId,e.gatewayId);
    return n>0&&size_t(n)==topicLength&&size_t(n)<sizeof(expected)&&!memcmp(expected,topic,topicLength);
  }

  static Decision evaluate(const Context &c,const MqttServiceEnvelopeDecoder::Decoded &e) {
    if(!c.mqttEnabled)return Decision::Disabled;
    if(!c.brokerConfigured)return Decision::Unconfigured;
    if(c.phoneProxyMode) {
      if(!c.proxyModeEnabled||!c.transportReady)return Decision::TransportUnavailable;
    } else if(!c.transportReady||(c.tlsEnabled&&!c.tlsTrustReady))
      return Decision::TransportUnavailable;
    if(c.ignoreMqtt)return Decision::Ignored;
    if(!c.channelDownlinkEnabled)return Decision::NoDownlinkConsent;
    if(same(e.channelId,e.channelIdLength,"PKI"))return privatePacket(c,e);
    if(!c.channelActive||c.channelIndex>=8||!MqttTopicLevel::validCString(c.localChannelId)||
       !MqttTopicLevel::valid(e.channelId,e.channelIdLength)||
       !same(e.channelId,e.channelIdLength,c.localChannelId))return Decision::ChannelMismatch;
    if(!c.channelKeyBound)return Decision::KeyMismatch;
    if(!validGateway(e.gatewayId,e.gatewayIdLength)||
       !validGateway(c.localGatewayId,c.localGatewayIdLength))return Decision::InvalidPacket;
    if(same(e.gatewayId,e.gatewayIdLength,c.localGatewayId,c.localGatewayIdLength))return Decision::GatewayEcho;
    if(!c.localNode||c.localNode==kBroadcast||c.localNode==kInvalidNode||e.packet.from==c.localNode)
      return Decision::LocalSource;
    if(c.ignoreSender)return Decision::Ignored;
    if(!c.receivePathReady)return Decision::ReceiveUnavailable;
    return packet(c,e);
  }

 private:
  static constexpr size_t DownlinkTopicCapacity=160;
  static bool same(const char *a,size_t an,const char *b,size_t bn) {
    return a&&b&&an==bn&&an<=MqttServiceEnvelopeDecoder::kMaxStringBytes&&!memcmp(a,b,an);
  }
  static bool same(const char *a,size_t an,const char *literal) {
    if(!literal)return false;size_t n=0;
    while(n<=MqttServiceEnvelopeDecoder::kMaxStringBytes&&literal[n])++n;
    return n<=MqttServiceEnvelopeDecoder::kMaxStringBytes&&same(a,an,literal,n);
  }
  static bool validGateway(const char *p,size_t n) {
    if(!p||n!=9||p[0]!='!')return false;
    for(size_t i=1;i<n;++i)if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')))return false;
    return p[n]=='\0';
  }
  static Decision privatePacket(const Context &c,const MqttServiceEnvelopeDecoder::Decoded &e) {
    if(!validGateway(e.gatewayId,e.gatewayIdLength)||
       !validGateway(c.localGatewayId,c.localGatewayIdLength))return Decision::InvalidPacket;
    if(same(e.gatewayId,e.gatewayIdLength,c.localGatewayId,c.localGatewayIdLength))
      return Decision::GatewayEcho;
    const auto &p=e.packet;
    if(!p.id||p.from<=1||p.from==kBroadcast||p.from==kInvalidNode||
       p.to<=1||p.to==kBroadcast||p.to==kInvalidNode||
       p.hop_limit>kMaxHop||p.hop_start>kMaxHop||p.hop_limit>p.hop_start||
       p.next_hop>255||p.relay_node>255||
       p.which_payload_variant!=meshtastic_MeshPacket_encrypted_tag||p.channel!=0||
       p.encrypted.size<13||p.encrypted.size>kMaxPrivateCiphertextBytes)
      return Decision::InvalidPacket;
    if(!c.localNode||c.localNode<=1||c.localNode==kBroadcast||c.localNode==kInvalidNode)
      return Decision::InvalidPacket;
    if(p.from==c.localNode)return Decision::LocalSource;
    if(c.ignoreSender)return Decision::Ignored;
    if(p.to==c.localNode) {
      if(!c.receivePathReady||!c.privateRuntimeReady)return Decision::PrivateRuntimeUnavailable;
      if(!c.pinnedPrivatePeer)return Decision::UnpinnedPrivatePeer;
      // PKI frames are encrypted independently of MQTT's ordinary-channel
      // encryption setting. The pki_encrypted/public_key/via_mqtt fields are
      // remote claims; only PkiRuntime may establish authenticated identity.
      return Decision::Allow;
    }
    if(!c.privateTransitReady)return Decision::PrivateTransitUnavailable;
    if(!c.privateTransitSourceKnown||!c.privateTransitDestinationKnown)
      return Decision::UnknownPrivateTransitPeer;
    // No local decryption or authentication is performed on transit.
    return Decision::Allow;
  }
  static Decision packet(const Context &c,const MqttServiceEnvelopeDecoder::Decoded &e) {
    const auto &p=e.packet;
    if(!p.id||p.from<=1||p.from==kBroadcast||p.from==kInvalidNode||p.to<=1||p.to==kInvalidNode||
       p.hop_limit>kMaxHop||p.hop_start>kMaxHop||p.hop_limit>p.hop_start||
       p.next_hop>255||p.relay_node>255||p.public_key.size>sizeof(p.public_key.bytes))
      return Decision::InvalidPacket;
    if(e.kind==MqttServiceEnvelopeDecoder::PacketKind::kDecoded)
      return Decision::UnsupportedPacket;
    if(e.kind==MqttServiceEnvelopeDecoder::PacketKind::kEncrypted) {
      if(!c.encryptionEnabled||p.which_payload_variant!=meshtastic_MeshPacket_encrypted_tag||!p.encrypted.size||
         p.encrypted.size>sizeof(p.encrypted.bytes)||p.pki_encrypted||p.via_mqtt)
        return Decision::UnsupportedPacket;
      if(p.channel!=c.channelHash)return Decision::KeyMismatch;
    } else return Decision::UnsupportedPacket;
    return Decision::Allow;
  }
};
}  // namespace mt::mqtt
