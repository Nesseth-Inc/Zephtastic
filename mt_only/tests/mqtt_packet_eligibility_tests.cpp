#include "network/MqttPacketEligibility.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

using mt::network::MqttPacketEligibility;

struct Fixture {
  MqttPacketEligibility::Context context;
  meshtastic_MeshPacket encrypted{};
  meshtastic_MeshPacket decoded{};
  uint8_t data[239]{};
  size_t dataSize=0;

  Fixture() {
    context.mqttEnabled=true;context.transportReady=true;
    context.anyChannelUplinkEnabled=true;context.thisChannelUplinkEnabled=true;
    context.channelActive=true;context.channelRole=1;context.channelIndex=2;context.channelHash=0x37;context.localNode=0x12345678;
    encrypted.which_payload_variant=meshtastic_MeshPacket_encrypted_tag;
    encrypted.encrypted.size=17;memset(encrypted.encrypted.bytes,0xA5,17);
    encrypted.from=0x23456789;encrypted.to=0xffffffffu;encrypted.id=0x76543210;encrypted.channel=0x37;
    decoded.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    decoded.from=encrypted.from;decoded.to=encrypted.to;decoded.id=encrypted.id;decoded.channel=2;
    const uint8_t payload[]={0x10,0x20,0x30};
    decoded.decoded.portnum=static_cast<meshtastic_PortNum>(8);
    decoded.decoded.payload.size=sizeof(payload);memcpy(decoded.decoded.payload.bytes,payload,sizeof(payload));
    decoded.decoded.has_bitfield=true;decoded.decoded.bitfield=1;
    assert(mt::PublicApplication::encode(static_cast<meshtastic_PortNum>(8),payload,sizeof(payload),0,0,
                                         data,sizeof(data),dataSize,1));
  }
  MqttPacketEligibility::Decision evaluate() const {
    return MqttPacketEligibility::evaluate(context,&encrypted,&decoded,data,dataSize);
  }
  void bitfield(uint32_t value) {
    const uint8_t payload[]={0x10,0x20,0x30};
    assert(mt::PublicApplication::encode(static_cast<meshtastic_PortNum>(8),payload,sizeof(payload),
      0,0,data,sizeof(data),dataSize,value));
    decoded.decoded.bitfield=value;
  }
};

int main() {
  Fixture f;
  assert(f.evaluate()==MqttPacketEligibility::Decision::CanonicalDecodedPublic);
  f.context.encryptionEnabled=true;
  assert(f.evaluate()==MqttPacketEligibility::Decision::OriginalEncryptedEnvelope);
  f.context.encryptionEnabled=false;

  // Explicit configuration and transport gates all default deny.
  Fixture f0;f0.context.mqttEnabled=false;assert(f0.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f1;f1.context.transportReady=false;assert(f1.evaluate()==MqttPacketEligibility::Decision::Deny);
  // The legacy json_enabled bit is retained for config round-trip only; MQTT
  // remains binary protobuf and admission follows the ordinary gates.
  Fixture f2;f2.context.jsonEnabled=true;assert(f2.evaluate()==MqttPacketEligibility::Decision::CanonicalDecodedPublic);
  Fixture f3;f3.context.proxyEnabled=true;assert(f3.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f4;f4.context.mapReportingEnabled=true;assert(f4.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f5;f5.context.anyChannelUplinkEnabled=false;assert(f5.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f6;f6.context.thisChannelUplinkEnabled=false;assert(f6.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f7;f7.context.channelActive=false;assert(f7.evaluate()==MqttPacketEligibility::Decision::Deny);

  // Consent, loop, role/selector, and correlated packet metadata are mandatory.
  Fixture f8;f8.decoded.decoded.bitfield=0;assert(f8.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f9;f9.decoded.via_mqtt=true;assert(f9.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f10;f10.context.channelRole=3;assert(f10.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f11;f11.context.channelIndex=8;assert(f11.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f12;f12.context.sourceIsLocal=true;assert(f12.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f13;f13.encrypted.id++;assert(f13.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f14;f14.decoded.channel++;assert(f14.evaluate()==MqttPacketEligibility::Decision::Deny);
  // Stock MeshPacket.channel is the raw key hash on the encrypted packet;
  // decoded.channel is the local channel slot. They intentionally differ here.
  Fixture f14a;f14a.encrypted.channel=2;assert(f14a.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f14b;f14b.context.channelHash=2;assert(f14b.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f15;f15.decoded.decoded.bitfield=2;assert(f15.evaluate()==MqttPacketEligibility::Decision::Deny);

  // A local packet needs the explicit global upload consent and does not use
  // the remote sender's Data bit as consent. Remote packets still need bit 0.
  Fixture localDenied;localDenied.context.sourceIsLocal=true;
  localDenied.decoded.from=localDenied.context.localNode;
  localDenied.encrypted.from=localDenied.decoded.from;
  localDenied.bitfield(0);
  assert(localDenied.evaluate()==MqttPacketEligibility::Decision::Deny);
  localDenied.context.localUploadConsent=true;
  assert(localDenied.evaluate()==MqttPacketEligibility::Decision::CanonicalDecodedPublic);
  localDenied.context.encryptionEnabled=true;
  assert(localDenied.evaluate()==MqttPacketEligibility::Decision::OriginalEncryptedEnvelope);
  Fixture localBitOnly;localBitOnly.context.sourceIsLocal=true;
  localBitOnly.decoded.from=localBitOnly.context.localNode;
  localBitOnly.encrypted.from=localBitOnly.decoded.from;
  assert(localBitOnly.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture remoteConsent;remoteConsent.context.localUploadConsent=true;
  remoteConsent.bitfield(0);
  assert(remoteConsent.evaluate()==MqttPacketEligibility::Decision::Deny);

  // Canonical bytes must describe this exact decoded packet; malformed and
  // oversized selectors/payloads, unsupported PKI, and plaintext-only envelopes deny.
  Fixture f16;f16.data[f16.dataSize++]=0x58;f16.data[f16.dataSize++]=0;assert(f16.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f17;f17.decoded.decoded.payload.bytes[0]^=1;assert(f17.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f18;f18.decoded.pki_encrypted=true;assert(f18.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f19;f19.encrypted.which_payload_variant=meshtastic_MeshPacket_decoded_tag;assert(f19.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f20;f20.encrypted.encrypted.size=sizeof(f20.encrypted.encrypted.bytes)+1;assert(f20.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f21;f21.decoded.decoded.payload.size=sizeof(f21.decoded.decoded.payload.bytes)+1;assert(f21.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f22;f22.decoded.decoded.xeddsa_signature.size=1;assert(f22.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f23;f23.encrypted.encrypted.size=0;assert(f23.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f24;f24.encrypted.public_key.size=sizeof(f24.encrypted.public_key.bytes)+1;assert(f24.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f25;f25.decoded.to=0xfffffffeu;assert(f25.evaluate()==MqttPacketEligibility::Decision::Deny);
  Fixture f26;f26.decoded.from=0xffffffffu;assert(f26.evaluate()==MqttPacketEligibility::Decision::Deny);
  puts("PASS MqttPacketEligibility explicit gates, consent, canonical correlation, ciphertext reference-only, malformed and loop rejection");
}
