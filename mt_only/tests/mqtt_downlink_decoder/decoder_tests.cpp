// SPDX-License-Identifier: GPL-3.0-only
#include "MqttServiceEnvelopeDecoder.h"
#include "MqttServiceEnvelopeCodec.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

using mt::mqtt::MqttServiceEnvelopeDecoder;
using mt::mqtt::ServiceEnvelopeCodec;
using Decoded = MqttServiceEnvelopeDecoder::Decoded;

namespace {
size_t varint(uint64_t value, uint8_t *out) {
  size_t n = 0;
  do {
    uint8_t b = static_cast<uint8_t>(value & 0x7f);
    value >>= 7;
    if (value) b |= 0x80;
    out[n++] = b;
  } while (value);
  return n;
}

void appendVarint(std::vector<uint8_t> &out, uint64_t value) {
  uint8_t bytes[10];
  const size_t n = varint(value, bytes);
  out.insert(out.end(), bytes, bytes + n);
}

void appendField(std::vector<uint8_t> &out, uint8_t tag, const uint8_t *value, size_t length) {
  appendVarint(out, (uint64_t(tag) << 3) | 2u);
  appendVarint(out, length);
  out.insert(out.end(), value, value + length);
}

std::vector<uint8_t> outer(const std::vector<uint8_t> &packet, const char *channel,
                           size_t channelLength, const char *gateway, size_t gatewayLength) {
  std::vector<uint8_t> result;
  appendField(result, 1, packet.data(), packet.size());
  if (channel != nullptr)
    appendField(result, 2, reinterpret_cast<const uint8_t *>(channel), channelLength);
  if (gateway != nullptr)
    appendField(result, 3, reinterpret_cast<const uint8_t *>(gateway), gatewayLength);
  return result;
}

std::vector<uint8_t> packetWire(const meshtastic_MeshPacket &packet) {
  uint8_t bytes[meshtastic_MeshPacket_size]{};
  pb_ostream_t stream = pb_ostream_from_buffer(bytes, sizeof(bytes));
  assert(pb_encode(&stream, meshtastic_MeshPacket_fields, &packet));
  return std::vector<uint8_t>(bytes, bytes + stream.bytes_written);
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
  const uint8_t body[] = {'h','e','l','l','o'};
  packet.decoded.payload.size = sizeof(body);
  memcpy(packet.decoded.payload.bytes, body, sizeof(body));
  return packet;
}

std::vector<uint8_t> actualEncoder(const meshtastic_MeshPacket &packet,
                                   const char *channel, const char *gateway) {
  uint8_t out[ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes]{};
  size_t written = 0;
  assert(ServiceEnvelopeCodec::encode(&packet, channel, strlen(channel), gateway,
                                      strlen(gateway), out, sizeof(out), &written));
  return std::vector<uint8_t>(out, out + written);
}

bool isMarker(const Decoded &value) {
  return value.channelIdLength == 1 && value.channelId[0] == 'q' &&
         value.gatewayIdLength == 1 && value.gatewayId[0] == 'g' &&
         value.packet.from == 0xdecafbad && value.authenticationTrusted == false &&
         value.signatureTrusted == false;
}

unsigned tests = 0;
void pass(const char *label) { ++tests; printf("PASS %s\n", label); }

void rejectUnchanged(const std::vector<uint8_t> &wire) {
  Decoded out{};
  out.packet.from = 0xdecafbad;
  out.channelId[0] = 'q'; out.channelId[1] = '\0'; out.channelIdLength = 1;
  out.gatewayId[0] = 'g'; out.gatewayId[1] = '\0'; out.gatewayIdLength = 1;
  uint8_t before[sizeof(out)];
  memcpy(before, &out, sizeof(out));
  assert(!MqttServiceEnvelopeDecoder::decode(wire.data(), wire.size(), &out));
  assert(memcmp(before, &out, sizeof(out)) == 0);
  assert(isMarker(out));
}

void testRoundTrip() {
  auto packet = decodedPacket();
  const char channel[] = "public-fixture-α";
  const char gateway[] = "!12345678";
  const auto wire = actualEncoder(packet, channel, gateway);
  const auto independentlyWrapped = outer(packetWire(packet), channel, strlen(channel), gateway, strlen(gateway));
  assert(wire == independentlyWrapped);

  Decoded decoded{};
  assert(MqttServiceEnvelopeDecoder::decode(wire.data(), wire.size(), &decoded));
  assert(decoded.kind == MqttServiceEnvelopeDecoder::PacketKind::kDecoded);
  assert(decoded.channelIdLength == strlen(channel) && !strcmp(decoded.channelId, channel));
  assert(decoded.gatewayIdLength == strlen(gateway) && !strcmp(decoded.gatewayId, gateway));
  assert(decoded.packet.from == packet.from && decoded.packet.id == packet.id);
  assert(decoded.packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag);
  assert(!decoded.authenticationTrusted && !decoded.signatureTrusted);
  // Re-encoding the returned value proves no input pointers are retained.
  const auto reencoded = actualEncoder(decoded.packet, decoded.channelId, decoded.gatewayId);
  assert(reencoded == wire);
  pass("published M114 encoder round-trip, UTF-8 identifiers, static packet data, no trusted claims");
}

void testEncryptedAndPkiClaims() {
  meshtastic_MeshPacket packet{};
  packet.from = 0x23456789;
  packet.to = 0xffffffffu;
  packet.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
  packet.encrypted.size = 4;
  const uint8_t cipher[] = {0x11, 0x22, 0x33, 0x44};
  memcpy(packet.encrypted.bytes, cipher, sizeof(cipher));
  packet.pki_encrypted = true;
  packet.xeddsa_signed = true;
  const auto wire = actualEncoder(packet, "channel", "!23456789");
  Decoded decoded{};
  assert(MqttServiceEnvelopeDecoder::decode(wire.data(), wire.size(), &decoded));
  assert(decoded.kind == MqttServiceEnvelopeDecoder::PacketKind::kEncrypted);
  assert(decoded.packet.encrypted.size == sizeof(cipher));
  assert(!memcmp(decoded.packet.encrypted.bytes, cipher, sizeof(cipher)));
  assert(decoded.packet.pki_encrypted && decoded.packet.xeddsa_signed);
  assert(!decoded.authenticationTrusted && !decoded.signatureTrusted);

  packet = decodedPacket();
  packet.pki_encrypted = true;  // False for a decoded plaintext payload.
  rejectUnchanged(actualEncoder(packet, "channel", "!23456789"));
  pass("decoded/encrypted distinction and PKI/signature bits remain untrusted; false plaintext PKI claim rejected");
}

void testOuterMalformedShapes() {
  const auto packet = packetWire(decodedPacket());
  auto good = outer(packet, "channel", 7, "!12345678", 9);
  Decoded out{};
  assert(MqttServiceEnvelopeDecoder::decode(good.data(), good.size(), &out));
  auto packetOnly = outer(packet, nullptr, 0, nullptr, 0);
  assert(MqttServiceEnvelopeDecoder::decode(packetOnly.data(), packetOnly.size(), &out));
  assert(out.channelIdLength == 0 && out.gatewayIdLength == 0);

  auto unknown = good; unknown.push_back(0x22); unknown.push_back(0);
  assert(MqttServiceEnvelopeDecoder::decode(unknown.data(), unknown.size(), &out));
  auto trailing = good; trailing.push_back(0);
  rejectUnchanged(trailing);
  auto missingPacket = outer({}, "channel", 7, nullptr, 0);
  rejectUnchanged(missingPacket);

  std::vector<uint8_t> duplicate;
  appendField(duplicate, 1, packet.data(), packet.size());
  appendField(duplicate, 2, reinterpret_cast<const uint8_t *>("a"), 1);
  appendField(duplicate, 2, reinterpret_cast<const uint8_t *>("b"), 1);
  assert(MqttServiceEnvelopeDecoder::decode(duplicate.data(), duplicate.size(), &out));
  assert(out.channelIdLength == 1 && out.channelId[0] == 'b');

  std::vector<uint8_t> outOfOrder;
  appendField(outOfOrder, 1, packet.data(), packet.size());
  appendField(outOfOrder, 3, reinterpret_cast<const uint8_t *>("gw"), 2);
  appendField(outOfOrder, 2, reinterpret_cast<const uint8_t *>("ch"), 2);
  assert(MqttServiceEnvelopeDecoder::decode(outOfOrder.data(), outOfOrder.size(), &out));
  assert(out.channelIdLength == 2 && !strcmp(out.channelId, "ch"));
  assert(out.gatewayIdLength == 2 && !strcmp(out.gatewayId, "gw"));

  std::vector<uint8_t> noncanonicalLength;
  noncanonicalLength.push_back(0x0a);
  noncanonicalLength.push_back(static_cast<uint8_t>(packet.size() | 0x80u));
  noncanonicalLength.push_back(0);
  noncanonicalLength.insert(noncanonicalLength.end(), packet.begin(), packet.end());
  assert(MqttServiceEnvelopeDecoder::decode(noncanonicalLength.data(), noncanonicalLength.size(), &out));

  std::vector<uint8_t> wrongWire{0x08, 0x01};
  rejectUnchanged(wrongWire);
  std::vector<uint8_t> truncated{0x0a, 0x80};
  rejectUnchanged(truncated);
  std::vector<uint8_t> overflowingKey(9, 0x80);
  overflowingKey.push_back(0x02);
  rejectUnchanged(overflowingKey);
  std::vector<uint8_t> groupField;
  appendVarint(groupField, (uint64_t{99} << 3) | 3u);
  rejectUnchanged(groupField);
  std::vector<uint8_t> shortUnknownFixed64;
  appendVarint(shortUnknownFixed64, (uint64_t{99} << 3) | 1u);
  shortUnknownFixed64.insert(shortUnknownFixed64.end(), {1, 2, 3});
  rejectUnchanged(shortUnknownFixed64);
  std::vector<uint8_t> oversized(MqttServiceEnvelopeDecoder::kMaxEnvelopeBytes + 1, 0);
  rejectUnchanged(oversized);
  pass("unknown/duplicate/out-of-order/nonminimal protobuf fields follow nanopb semantics; malformed, trailing, missing, and oversized outer shapes reject");
}

void testStringBoundsAndUtf8() {
  const auto packet = packetWire(decodedPacket());
  std::vector<uint8_t> atLimit(128, 'x');
  std::vector<uint8_t> wire;
  appendField(wire, 1, packet.data(), packet.size());
  appendField(wire, 2, atLimit.data(), atLimit.size());
  Decoded accepted{};
  assert(MqttServiceEnvelopeDecoder::decode(wire.data(), wire.size(), &accepted));
  assert(accepted.channelIdLength == atLimit.size() && accepted.channelId[128] == '\0');

  std::vector<uint8_t> tooLong(129, 'x');
  wire.clear();
  appendField(wire, 1, packet.data(), packet.size());
  appendField(wire, 2, tooLong.data(), tooLong.size());
  rejectUnchanged(wire);

  wire.clear(); appendField(wire, 1, packet.data(), packet.size());
  appendField(wire, 2, tooLong.data(), tooLong.size());
  appendField(wire, 2, reinterpret_cast<const uint8_t *>("last"), 4);
  Decoded superseded{};
  assert(MqttServiceEnvelopeDecoder::decode(wire.data(), wire.size(), &superseded));
  assert(superseded.channelIdLength == 4 && !strcmp(superseded.channelId, "last"));

  const uint8_t embeddedNul[] = {'a', 0, 'b'};
  wire.clear(); appendField(wire, 1, packet.data(), packet.size());
  appendField(wire, 2, embeddedNul, sizeof(embeddedNul)); rejectUnchanged(wire);

  const uint8_t invalidUtf8[] = {0xc0, 0xaf};
  wire.clear(); appendField(wire, 1, packet.data(), packet.size());
  appendField(wire, 2, invalidUtf8, sizeof(invalidUtf8)); rejectUnchanged(wire);
  pass("metadata strings enforce 128-byte bound, no embedded NUL, and valid UTF-8");
}

void testNestedCanonicalAndBounds() {
  auto packet = packetWire(decodedPacket());
  auto truncatedPacket = packet;
  truncatedPacket.pop_back();
  rejectUnchanged(outer(truncatedPacket, "channel", 7, "gateway", 7));

  auto duplicateField = packet;
  duplicateField.insert(duplicateField.end(), packet.begin(), packet.begin() + 5); // duplicate fixed32 `from`
  auto duplicateOuter = outer(duplicateField, "channel", 7, "gateway", 7);
  Decoded duplicateDecoded{};
  assert(MqttServiceEnvelopeDecoder::decode(duplicateOuter.data(), duplicateOuter.size(), &duplicateDecoded));
  assert(duplicateDecoded.packet.from == decodedPacket().from);

  auto unknownField = packet;
  unknownField.insert(unknownField.end(), {0xa0, 0x06, 0x01}); // field 100, varint
  auto unknownOuter = outer(unknownField, "channel", 7, "gateway", 7);
  assert(MqttServiceEnvelopeDecoder::decode(unknownOuter.data(), unknownOuter.size(), &duplicateDecoded));

  auto conflictingOneof = packet;
  conflictingOneof.insert(conflictingOneof.end(), {0x2a, 0x01, 0x55}); // append encrypted branch
  auto conflictingOuter = outer(conflictingOneof, "channel", 7, "gateway", 7);
  assert(MqttServiceEnvelopeDecoder::decode(conflictingOuter.data(), conflictingOuter.size(), &duplicateDecoded));
  assert(duplicateDecoded.kind == MqttServiceEnvelopeDecoder::PacketKind::kEncrypted);

  // MeshPacket decoded field 4 wraps Data; Data field 2 is the bounded payload.
  std::vector<uint8_t> data{0x08, static_cast<uint8_t>(meshtastic_PortNum_TEXT_MESSAGE_APP), 0x12};
  appendVarint(data, 234);
  data.insert(data.end(), 234, 0x5a);
  std::vector<uint8_t> oversizedPayload{0x0d, 0x78, 0x56, 0x34, 0x12, 0x22};
  appendVarint(oversizedPayload, data.size());
  oversizedPayload.insert(oversizedPayload.end(), data.begin(), data.end());
  rejectUnchanged(outer(oversizedPayload, "channel", 7, "gateway", 7));

  std::vector<uint8_t> oversizedCipher{0x2a};
  appendVarint(oversizedCipher, 257);
  oversizedCipher.insert(oversizedCipher.end(), 257, 0x42);
  rejectUnchanged(outer(oversizedCipher, "channel", 7, "gateway", 7));
  pass("nested unknown/duplicate/oneof fields follow nanopb last-value semantics; truncation and static byte-array overflow reject");
}
}  // namespace

int main() {
  testRoundTrip();
  testEncryptedAndPkiClaims();
  testOuterMalformedShapes();
  testStringBoundsAndUtf8();
  testNestedCanonicalAndBounds();
  printf("MQTT_DOWNLINK_DECODER_CASES %u\n", tests);
  assert(tests == 5);
}
