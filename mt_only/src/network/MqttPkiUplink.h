// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "MqttServiceEnvelopeCodec.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mt::network {

// Bounded bridge from explicitly typed PKI events to the pinned stock
// ServiceEnvelope/PKI topic. It does no crypto, transport, radio work, key
// lookup, or allocation. The local and transit entry points intentionally have
// separate origin checks; callers must not infer PKI from outer header bits.
class MqttPkiUplink {
 public:
  static constexpr size_t kTopicCapacity = 60;
  static constexpr size_t kMaxPrivateCiphertextBytes = 239;

  // `frame` is the already-encrypted local private TX frame. A trusted caller
  // invokes this only from MqttPrivateTxSink after committed settings and
  // policy admission. The header bits alone cannot distinguish this frame
  // family from channel data.
  static bool encode(uint32_t localNode, const uint8_t *frame,
                     size_t frameSize, const char *root,
                     const char *gatewayId, char *topic, size_t topicCapacity,
                     uint8_t *payload, size_t payloadCapacity,
                     size_t *payloadSize) {
    return encodeTyped(localNode, frame, frameSize, root, gatewayId, topic,
                       topicCapacity, payload, payloadCapacity, payloadSize,
                       false,0,false);
  }

  // `frame` is already-encrypted nonlocal PKI ciphertext from a frame that the
  // RF receive path observed between two other nodes. This
  // function validates and serializes the opaque frame only; it does not
  // establish that the remote source was authenticated by the gateway.
  static bool encodeTransit(uint32_t localNode, const uint8_t *frame,
                            size_t frameSize, const char *root,
                            const char *gatewayId, char *topic,
                            size_t topicCapacity, uint8_t *payload,
                            size_t payloadCapacity, size_t *payloadSize,uint32_t stamp=0,bool validTime=false) {
    return encodeTyped(localNode, frame, frameSize, root, gatewayId, topic,
                       topicCapacity, payload, payloadCapacity, payloadSize,
                       true,stamp,validTime);
  }

 private:
  static bool encodeTyped(uint32_t localNode, const uint8_t *frame,
                          size_t frameSize, const char *root,
                          const char *gatewayId, char *topic,
                          size_t topicCapacity, uint8_t *payload,
                          size_t payloadCapacity, size_t *payloadSize,
                          bool transit,uint32_t stamp,bool validTime) {
    if (localNode <= 1 || localNode >= 0xfffffffeu || !frame ||
        frameSize < 17 || frameSize > 255 ||
        !root || !gatewayId || !topic || !payload || !payloadSize) return false;

    const uint32_t to = le32(frame);
    const uint32_t from = le32(frame + 4);
    const uint32_t id = le32(frame + 8);
    const uint8_t flags = frame[12];
    const uint8_t hopLimit = flags & 7u;
    const uint8_t hopStart = (flags >> 5) & 7u;
    const size_t cipherSize = frameSize - 16;
    if (!id || from <= 1 || from >= 0xfffffffeu ||
        (transit ? from == localNode : from != localNode) ||
        to <= 1 || to >= 0xfffffffeu || to == localNode ||
        hopLimit > hopStart || (flags & 0x10u) ||
        frame[13] != 0 || cipherSize == 0 ||
        cipherSize > kMaxPrivateCiphertextBytes ||
        !validRoot(root) || !validGateway(gatewayId)) return false;

    char stagedTopic[kTopicCapacity]{};
    const int topicLength = snprintf(stagedTopic, sizeof(stagedTopic),
                                     "%s/2/e/PKI/%s",
                                     *root ? root : "msh", gatewayId);
    if (topicLength <= 0 || size_t(topicLength) >= sizeof(stagedTopic) ||
        size_t(topicLength) + 1 > topicCapacity) return false;

    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
    packet.from = from;
    packet.to = to;
    packet.id = id;
    packet.rx_time = stamp;
    packet.has_rx_time = validTime;
    packet.channel = 0;  // The private frame has no ordinary channel key.
    packet.hop_limit = hopLimit;
    packet.hop_start = hopStart;
    packet.want_ack = (flags & 8u) != 0;
    packet.next_hop = frame[14];
    packet.relay_node = frame[15];
    packet.pki_encrypted = true;
    packet.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
    packet.encrypted.size = pb_size_t(cipherSize);
    if (cipherSize > sizeof(packet.encrypted.bytes)) return false;
    memcpy(packet.encrypted.bytes, frame + 16, cipherSize);

    if (!mt::mqtt::ServiceEnvelopeCodec::encode(
            &packet, "PKI", 3, gatewayId, 9, payload,
            payloadCapacity, payloadSize)) return false;

    memcpy(topic, stagedTopic, size_t(topicLength) + 1);
    return true;
  }
  static uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  }
  static bool validRoot(const char *value) {
    size_t n = 0;
    while (n < 32 && value[n]) {
      const uint8_t c = static_cast<uint8_t>(value[n]);
      if (c < 0x20 || c == '+' || c == '#') return false;
      ++n;
    }
    return n < 32;
  }
  static bool validGateway(const char *value) {
    if (!value || value[0] != '!') return false;
    for (size_t i = 1; i < 9; ++i) {
      const char c = value[i];
      if (!c || !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return value[9] == '\0';
  }
};

}  // namespace mt::network
