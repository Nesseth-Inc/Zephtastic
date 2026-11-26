// SPDX-License-Identifier: GPL-3.0-only
#ifndef MT_MQTT_SERVICE_ENVELOPE_CODEC_H_
#define MT_MQTT_SERVICE_ENVELOPE_CODEC_H_

#include <stddef.h>
#include <stdint.h>

#include "meshtastic/mqtt.pb.h"
#include "pb_encode.h"

namespace mt::mqtt {

// A bounded, synchronous encoder for the pinned stock ServiceEnvelope schema.
// No transport, policy, credentials, logging, or dynamic allocation is used.
class ServiceEnvelopeCodec {
 public:
  static constexpr size_t kMaxStringBytes = 128;
  static constexpr size_t kMaxEncodedEnvelopeBytes = 1024;

  // Each string must expose exactly length non-NUL bytes followed by a NUL;
  // the caller guarantees that length+1 bytes are readable. Empty strings are
  // valid. Caller-owned packet/string/input and output ranges must remain valid
  // for the call; out, outLength, packet, and both strings must not overlap.
  // On failure, out and *outLength are unchanged. The input packet and strings
  // are read synchronously and are not retained.
  static bool encode(const meshtastic_MeshPacket *packet,
                     const char *channelId, size_t channelIdLength,
                     const char *gatewayId, size_t gatewayIdLength,
                     uint8_t *out, size_t capacity, size_t *outLength) {
    if (packet == NULL || out == NULL || outLength == NULL ||
        !validString(channelId, channelIdLength) ||
        !validString(gatewayId, gatewayIdLength) || !validPacket(*packet)) {
      return false;
    }

    meshtastic_ServiceEnvelope envelope = meshtastic_ServiceEnvelope_init_zero;
    envelope.packet = const_cast<meshtastic_MeshPacket *>(packet);
    envelope.channel_id = const_cast<char *>(channelId);
    envelope.gateway_id = const_cast<char *>(gatewayId);

    size_t measured = 0;
    if (!pb_get_encoded_size(&measured, meshtastic_ServiceEnvelope_fields,
                             &envelope) ||
        measured > kMaxEncodedEnvelopeBytes || measured > capacity) {
      return false;
    }

    // Stage the complete message so encoder errors cannot partially modify the
    // caller's buffer. The explicit envelope cap bounds this stack allocation.
    uint8_t scratch[kMaxEncodedEnvelopeBytes];
    pb_ostream_t stream = pb_ostream_from_buffer(scratch, sizeof(scratch));
    if (!pb_encode(&stream, meshtastic_ServiceEnvelope_fields, &envelope) ||
        stream.bytes_written != measured || stream.bytes_written > capacity) {
      return false;
    }

    for (size_t i = 0; i < stream.bytes_written; ++i) {
      out[i] = scratch[i];
    }
    *outLength = stream.bytes_written;
    return true;
  }

 private:
  static bool validString(const char *value, size_t length) {
    if (value == NULL || length > kMaxStringBytes) return false;
    for (size_t i = 0; i < length; ++i) {
      if (value[i] == '\0') return false;
    }
    return value[length] == '\0';
  }

  static bool validPacket(const meshtastic_MeshPacket &packet) {
    if (packet.public_key.size > sizeof(packet.public_key.bytes)) return false;
    if (packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
      return packet.decoded.payload.size <=
                 sizeof(packet.decoded.payload.bytes) &&
             packet.decoded.xeddsa_signature.size <=
                 sizeof(packet.decoded.xeddsa_signature.bytes);
    }
    if (packet.which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
      return packet.encrypted.size <= sizeof(packet.encrypted.bytes);
    }
    return false;
  }

  static_assert(meshtastic_MeshPacket_size + 3 +
                        2 * (3 + kMaxStringBytes) <=
                    kMaxEncodedEnvelopeBytes,
                "string and stock packet limits must fit the envelope cap");
};

}  // namespace mt::mqtt

#endif  // MT_MQTT_SERVICE_ENVELOPE_CODEC_H_
