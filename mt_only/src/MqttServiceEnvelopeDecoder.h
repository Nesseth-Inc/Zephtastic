// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "MqttServiceEnvelopeCodec.h"
#include "meshtastic/mqtt.pb.h"
#include "pb_decode.h"

namespace mt::mqtt {

// Decoder-only component. It does not authenticate, authorize, route, decrypt,
// or retain any input pointers. The returned MeshPacket is fully static data.
class MqttServiceEnvelopeDecoder {
 public:
  static constexpr size_t kMaxStringBytes = 128;
  static constexpr size_t kMaxEnvelopeBytes = ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes;
  static constexpr size_t kMaxPacketBytes = meshtastic_MeshPacket_size;

  enum class PacketKind : uint8_t { kDecoded = 1, kEncrypted = 2 };

  struct Decoded {
    meshtastic_MeshPacket packet{};
    char channelId[kMaxStringBytes + 1]{};
    char gatewayId[kMaxStringBytes + 1]{};
    uint16_t channelIdLength = 0;
    uint16_t gatewayIdLength = 0;
    PacketKind kind = PacketKind::kDecoded;
    // Wire metadata is a remote claim. This decoder never validates PKI or
    // signatures and therefore never returns a trusted authentication result.
    bool authenticationTrusted = false;
    bool signatureTrusted = false;
  };
  static_assert(sizeof(Decoded) <= 768, "caller-owned decoded result remains bounded");

  // On failure, *out remains byte-for-byte unchanged. Input is read only for
  // this call. No pointer into input escapes. `out` must point to a live object.
  static bool decode(const uint8_t *input, size_t inputLength, Decoded *out) {
    if (input == nullptr || out == nullptr || inputLength == 0 ||
        inputLength > kMaxEnvelopeBytes) return false;

    Decoded next{};
    const uint8_t *packetBytes = nullptr;
    size_t packetLength = 0;
    const uint8_t *channelBytes = nullptr;
    size_t channelLength = 0;
    const uint8_t *gatewayBytes = nullptr;
    size_t gatewayLength = 0;
    size_t cursor = 0;
    bool havePacket = false;
    while (cursor < inputLength) {
      uint32_t key = 0;
      if (!readVarint32(input, inputLength, cursor, key)) return false;
      const uint32_t field = key >> 3;
      const uint8_t wireType = static_cast<uint8_t>(key & 7u);
      if (field == 0) return false;

      if (field < meshtastic_ServiceEnvelope_packet_tag ||
          field > meshtastic_ServiceEnvelope_gateway_id_tag) {
        if (!skipField(input, inputLength, cursor, wireType)) return false;
        continue;
      }
      if (wireType != 2) return false;

      uint32_t length32 = 0;
      if (!readVarint32(input, inputLength, cursor, length32) ||
          static_cast<size_t>(length32) > inputLength - cursor) return false;
      const size_t length = static_cast<size_t>(length32);
      const uint8_t *value = input + cursor;
      cursor += length;

      if (field == meshtastic_ServiceEnvelope_packet_tag) {
        if (length == 0) return false;
        packetBytes = value;
        packetLength = length;
        havePacket = true;
      } else if (field == meshtastic_ServiceEnvelope_channel_id_tag) {
        channelBytes = value;
        channelLength = length;
      } else if (field == meshtastic_ServiceEnvelope_gateway_id_tag) {
        gatewayBytes = value;
        gatewayLength = length;
      } else {
        return false;
      }
    }
    if (!havePacket || cursor != inputLength) return false;
    if ((channelBytes != nullptr &&
         (channelLength > kMaxStringBytes || !validUtf8(channelBytes, channelLength))) ||
        (gatewayBytes != nullptr &&
         (gatewayLength > kMaxStringBytes || !validUtf8(gatewayBytes, gatewayLength)))) return false;
    if (channelBytes) {
      memcpyBytes(next.channelId, channelBytes, channelLength);
      next.channelIdLength = static_cast<uint16_t>(channelLength);
    }
    if (gatewayBytes) {
      memcpyBytes(next.gatewayId, gatewayBytes, gatewayLength);
      next.gatewayIdLength = static_cast<uint16_t>(gatewayLength);
    }

    pb_istream_t packetStream = pb_istream_from_buffer(packetBytes, packetLength);
    if (!pb_decode(&packetStream, meshtastic_MeshPacket_fields, &next.packet) ||
        packetStream.bytes_left != 0 || !validPacket(next.packet)) return false;

    if (next.packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
      // A decoded body cannot truthfully claim to remain PKI-encrypted.
      if (next.packet.pki_encrypted) return false;
      next.kind = PacketKind::kDecoded;
    } else if (next.packet.which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
      next.kind = PacketKind::kEncrypted;
    } else {
      return false;
    }
    *out = next;
    return true;
  }

 private:
  static void memcpyBytes(char *destination, const uint8_t *source, size_t length) {
    for (size_t i = 0; i < length; ++i) destination[i] = static_cast<char>(source[i]);
    destination[length] = '\0';
  }

  // Match nanopb 0.4.9.1's uint32 varint reader: nonminimal encodings and
  // valid sign-extension bytes are accepted, while overflow/truncation fails.
  static bool readVarint32(const uint8_t *input, size_t size, size_t &cursor,
                           uint32_t &value) {
    if (cursor >= size) return false;
    uint8_t byte = input[cursor++];
    uint32_t result = byte & 0x7fu;
    if ((byte & 0x80u) == 0) {
      value = byte;
      return true;
    }

    unsigned bitpos = 7;
    do {
      if (cursor >= size) return false;
      byte = input[cursor++];
      if (bitpos >= 32) {
        const uint8_t extension = bitpos < 63 ? 0xffu : 0x01u;
        const bool validExtension = (byte & 0x7fu) == 0 ||
                                    ((result >> 31) != 0 && byte == extension);
        if (bitpos >= 64 || !validExtension) return false;
      } else if (bitpos == 28) {
        if ((byte & 0x70u) != 0 && (byte & 0x78u) != 0x78u) return false;
        result |= static_cast<uint32_t>(byte & 0x0fu) << bitpos;
      } else {
        result |= static_cast<uint32_t>(byte & 0x7fu) << bitpos;
      }
      bitpos += 7;
    } while (byte & 0x80u);

    value = result;
    return true;
  }

  static bool skipField(const uint8_t *input, size_t size, size_t &cursor,
                        uint8_t wireType) {
    switch (wireType) {
      case 0: // PB_WT_VARINT: nanopb skips through the terminating byte.
        while (cursor < size) {
          if ((input[cursor++] & 0x80u) == 0) return true;
        }
        return false;
      case 1: // PB_WT_64BIT
        if (size - cursor < 8) return false;
        cursor += 8;
        return true;
      case 2: { // PB_WT_STRING
        uint32_t length = 0;
        if (!readVarint32(input, size, cursor, length) ||
            static_cast<size_t>(length) > size - cursor) return false;
        cursor += static_cast<size_t>(length);
        return true;
      }
      case 5: // PB_WT_32BIT
        if (size - cursor < 4) return false;
        cursor += 4;
        return true;
      default: // Nanopb 0.4.9.1 rejects groups and reserved wire types.
        return false;
    }
  }

  static bool validUtf8(const uint8_t *value, size_t length) {
    size_t i = 0;
    while (i < length) {
      const uint8_t a = value[i++];
      if (a == 0) return false;
      if (a < 0x80) continue;
      uint32_t code = 0;
      unsigned continuation = 0;
      if (a >= 0xc2 && a <= 0xdf) { code = a & 0x1fu; continuation = 1; }
      else if (a >= 0xe0 && a <= 0xef) { code = a & 0x0fu; continuation = 2; }
      else if (a >= 0xf0 && a <= 0xf4) { code = a & 0x07u; continuation = 3; }
      else return false;
      if (continuation > length - i) return false;
      for (unsigned j = 0; j < continuation; ++j) {
        const uint8_t b = value[i++];
        if ((b & 0xc0u) != 0x80u) return false;
        code = (code << 6) | (b & 0x3fu);
      }
      if ((continuation == 1 && code < 0x80) || (continuation == 2 && code < 0x800) ||
          (continuation == 3 && code < 0x10000) || code > 0x10ffff ||
          (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
  }

  static bool validPacket(const meshtastic_MeshPacket &packet) {
    if (packet.public_key.size > sizeof(packet.public_key.bytes)) return false;
    if (packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
      return packet.decoded.payload.size <= sizeof(packet.decoded.payload.bytes) &&
             packet.decoded.xeddsa_signature.size <= sizeof(packet.decoded.xeddsa_signature.bytes);
    }
    if (packet.which_payload_variant == meshtastic_MeshPacket_encrypted_tag)
      return packet.encrypted.size <= sizeof(packet.encrypted.bytes);
    return false;
  }

  static_assert(meshtastic_MeshPacket_size + 3 +
                    2 * (3 + kMaxStringBytes) <= kMaxEnvelopeBytes,
                "pinned envelope limits must fit the bounded decode input");
};

}  // namespace mt::mqtt
