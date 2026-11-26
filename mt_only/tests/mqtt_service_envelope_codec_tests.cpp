// SPDX-License-Identifier: GPL-3.0-only
#include "MqttServiceEnvelopeCodec.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pb_decode.h"

namespace {
using mt::mqtt::ServiceEnvelopeCodec;

static unsigned groups = 0;

void group(const char *name) {
  ++groups;
  printf("PASS %s\n", name);
}

meshtastic_MeshPacket decodedPacket() {
  meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
  packet.from = 0x12345678;
  packet.to = 0xffffffffu;
  packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
  packet.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
  static const uint8_t payload[] = {'o', 'k'};
  packet.decoded.payload.size = sizeof(payload);
  memcpy(packet.decoded.payload.bytes, payload, sizeof(payload));
  return packet;
}

meshtastic_MeshPacket encryptedPacket() {
  meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
  packet.from = 0x87654321;
  packet.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
  packet.encrypted.size = 3;
  packet.encrypted.bytes[0] = 0x10;
  packet.encrypted.bytes[1] = 0x20;
  packet.encrypted.bytes[2] = 0x30;
  packet.public_key.size = 2;
  packet.public_key.bytes[0] = 0x40;
  packet.public_key.bytes[1] = 0x50;
  return packet;
}

bool unchanged(const uint8_t *bytes, size_t size, uint8_t value) {
  for (size_t i = 0; i < size; ++i) {
    if (bytes[i] != value) return false;
  }
  return true;
}

bool encodeFailure(const meshtastic_MeshPacket *packet, const char *channel,
                   size_t channelLength, const char *gateway,
                   size_t gatewayLength, size_t capacity) {
  uint8_t out[ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes];
  memset(out, 0xa5, sizeof(out));
  size_t outLength = 0x1234;
  const bool ok = ServiceEnvelopeCodec::encode(
      packet, channel, channelLength, gateway, gatewayLength, out, capacity,
      &outLength);
  return !ok && outLength == 0x1234 && unchanged(out, sizeof(out), 0xa5);
}

void roundTrip(const meshtastic_MeshPacket &packet, const char *name) {
  uint8_t wire[ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes];
  memset(wire, 0xcc, sizeof(wire));
  size_t written = 0;
  assert(ServiceEnvelopeCodec::encode(&packet, "mesh", 4, "!12345678", 9,
                                      wire, sizeof(wire), &written));
  assert(written > 0 && written <= ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes);

  meshtastic_ServiceEnvelope decoded = meshtastic_ServiceEnvelope_init_zero;
  pb_istream_t input = pb_istream_from_buffer(wire, written);
  assert(pb_decode(&input, meshtastic_ServiceEnvelope_fields, &decoded));
  assert(input.bytes_left == 0);
  assert(decoded.packet != NULL && decoded.channel_id != NULL &&
         decoded.gateway_id != NULL);
  assert(strcmp(decoded.channel_id, "mesh") == 0);
  assert(strcmp(decoded.gateway_id, "!12345678") == 0);
  assert(decoded.packet->from == packet.from);
  assert(decoded.packet->which_payload_variant == packet.which_payload_variant);
  if (packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
    assert(decoded.packet->decoded.portnum == packet.decoded.portnum);
    assert(decoded.packet->decoded.payload.size == packet.decoded.payload.size);
    assert(memcmp(decoded.packet->decoded.payload.bytes,
                  packet.decoded.payload.bytes, packet.decoded.payload.size) == 0);
  } else {
    assert(decoded.packet->encrypted.size == packet.encrypted.size);
    assert(memcmp(decoded.packet->encrypted.bytes, packet.encrypted.bytes,
                  packet.encrypted.size) == 0);
    assert(decoded.packet->public_key.size == packet.public_key.size);
    assert(memcmp(decoded.packet->public_key.bytes, packet.public_key.bytes,
                  packet.public_key.size) == 0);
  }
  pb_release(meshtastic_ServiceEnvelope_fields, &decoded);
  group(name);
}

void testRoundTripsAndBoundaries() {
  roundTrip(decodedPacket(), "stock decoded MeshPacket envelope round-trip");
  roundTrip(encryptedPacket(), "stock encrypted MeshPacket envelope round-trip");

  char maxString[ServiceEnvelopeCodec::kMaxStringBytes + 1];
  memset(maxString, 'x', sizeof(maxString) - 1);
  maxString[sizeof(maxString) - 1] = '\0';
  meshtastic_MeshPacket packet = decodedPacket();
  packet.decoded.payload.size = sizeof(packet.decoded.payload.bytes);
  memset(packet.decoded.payload.bytes, 0x6b, packet.decoded.payload.size);
  packet.decoded.xeddsa_signature.size =
      sizeof(packet.decoded.xeddsa_signature.bytes);
  memset(packet.decoded.xeddsa_signature.bytes, 0x7c,
         packet.decoded.xeddsa_signature.size);
  uint8_t wire[ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes];
  size_t written = 0;
  assert(ServiceEnvelopeCodec::encode(&packet, maxString,
                                      ServiceEnvelopeCodec::kMaxStringBytes,
                                      maxString,
                                      ServiceEnvelopeCodec::kMaxStringBytes,
                                      wire, sizeof(wire), &written));
  assert(written <= ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes);
  group("maximum packet and adapter strings fit within total envelope cap");

  uint8_t exact[ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes];
  memset(exact, 0xa5, sizeof(exact));
  size_t exactLength = 0;
  assert(ServiceEnvelopeCodec::encode(&packet, "", 0, "", 0, exact,
                                      sizeof(exact), &exactLength));
  assert(exactLength > 0);
  uint8_t exactCapacity[ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes];
  memset(exactCapacity, 0xa5, sizeof(exactCapacity));
  size_t exactCapacityLength = 99;
  assert(ServiceEnvelopeCodec::encode(
      &packet, "", 0, "", 0, exactCapacity, exactLength,
      &exactCapacityLength));
  assert(exactCapacityLength == exactLength);
  assert(encodeFailure(&packet, "", 0, "", 0, exactLength - 1));
  group("empty strings and exact/undersized capacity boundaries are valid");
}

void testAtomicRefusals() {
  meshtastic_MeshPacket packet = decodedPacket();
  assert(encodeFailure(NULL, "c", 1, "g", 1, 1024));
  assert(encodeFailure(&packet, NULL, 0, "g", 1, 1024));
  assert(encodeFailure(&packet, "c", 1, NULL, 0, 1024));
  uint8_t nullOutBuffer[16];
  memset(nullOutBuffer, 0xa5, sizeof(nullOutBuffer));
  size_t untouchedLength = 0x1234;
  assert(!ServiceEnvelopeCodec::encode(&packet, "c", 1, "g", 1, NULL,
                                       sizeof(nullOutBuffer), &untouchedLength));
  assert(untouchedLength == 0x1234 &&
         unchanged(nullOutBuffer, sizeof(nullOutBuffer), 0xa5));
  assert(!ServiceEnvelopeCodec::encode(&packet, "c", 1, "g", 1,
                                       nullOutBuffer, sizeof(nullOutBuffer),
                                       NULL));
  assert(unchanged(nullOutBuffer, sizeof(nullOutBuffer), 0xa5));

  char overlong[ServiceEnvelopeCodec::kMaxStringBytes + 2];
  memset(overlong, 'x', sizeof(overlong) - 1);
  overlong[sizeof(overlong) - 1] = '\0';
  assert(encodeFailure(&packet, overlong, sizeof(overlong) - 1, "g", 1, 1024));

  const char embedded[] = {'a', '\0', 'b', '\0'};
  assert(encodeFailure(&packet, embedded, 3, "g", 1, 1024));
  const char badTerminator[] = {'a', 'b'};
  assert(encodeFailure(&packet, badTerminator, 1, "g", 1, 1024));
  assert(encodeFailure(&packet, "channel", 7, "gateway", 7, 1));

  packet.which_payload_variant = 0;
  assert(encodeFailure(&packet, "c", 1, "g", 1, 1024));
  packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
  packet.decoded.payload.size =
      static_cast<pb_size_t>(sizeof(packet.decoded.payload.bytes) + 1);
  assert(encodeFailure(&packet, "c", 1, "g", 1, 1024));
  packet = decodedPacket();
  packet.public_key.size =
      static_cast<pb_size_t>(sizeof(packet.public_key.bytes) + 1);
  assert(encodeFailure(&packet, "c", 1, "g", 1, 1024));
  packet = encryptedPacket();
  packet.encrypted.size =
      static_cast<pb_size_t>(sizeof(packet.encrypted.bytes) + 1);
  assert(encodeFailure(&packet, "c", 1, "g", 1, 1024));
  group("null, string, capacity, selector, and malformed-size refusals are atomic");
}

void testMalformedWireDecode() {
  const meshtastic_MeshPacket packet = decodedPacket();
  uint8_t wire[ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes];
  size_t written = 0;
  assert(ServiceEnvelopeCodec::encode(&packet, "mesh", 4, "gateway", 7,
                                      wire, sizeof(wire), &written));
  meshtastic_ServiceEnvelope decoded = meshtastic_ServiceEnvelope_init_zero;
  pb_istream_t truncated = pb_istream_from_buffer(wire, written - 1);
  assert(!pb_decode(&truncated, meshtastic_ServiceEnvelope_fields, &decoded));

  const uint8_t malformed[] = {0x0a, 0x80};
  meshtastic_ServiceEnvelope malformedOut = meshtastic_ServiceEnvelope_init_zero;
  pb_istream_t malformedInput =
      pb_istream_from_buffer(malformed, sizeof(malformed));
  assert(!pb_decode(&malformedInput, meshtastic_ServiceEnvelope_fields,
                    &malformedOut));
  group("truncated and malformed stock protobuf wire data is rejected");
}
}  // namespace

int main() {
  testRoundTripsAndBoundaries();
  testAtomicRefusals();
  testMalformedWireDecode();
  printf("PASS total groups=%u\n", groups);
  return 0;
}
