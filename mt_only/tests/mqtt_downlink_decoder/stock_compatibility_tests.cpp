// SPDX-License-Identifier: GPL-3.0-only
// Differential fixtures for the pinned stock ServiceEnvelope nanopb decoder.
#include "MqttServiceEnvelopeDecoder.h"
#include "MqttServiceEnvelopeCodec.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

using mt::mqtt::MqttServiceEnvelopeDecoder;
using mt::mqtt::ServiceEnvelopeCodec;

namespace {
size_t appendVarint(uint64_t value, uint8_t *out) {
  size_t n = 0;
  do {
    uint8_t byte = static_cast<uint8_t>(value & 0x7fu);
    value >>= 7;
    if (value) byte |= 0x80u;
    out[n++] = byte;
  } while (value);
  return n;
}

void pushVarint(std::vector<uint8_t> &out, uint64_t value) {
  uint8_t bytes[10];
  const size_t n = appendVarint(value, bytes);
  out.insert(out.end(), bytes, bytes + n);
}

void appendField(std::vector<uint8_t> &out, uint32_t tag,
                 const uint8_t *value, size_t length) {
  pushVarint(out, (uint64_t(tag) << 3) | 2u);
  pushVarint(out, length);
  out.insert(out.end(), value, value + length);
}

void appendBytesField(std::vector<uint8_t> &out, uint32_t tag,
                      const char *value, size_t length) {
  appendField(out, tag, reinterpret_cast<const uint8_t *>(value), length);
}

struct WireField {
  uint32_t tag;
  uint8_t wireType;
  std::vector<uint8_t> raw;
};

bool readVarint(const std::vector<uint8_t> &input, size_t &cursor, uint64_t &value) {
  value = 0;
  for (unsigned i = 0; i < 10 && cursor < input.size(); ++i) {
    const uint8_t byte = input[cursor++];
    if (i == 9 && (byte & 0xfeu)) return false;
    value |= uint64_t(byte & 0x7fu) << (7u * i);
    if ((byte & 0x80u) == 0) return true;
  }
  return false;
}

std::vector<WireField> splitFields(const std::vector<uint8_t> &input) {
  std::vector<WireField> result;
  size_t cursor = 0;
  while (cursor < input.size()) {
    const size_t start = cursor;
    uint64_t key = 0;
    assert(readVarint(input, cursor, key) && key <= UINT32_MAX);
    const uint32_t tag = static_cast<uint32_t>(key >> 3);
    const uint8_t wireType = static_cast<uint8_t>(key & 7u);
    assert(tag != 0);
    if (wireType == 0) {
      uint64_t ignored = 0;
      assert(readVarint(input, cursor, ignored));
    } else if (wireType == 1) {
      assert(input.size() - cursor >= 8);
      cursor += 8;
    } else if (wireType == 2) {
      uint64_t length = 0;
      assert(readVarint(input, cursor, length) && length <= input.size() - cursor);
      cursor += static_cast<size_t>(length);
    } else if (wireType == 5) {
      assert(input.size() - cursor >= 4);
      cursor += 4;
    } else {
      assert(false && "fixture contains unsupported protobuf wire type");
    }
    result.push_back({tag, wireType,
                      std::vector<uint8_t>(input.begin() + start, input.begin() + cursor)});
  }
  return result;
}

meshtastic_MeshPacket decodedPacket() {
  meshtastic_MeshPacket packet{};
  packet.from = 0x12345678;
  packet.to = 0xffffffffu;
  packet.id = 0x87654321;
  packet.channel = 2;
  packet.hop_limit = 3;
  packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
  packet.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
  const uint8_t body[] = {'s', 't', 'o', 'c', 'k'};
  packet.decoded.payload.size = sizeof(body);
  memcpy(packet.decoded.payload.bytes, body, sizeof(body));
  return packet;
}

std::vector<uint8_t> packetWire(const meshtastic_MeshPacket &packet) {
  uint8_t bytes[meshtastic_MeshPacket_size]{};
  pb_ostream_t stream = pb_ostream_from_buffer(bytes, sizeof(bytes));
  assert(pb_encode(&stream, meshtastic_MeshPacket_fields, &packet));
  return {bytes, bytes + stream.bytes_written};
}

std::vector<uint8_t> envelopeWire(const meshtastic_MeshPacket &packet,
                                  const char *channel, const char *gateway) {
  uint8_t bytes[ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes]{};
  size_t length = 0;
  assert(ServiceEnvelopeCodec::encode(&packet, channel, strlen(channel), gateway,
                                     strlen(gateway), bytes, sizeof(bytes), &length));
  return {bytes, bytes + length};
}

struct StockDecoded {
  meshtastic_ServiceEnvelope envelope = meshtastic_ServiceEnvelope_init_zero;
  bool valid = false;
  ~StockDecoded() {
    if (valid) pb_release(meshtastic_ServiceEnvelope_fields, &envelope);
  }
};

void stockDecode(const std::vector<uint8_t> &wire, StockDecoded &result) {
  pb_istream_t stream = pb_istream_from_buffer(wire.data(), wire.size());
  result.valid = pb_decode(&stream, meshtastic_ServiceEnvelope_fields, &result.envelope) &&
                 stream.bytes_left == 0;
}

void assertBothDecoders(const std::vector<uint8_t> &wire,
                        const char *channel, const char *gateway,
                        uint32_t from, uint32_t id) {
  StockDecoded stock;
  stockDecode(wire, stock);
  assert(stock.valid && stock.envelope.packet && stock.envelope.channel_id &&
         stock.envelope.gateway_id);
  assert(strcmp(stock.envelope.channel_id, channel) == 0);
  assert(strcmp(stock.envelope.gateway_id, gateway) == 0);
  assert(stock.envelope.packet->from == from && stock.envelope.packet->id == id);

  MqttServiceEnvelopeDecoder::Decoded decoded{};
  assert(MqttServiceEnvelopeDecoder::decode(wire.data(), wire.size(), &decoded));
  assert(strcmp(decoded.channelId, channel) == 0 && strcmp(decoded.gatewayId, gateway) == 0);
  assert(decoded.packet.from == from && decoded.packet.id == id);
}

void testStockGeneratedRoundTrip() {
  const auto wire = envelopeWire(decodedPacket(), "stock-channel", "!12345678");
  assertBothDecoders(wire, "stock-channel", "!12345678", 0x12345678, 0x87654321);
  puts("PASS actual stock generated ServiceEnvelope descriptor round-trip");
}

void testStockOrderingDuplicatesAndUnknowns() {
  const auto packet = packetWire(decodedPacket());
  auto fields = splitFields(packet);
  std::reverse(fields.begin(), fields.end());

  std::vector<uint8_t> reorderedPacket;
  for (const auto &field : fields)
    reorderedPacket.insert(reorderedPacket.end(), field.raw.begin(), field.raw.end());

  // The last duplicate scalar value wins, just as it does in nanopb.
  const uint8_t replacementFrom[] = {0x0d, 0xef, 0xbe, 0xad, 0xde};
  reorderedPacket.insert(reorderedPacket.end(), replacementFrom, replacementFrom + sizeof(replacementFrom));
  // Unknown nested fields of wire types 0, 1, 2, and 5 are skipped.
  const uint8_t unknownNested[] = {0xa0, 0x06, 0x01, 0xa9, 0x06, 1, 2, 3, 4, 5, 6, 7, 8,
                                   0xb2, 0x06, 0x02, 0xaa, 0xbb,
                                   0xbd, 0x06, 1, 2, 3, 4};
  reorderedPacket.insert(reorderedPacket.end(), unknownNested, unknownNested + sizeof(unknownNested));

  std::vector<uint8_t> wire;
  // Envelope field order is not canonicalized; duplicate string uses last value.
  appendBytesField(wire, 2, "old", 3);
  pushVarint(wire, (90u << 3) | 0u);
  pushVarint(wire, 7);
  appendBytesField(wire, 3, "old-gw", 6);
  appendBytesField(wire, 2, "stock-channel", 13);
  appendField(wire, 1, packet.data(), packet.size());
  appendField(wire, 1, reorderedPacket.data(), reorderedPacket.size());
  appendBytesField(wire, 3, "!12345678", 9);
  // Unknown outer fixed-width fields are skipped as well.
  pushVarint(wire, (91u << 3) | 1u);
  wire.insert(wire.end(), {1, 2, 3, 4, 5, 6, 7, 8});
  pushVarint(wire, (92u << 3) | 5u);
  wire.insert(wire.end(), {9, 10, 11, 12});

  assertBothDecoders(wire, "stock-channel", "!12345678", 0xdeadbeef, 0x87654321);
  puts("PASS stock nanopb order, duplicate-last, and unknown-field behavior");
}

void testStockOneofLastValue() {
  auto packet = packetWire(decodedPacket());
  const uint8_t encryptedBranch[] = {0x2a, 0x01, 0x55};
  packet.insert(packet.end(), encryptedBranch, encryptedBranch + sizeof(encryptedBranch));
  std::vector<uint8_t> wire;
  appendField(wire, 1, packet.data(), packet.size());
  appendBytesField(wire, 2, "channel", 7);
  appendBytesField(wire, 3, "!12345678", 9);

  StockDecoded stock;
  stockDecode(wire, stock);
  assert(stock.valid && stock.envelope.packet);
  assert(stock.envelope.packet->which_payload_variant == meshtastic_MeshPacket_encrypted_tag);
  assert(stock.envelope.packet->encrypted.size == 1 &&
         stock.envelope.packet->encrypted.bytes[0] == 0x55);

  MqttServiceEnvelopeDecoder::Decoded decoded{};
  assert(MqttServiceEnvelopeDecoder::decode(wire.data(), wire.size(), &decoded));
  assert(decoded.kind == MqttServiceEnvelopeDecoder::PacketKind::kEncrypted);
  assert(decoded.packet.encrypted.size == 1 && decoded.packet.encrypted.bytes[0] == 0x55);
  puts("PASS stock oneof last-value behavior");
}
}  // namespace

int main() {
#ifndef PB_ENABLE_MALLOC
  assert(false && "stock ServiceEnvelope fixture must use its pointer-field malloc configuration");
#endif
  testStockGeneratedRoundTrip();
  testStockOrderingDuplicatesAndUnknowns();
  testStockOneofLastValue();
  puts("MQTT_STOCK_COMPATIBILITY_CASES 3");
}
