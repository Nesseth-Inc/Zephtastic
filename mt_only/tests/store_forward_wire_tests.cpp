// SPDX-License-Identifier: GPL-3.0-only
#include "StoreForwardWire.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "StoreForwardControl.h"
#include "pb_decode.h"

using mt::store_forward::StoreForwardWire;

namespace {
unsigned groups = 0;
void pass(const char *name) { ++groups; printf("PASS %s\n", name); }

using RequestResponse = meshtastic_StoreAndForward_RequestResponse;

uint32_t rawRr(const meshtastic_StoreAndForward &message) {
  if (sizeof(message.rr) == sizeof(uint8_t)) {
    uint8_t raw = 0;
    memcpy(&raw, &message.rr, sizeof(raw));
    return raw;
  }
  uint32_t raw = 0;
  memcpy(&raw, &message.rr, sizeof(raw));
  return raw;
}

void setRawRr(meshtastic_StoreAndForward &message, uint32_t raw) {
  if (sizeof(message.rr) == sizeof(uint8_t)) {
    const uint8_t byte = static_cast<uint8_t>(raw);
    memcpy(&message.rr, &byte, sizeof(byte));
  } else {
    memcpy(&message.rr, &raw, sizeof(raw));
  }
}

void expectWire(const meshtastic_StoreAndForward &message,
                const uint8_t *expected, size_t expectedSize) {
  uint8_t out[StoreForwardWire::MaxEncodedBytes];
  memset(out, 0xa5, sizeof(out));
  size_t written = 999;
  StoreForwardWire::Error error = StoreForwardWire::Error::InvalidArgument;
  assert(StoreForwardWire::encode(message, out, sizeof(out), &written, &error));
  assert(error == StoreForwardWire::Error::None && written == expectedSize);
  assert(memcmp(out, expected, expectedSize) == 0);
}

void decodeGoldenVectors() {
  // rr=CLIENT_HISTORY; history={messages:2, raw window:7, last_request:9}.
  static const uint8_t history[] = {0x08,0x41,0x1a,0x06,0x08,0x02,0x10,0x07,0x18,0x09};
  meshtastic_StoreAndForward decoded = meshtastic_StoreAndForward_init_zero;
  StoreForwardWire::Error error;
  assert(StoreForwardWire::decode(history, sizeof(history), &decoded, &error));
  assert(error == StoreForwardWire::Error::None);
  assert(decoded.rr == meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY);
  assert(decoded.which_variant == meshtastic_StoreAndForward_history_tag);
  assert(decoded.variant.history.history_messages == 2);
  assert(decoded.variant.history.window == 7);
  assert(decoded.variant.history.last_request == 9);
  pass("golden history request preserves raw window and cursor");

  // rr=ROUTER_TEXT_BROADCAST, text="abc", original_id=150.
  static const uint8_t text[] = {0x08,0x09,0x2a,0x03,'a','b','c',0x30,0x96,0x01};
  assert(StoreForwardWire::decode(text, sizeof(text), &decoded, &error));
  assert(decoded.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_BROADCAST);
  assert(decoded.which_variant == meshtastic_StoreAndForward_text_tag);
  assert(decoded.variant.text.size == 3);
  assert(memcmp(decoded.variant.text.bytes, "abc", 3) == 0);
  assert(decoded.original_id == 150);
  pass("golden text and original id");
}

void enumRangeBoundaryIsTransactional() {
  static const uint8_t unknownInRange[] = {0x08, 0x63};  // rr=99
  static const uint8_t upperInRange[] = {0x08, 0x7f};  // rr=127
  static const uint8_t firstOutOfRange[] = {0x08, 0x80, 0x01};  // rr=128
  static const uint8_t unknownOutOfRange[] = {0x08, 0xc8, 0x01};  // rr=200
  static const uint8_t uint32Max[] = {0x08, 0xff, 0xff, 0xff, 0xff, 0x0f};
  static const uint8_t overlong[] = {
      0x08, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x00};

  StoreForwardWire::Error error = StoreForwardWire::Error::InvalidArgument;
  meshtastic_StoreAndForward decoded = meshtastic_StoreAndForward_init_zero;
  assert(StoreForwardWire::decode(unknownInRange, sizeof(unknownInRange),
                                  &decoded, &error));
  assert(error == StoreForwardWire::Error::None && rawRr(decoded) == 99);
  assert(StoreForwardWire::decode(upperInRange, sizeof(upperInRange),
                                  &decoded, &error));
  assert(error == StoreForwardWire::Error::None && rawRr(decoded) == 127);

  // 99 is an unknown protocol value but lies within the generated enum's
  // current C++ representable range, so stock default dispatch remains safe.
  mt::modules::StoreForwardArchive::Record records[1]{};
  mt::modules::StoreForwardArchive::Cursor cursors[1]{};
  mt::modules::StoreForwardArchive archive(records, 1, cursors, 1);
  mt::modules::StoreForwardControl::Config config{};
  config.enabled = true;
  config.isServer = true;
  mt::modules::StoreForwardControl control(archive, config);
  mt::modules::StoreForwardControl::Context context{};
  context.peer = 12;
  assert(control.receive(decoded, context).kind ==
         mt::modules::StoreForwardControl::ActionKind::None);
  assert(control.requests() == 1);

  decoded = meshtastic_StoreAndForward_init_zero;
  decoded.rr = meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS;
  decoded.which_variant = meshtastic_StoreAndForward_text_tag;
  decoded.variant.text.size = 1;
  decoded.variant.text.bytes[0] = 's';
  auto expectDecodeRejectedUnchanged = [&](const uint8_t *wire, size_t size) {
    uint8_t before[sizeof(decoded)];
    memcpy(before, &decoded, sizeof(decoded));
    assert(!StoreForwardWire::decode(wire, size, &decoded, &error));
    assert(error == StoreForwardWire::Error::Malformed);
    assert(memcmp(&decoded, before, sizeof(decoded)) == 0);
  };
  expectDecodeRejectedUnchanged(unknownOutOfRange, sizeof(unknownOutOfRange));
  expectDecodeRejectedUnchanged(firstOutOfRange, sizeof(firstOutOfRange));
  expectDecodeRejectedUnchanged(uint32Max, sizeof(uint32Max));
  expectDecodeRejectedUnchanged(overlong, sizeof(overlong));

  uint8_t encoded[StoreForwardWire::MaxEncodedBytes];
  memset(encoded, 0xa5, sizeof(encoded));
  size_t written = 99;
  auto expectEncodeRejectedUnchanged = [&](uint32_t raw) {
    meshtastic_StoreAndForward invalid = meshtastic_StoreAndForward_init_zero;
    setRawRr(invalid, raw);
    written = 99;
    assert(!StoreForwardWire::encode(invalid, encoded, sizeof(encoded),
                                     &written, &error));
    assert(error == StoreForwardWire::Error::InvalidMessage && written == 0);
    for (uint8_t byte : encoded) assert(byte == 0xa5);
  };
  expectEncodeRejectedUnchanged(200);
  expectEncodeRejectedUnchanged(128);
  expectEncodeRejectedUnchanged(UINT32_MAX);
  pass("rr enum boundary preserves in-range unknown and rejects unsafe values transactionally");
}

void emptyMessageMatchesPinnedNanopbAndNoopDispatch() {
  // The pinned generated message has no required fields. An empty protobuf is
  // therefore the default-zero message under nanopb, including rr=UNSET.
  uint8_t emptyStorage = 0;
  meshtastic_StoreAndForward baseline = meshtastic_StoreAndForward_init_zero;
  pb_istream_t baselineStream = pb_istream_from_buffer(&emptyStorage, 0);
  assert(pb_decode(&baselineStream, meshtastic_StoreAndForward_fields, &baseline));
  assert(baselineStream.bytes_left == 0);
  assert(baseline.rr == meshtastic_StoreAndForward_RequestResponse_UNSET);
  assert(baseline.which_variant == 0 && baseline.original_id == 0);

  // Probe null/zero directly as well; the codec is allowed to use its stable
  // empty byte when this pinned buffer callback does not accept a null pointer.
  meshtastic_StoreAndForward nullBaseline = meshtastic_StoreAndForward_init_zero;
  pb_istream_t nullStream = pb_istream_from_buffer(nullptr, 0);
  const bool nullPointerAccepted =
      pb_decode(&nullStream, meshtastic_StoreAndForward_fields, &nullBaseline);
  if (nullPointerAccepted) {
    assert(nullBaseline.rr == meshtastic_StoreAndForward_RequestResponse_UNSET);
    assert(nullBaseline.which_variant == 0 && nullBaseline.original_id == 0);
  }
  printf("INFO pinned nanopb empty-null-buffer=%s\n",
         nullPointerAccepted ? "accepted" : "rejected");

  meshtastic_StoreAndForward decoded = meshtastic_StoreAndForward_init_zero;
  decoded.rr = meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS;
  decoded.which_variant = meshtastic_StoreAndForward_text_tag;
  decoded.variant.text.size = 1;
  decoded.variant.text.bytes[0] = 's';
  StoreForwardWire::Error error = StoreForwardWire::Error::Malformed;
  assert(StoreForwardWire::decode(nullptr, 0, &decoded, &error));
  assert(error == StoreForwardWire::Error::None);
  assert(decoded.rr == meshtastic_StoreAndForward_RequestResponse_UNSET);
  assert(decoded.which_variant == 0 && decoded.original_id == 0);

  assert(!StoreForwardWire::decode(nullptr, 1, &decoded, &error));
  assert(error == StoreForwardWire::Error::InvalidArgument);
  assert(decoded.rr == meshtastic_StoreAndForward_RequestResponse_UNSET);
  assert(decoded.which_variant == 0 && decoded.original_id == 0);
  assert(!StoreForwardWire::decode(nullptr, 0, nullptr, &error));
  assert(error == StoreForwardWire::Error::InvalidArgument);

  mt::modules::StoreForwardArchive::Record records[1]{};
  mt::modules::StoreForwardArchive::Cursor cursors[1]{};
  mt::modules::StoreForwardArchive archive(records, 1, cursors, 1);
  mt::modules::StoreForwardControl::Config config{};
  config.enabled = true;
  config.isServer = true;
  mt::modules::StoreForwardControl control(archive, config);
  mt::modules::StoreForwardControl::Context context{};
  context.peer = 12;
  const auto action = control.receive(decoded, context);
  assert(action.kind == mt::modules::StoreForwardControl::ActionKind::None);
  assert(control.requests() == 1); // stock increments before its rr default case
  pass("empty default message and rr UNSET no-op boundary");
}

void encodePinnedSchemaVectors() {
  meshtastic_StoreAndForward msg = meshtastic_StoreAndForward_init_zero;
  msg.rr = meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY;
  msg.which_variant = meshtastic_StoreAndForward_history_tag;
  msg.variant.history.history_messages = 2;
  msg.variant.history.window = 7;
  msg.variant.history.last_request = 9;
  static const uint8_t history[] = {0x08,0x41,0x1a,0x06,0x08,0x02,0x10,0x07,0x18,0x09};
  expectWire(msg, history, sizeof(history));

  msg = meshtastic_StoreAndForward_init_zero;
  msg.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_BROADCAST;
  msg.which_variant = meshtastic_StoreAndForward_text_tag;
  msg.variant.text.size = 3;
  memcpy(msg.variant.text.bytes, "abc", 3);
  msg.original_id = 150;
  static const uint8_t text[] = {0x08,0x09,0x2a,0x03,'a','b','c',0x30,0x96,0x01};
  expectWire(msg, text, sizeof(text));

  msg = meshtastic_StoreAndForward_init_zero;
  msg.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT;
  msg.which_variant = meshtastic_StoreAndForward_heartbeat_tag;
  msg.variant.heartbeat.period = 900;
  msg.variant.heartbeat.secondary = 2;
  static const uint8_t heartbeat[] = {0x08,0x02,0x22,0x05,0x08,0x84,0x07,0x10,0x02};
  expectWire(msg, heartbeat, sizeof(heartbeat));

  msg = meshtastic_StoreAndForward_init_zero;
  msg.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS;
  msg.which_variant = meshtastic_StoreAndForward_stats_tag;
  msg.variant.stats.messages_total = 1;
  msg.variant.stats.heartbeat = true;
  msg.variant.stats.return_window = 240;
  static const uint8_t stats[] = {0x08,0x07,0x12,0x07,0x08,0x01,0x38,0x01,0x48,0xf0,0x01};
  expectWire(msg, stats, sizeof(stats));
  pass("pinned history text heartbeat stats encoding vectors");
}

void boundsAndFailures() {
  meshtastic_StoreAndForward msg = meshtastic_StoreAndForward_init_zero;
  msg.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_DIRECT;
  msg.which_variant = meshtastic_StoreAndForward_text_tag;
  msg.variant.text.size = StoreForwardWire::MaxTextBytes;
  memset(msg.variant.text.bytes, 'x', StoreForwardWire::MaxTextBytes);
  uint8_t encoded[StoreForwardWire::MaxEncodedBytes];
  memset(encoded, 0xa5, sizeof(encoded));
  size_t written = 0;
  StoreForwardWire::Error error;
  assert(StoreForwardWire::encode(msg, encoded, sizeof(encoded), &written, &error));
  assert(written > StoreForwardWire::MaxTextBytes);
  meshtastic_StoreAndForward decoded = meshtastic_StoreAndForward_init_zero;
  assert(StoreForwardWire::decode(encoded, written, &decoded, &error));
  assert(decoded.variant.text.size == StoreForwardWire::MaxTextBytes);

  msg.variant.text.size = StoreForwardWire::MaxTextBytes + 1;
  memset(encoded, 0xa5, sizeof(encoded));
  written = 99;
  assert(!StoreForwardWire::encode(msg, encoded, sizeof(encoded), &written, &error));
  assert(error == StoreForwardWire::Error::TooLarge && written == 0);
  for (uint8_t byte : encoded) assert(byte == 0xa5);

  msg.variant.text.size = 1;
  msg.variant.text.bytes[0] = 'x';
  memset(encoded, 0xa5, sizeof(encoded));
  written = 99;
  assert(!StoreForwardWire::encode(msg, encoded, 1, &written, &error));
  assert(error == StoreForwardWire::Error::NoSpace && written == 0);
  for (uint8_t byte : encoded) assert(byte == 0xa5);

  // Truncated nested message and malformed key both fail transactionally.
  static const uint8_t truncated[] = {0x08,0x41,0x1a,0x06,0x08,0x02};
  decoded.rr = meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS;
  assert(!StoreForwardWire::decode(truncated, sizeof(truncated), &decoded, &error));
  assert(error == StoreForwardWire::Error::Malformed);
  assert(decoded.rr == meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS);
  static const uint8_t badKey[] = {0x00};
  assert(!StoreForwardWire::decode(badKey, sizeof(badKey), &decoded, &error));
  assert(error == StoreForwardWire::Error::Malformed);
  uint8_t oversizedText[5 + StoreForwardWire::MaxTextBytes + 1] = {
      0x08, 0x09, 0x2a, 0xea, 0x01};
  memset(oversizedText + 5, 'z', StoreForwardWire::MaxTextBytes + 1);
  assert(!StoreForwardWire::decode(oversizedText, sizeof(oversizedText),
                                   &decoded, &error));
  assert(error == StoreForwardWire::Error::Malformed ||
         error == StoreForwardWire::Error::TooLarge);
  assert(decoded.rr == meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS);
  pass("text bounds output capacity malformed input and transactionality");
}

void stockDispatchShapePolicy() {
  // rr and oneof are not required to form a matching pair by the stock codec;
  // dispatch decides which oneof, if any, is meaningful for each rr.
  meshtastic_StoreAndForward msg = meshtastic_StoreAndForward_init_zero;
  msg.rr = meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY;
  msg.which_variant = meshtastic_StoreAndForward_stats_tag;
  msg.variant.stats.messages_saved = 3;
  uint8_t wire[StoreForwardWire::MaxEncodedBytes];
  size_t written = 0;
  assert(StoreForwardWire::encode(msg, wire, sizeof(wire), &written));
  meshtastic_StoreAndForward decoded = meshtastic_StoreAndForward_init_zero;
  assert(StoreForwardWire::decode(wire, written, &decoded));
  assert(decoded.rr == msg.rr && decoded.which_variant == msg.which_variant);
  assert(decoded.variant.stats.messages_saved == 3);
  pass("rr and oneof remain independent as in stock dispatch");
}
}  // namespace

int main() {
  emptyMessageMatchesPinnedNanopbAndNoopDispatch();
  decodeGoldenVectors();
  enumRangeBoundaryIsTransactional();
  encodePinnedSchemaVectors();
  boundsAndFailures();
  stockDispatchShapePolicy();
  printf("RESULT PASS groups=%u\n", groups);
  return 0;
}
