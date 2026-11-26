// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../stock/mqtt/MQTT.h"
#include "MtKeyCommand.h"
#include "RelayCore.h"
#include "PublicApplication.h"
#include "MqttDataPolicy.h"
#include "MqttDefaultServerPolicy.h"
#include "meshtastic/mesh.pb.h"
#include <pb_decode.h>
#include <stddef.h>
#include <stdint.h>

namespace mt { namespace network {

static_assert(static_cast<uint32_t>(meshtastic_PortNum_DETECTION_SENSOR_APP) ==
              MqttDefaultServerPolicy::kDetectionSensorPort,
              "stock DetectionSensor port assignment changed");
static_assert(static_cast<uint32_t>(meshtastic_PortNum_RANGE_TEST_APP) ==
              MqttDefaultServerPolicy::kRangeTestPort,
              "stock RangeTest port assignment changed");

// Stock basis: meshtastic/firmware 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c,
// src/mqtt/MQTT.cpp, MQTT::onSend() (pinned lines 755-821): suppresses via_mqtt
// loops, requires some channel uplink and the current channel uplink unless PKI,
// checks Data.bitfield bit 0 for public decoded packets, and chooses the original
// encrypted packet when encryption_enabled. onReceiveProto() (lines 76-199)
// applies downlink consent/ignore_mqtt/self-loop/hop/admin/plaintext checks.
// This component implements only the ordinary channel uplink decision.
// Downlink and PKI remain unsupported; transport is separate. MqttDataPolicy
// supplies this repository's canonical ordinary-channel Data gate.
class MqttPacketEligibility {
public:
  enum class Decision : uint8_t { Deny, OriginalEncryptedEnvelope, CanonicalDecodedPublic };

  struct Context {
    bool mqttEnabled = false;
    bool transportReady = false;
    bool encryptionEnabled = false;
    bool jsonEnabled = false;
    bool proxyEnabled = false;
    bool mapReportingEnabled = false;
    bool defaultServer = false;
    bool anyChannelUplinkEnabled = false;
    bool thisChannelUplinkEnabled = false;
    bool channelActive = false;
    bool sourceIsLocal = false;
    bool localUploadConsent = false;
    uint8_t channelRole = 0;
    uint8_t channelIndex = 0;
    uint8_t channelHash = 0;
    uint32_t localNode = 0;
  };

private:
  static bool matchesDecoded(const meshtastic_MeshPacket &packet,
                             const uint8_t *wire, size_t size) {
    meshtastic_Data data{};
    pb_istream_t input = pb_istream_from_buffer(wire, size);
    if (!pb_decode(&input, meshtastic_Data_fields, &data) || input.bytes_left ||
        data.portnum != packet.decoded.portnum ||
        data.payload.size != packet.decoded.payload.size ||
        data.xeddsa_signature.size != packet.decoded.xeddsa_signature.size ||
        memcmp(data.xeddsa_signature.bytes,packet.decoded.xeddsa_signature.bytes,data.xeddsa_signature.size) ||
        data.reply_id != packet.decoded.reply_id || data.emoji != packet.decoded.emoji ||
        data.has_bitfield != packet.decoded.has_bitfield ||
        data.bitfield != packet.decoded.bitfield ||
        data.payload.size > sizeof(data.payload.bytes)) return false;
    for (size_t i=0;i<data.payload.size;++i)
      if (data.payload.bytes[i] != packet.decoded.payload.bytes[i]) return false;
    return true;
  }

public:

  // All inputs are explicit snapshots from the caller. Packet pointers and
  // canonicalData are borrowed only for the duration of this pure decision.
  static Decision evaluate(const Context &c,
                           const meshtastic_MeshPacket *encrypted,
                           const meshtastic_MeshPacket *decoded,
                           const uint8_t *canonicalData, size_t canonicalDataSize,bool signatureVerified=false) {
    // jsonEnabled remains in Context for compatibility with callers, but the
    // stock client is binary protobuf regardless of the legacy stored flag.
    if (!c.mqttEnabled || !c.transportReady || c.proxyEnabled ||
        c.mapReportingEnabled || !c.channelActive || c.channelRole > 2 ||
        c.channelIndex >= 8 || !c.localNode || !encrypted || !decoded ||
        !encrypted->id || encrypted->id != decoded->id || !decoded->from ||
        decoded->from == 0xffffffffu || decoded->to <= 1 || decoded->to == 0xfffffffeu ||
        encrypted->public_key.size > sizeof(encrypted->public_key.bytes) ||
        decoded->public_key.size > sizeof(decoded->public_key.bytes) ||
        encrypted->from != decoded->from || encrypted->to != decoded->to ||
        encrypted->channel != c.channelHash || decoded->channel != c.channelIndex ||
        decoded->via_mqtt ||
        (c.sourceIsLocal != (decoded->from == c.localNode)) ||
        !canonicalData || !MqttDataPolicy::canonical(canonicalData, canonicalDataSize,signatureVerified) ||
        !matchesDecoded(*decoded, canonicalData, canonicalDataSize))
      return Decision::Deny;

    // PKI and other private/control paths require stock routing trust context.
    if (encrypted->pki_encrypted || decoded->pki_encrypted ||
        encrypted->which_payload_variant != meshtastic_MeshPacket_encrypted_tag ||
        !encrypted->encrypted.size ||
        encrypted->encrypted.size > sizeof(encrypted->encrypted.bytes) ||
        decoded->which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        decoded->decoded.payload.size > sizeof(decoded->decoded.payload.bytes) ||
        (decoded->decoded.xeddsa_signature.size&&!signatureVerified) ||
        !decoded->decoded.has_bitfield || decoded->decoded.bitfield > 1)
      return Decision::Deny;

    const auto *packet = stock::MQTT::onSend(*encrypted, *decoded, c);
    if (!packet) return Decision::Deny;
    return packet == encrypted ? Decision::OriginalEncryptedEnvelope : Decision::CanonicalDecodedPublic;
  }
};
}} // namespace mt::network
