// SPDX-License-Identifier: GPL-3.0-only
#include "modules/StoreForwardPacket.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "meshtastic/mesh.pb.h"
#include "pb_decode.h"

namespace {
unsigned groups = 0;
void pass(const char *name) {
  ++groups;
  printf("PASS %s\n", name);
}

meshtastic_Data decodeData(const mt::modules::StoreForwardPacket &packet) {
  meshtastic_Data data = meshtastic_Data_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(packet.data(), packet.size());
  assert(pb_decode(&stream, meshtastic_Data_fields, &data));
  assert(stream.bytes_left == 0);
  return data;
}

meshtastic_StoreAndForward decodeStoreForward(const meshtastic_Data &data) {
  assert(data.portnum == meshtastic_PortNum_STORE_FORWARD_APP);
  meshtastic_StoreAndForward message = meshtastic_StoreAndForward_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(data.payload.bytes, data.payload.size);
  assert(pb_decode(&stream, meshtastic_StoreAndForward_fields, &message));
  assert(stream.bytes_left == 0);
  return message;
}

void controlAndErrorFactories() {
  mt::modules::StoreForwardPacket packet;
  meshtastic_StoreAndForward control = meshtastic_StoreAndForward_init_zero;
  control.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY;
  control.which_variant = meshtastic_StoreAndForward_history_tag;
  control.variant.history.history_messages = 2;
  control.variant.history.window = 240000;
  control.variant.history.last_request = 7;

  assert(mt::modules::StoreForwardPacket::control(0x12345678, 0x87654321,
                                                  3, 4, control, packet));
  assert(packet.valid() && packet.from() == 0x12345678 &&
         packet.to() == 0x87654321 && packet.channel() == 3);
  assert(packet.flags() == uint8_t(4 | (4 << 5)));
  meshtastic_Data data = decodeData(packet);
  assert(data.portnum == meshtastic_PortNum_STORE_FORWARD_APP);
  assert(data.has_bitfield && !data.want_response);
  const auto decoded = decodeStoreForward(data);
  assert(decoded.rr == control.rr && decoded.which_variant == control.which_variant);
  assert(decoded.variant.history.history_messages == 2);
  assert(decoded.variant.history.window == 240000);
  assert(decoded.variant.history.last_request == 7);
  // Packet factory has no outer ID input or ID setter: Service allocates the
  // envelope ID at submit time, after this immutable payload/header value.
  pass("control uses port-65 Data and leaves fresh outer ID to Service");

  assert(mt::modules::StoreForwardPacket::error(0x12345678, 0x87654321,
                                                0, 3, true, packet));
  data = decodeData(packet);
  assert(data.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP);
  assert(data.payload.size == strlen("S&F - Busy. Try again shortly."));
  assert(memcmp(data.payload.bytes, "S&F - Busy. Try again shortly.", data.payload.size) == 0);
  assert(data.has_bitfield && !data.want_response);
  pass("busy error is normal unicast text with no reply request");

  assert(mt::modules::StoreForwardPacket::error(0x12345678, 0x87654321,
                                                1, 2, false, packet));
  data = decodeData(packet);
  assert(data.payload.size == strlen("S&F not permitted on the public channel."));
  assert(memcmp(data.payload.bytes, "S&F not permitted on the public channel.", data.payload.size) == 0);
  pass("public-channel denial text exact");
}

mt::modules::StoreForwardArchive::Record makeRecord(uint32_t to) {
  mt::modules::StoreForwardArchive::Record record{};
  record.time = 1234;
  record.to = to;
  record.from = 0xA1B2C3D4;
  record.id = 0x1234ABCD;
  record.replyId = 0x89ABCDEF;
  record.channel = 2;
  record.hopStart = 5;
  record.hopLimit = 3;
  record.viaMqtt = true;
  record.transportMechanism = 1;
  record.emoji = true;
  const char text[] = "archived body";
  record.payloadSize = sizeof(text) - 1;
  memcpy(record.payload, text, record.payloadSize);
  return record;
}

void replayEnvelopeAndMetadata() {
  const uint32_t requester = 0x55667788;
  auto record = makeRecord(UINT32_MAX);
  mt::modules::StoreForwardPacket packet;
  assert(mt::modules::StoreForwardPacket::replay(requester, record, packet));
  assert(packet.valid() && packet.from() == record.from && packet.to() == requester);
  assert(packet.channel() == record.channel);
  assert(packet.flags() == uint8_t(record.hopLimit | (record.hopStart << 5) | 0x10));

  const meshtastic_Data data = decodeData(packet);
  assert(data.has_bitfield && !data.want_response);
  assert(data.reply_id == record.replyId && data.emoji == 1);
  const auto message = decodeStoreForward(data);
  assert(message.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_BROADCAST);
  assert(message.which_variant == meshtastic_StoreAndForward_text_tag);
  assert(message.original_id == record.id);
  assert(message.variant.text.size == record.payloadSize);
  assert(memcmp(message.variant.text.bytes, record.payload, record.payloadSize) == 0);
  pass("broadcast replay preserves unauthenticated archived from, fresh-ID boundary, channel, hops, MQTT, reply and emoji");

  record = makeRecord(requester);
  assert(mt::modules::StoreForwardPacket::replay(requester, record, packet));
  const auto direct = decodeStoreForward(decodeData(packet));
  assert(packet.to() == requester);
  assert(direct.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_DIRECT);
  assert(direct.original_id == record.id);
  pass("direct replay only addresses its archived recipient");
}

void eligibilityAndInvalidHeaderRejection() {
  const uint32_t requester = 0x55667788;
  auto record = makeRecord(UINT32_MAX);
  record.from = requester;
  mt::modules::StoreForwardPacket packet;
  assert(!mt::modules::StoreForwardPacket::replay(requester, record, packet));
  record = makeRecord(0x11112222);  // direct to a different peer
  assert(!mt::modules::StoreForwardPacket::replay(requester, record, packet));

  record = makeRecord(UINT32_MAX);
  record.from = 0;
  assert(!mt::modules::StoreForwardPacket::replay(requester, record, packet));
  record.from = UINT32_MAX;
  assert(!mt::modules::StoreForwardPacket::replay(requester, record, packet));
  record = makeRecord(UINT32_MAX);
  record.channel = 8;
  assert(!mt::modules::StoreForwardPacket::replay(requester, record, packet));
  record = makeRecord(UINT32_MAX);
  record.hopLimit = 6;
  assert(!mt::modules::StoreForwardPacket::replay(requester, record, packet));
  record = makeRecord(UINT32_MAX);
  record.hopStart = 8;
  assert(!mt::modules::StoreForwardPacket::replay(requester, record, packet));

  meshtastic_StoreAndForward control = meshtastic_StoreAndForward_init_zero;
  control.rr = meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS;
  assert(!mt::modules::StoreForwardPacket::control(0, requester, 0, 3, control, packet));
  assert(!mt::modules::StoreForwardPacket::control(requester, requester, 0, 3, control, packet));
  assert(!mt::modules::StoreForwardPacket::control(0x12345678, requester, 8, 3, control, packet));
  assert(!mt::modules::StoreForwardPacket::control(0x12345678, requester, 0, 8, control, packet));
  pass("requester/privacy, identity, channel and hop bounds reject invalid envelopes");
}

void nestedTextBoundsAreAtomic() {
  const uint32_t requester = 0x55667788;
  auto record = makeRecord(UINT32_MAX);
  mt::modules::StoreForwardPacket packet;
  assert(mt::modules::StoreForwardPacket::replay(requester, record, packet));
  uint8_t previous[239];
  memcpy(previous, packet.data(), packet.size());
  const size_t previousSize = packet.size();
  const uint32_t previousFrom = packet.from();
  const uint32_t previousTo = packet.to();
  const uint8_t previousChannel = packet.channel();
  const uint8_t previousFlags = packet.flags();

  record.payloadSize = 233;
  memset(record.payload, 'x', record.payloadSize);
  assert(!mt::modules::StoreForwardPacket::replay(requester, record, packet));
  assert(packet.valid() && packet.size() == previousSize);
  assert(packet.from() == previousFrom && packet.to() == previousTo &&
         packet.channel() == previousChannel && packet.flags() == previousFlags);
  assert(memcmp(packet.data(), previous, previousSize) == 0);

  // A 200-byte text remains within the enclosing 233-byte Data.payload.
  record.payloadSize = 200;
  assert(mt::modules::StoreForwardPacket::replay(requester, record, packet));
  const auto data = decodeData(packet);
  assert(data.payload.size <= 233);
  const auto message = decodeStoreForward(data);
  assert(message.variant.text.size == 200);
  assert(memcmp(message.variant.text.bytes, record.payload, 200) == 0);
  pass("233-byte nested payload fails atomically; 200-byte replay encodes fully");
}
}  // namespace

int main() {
  controlAndErrorFactories();
  replayEnvelopeAndMetadata();
  eligibilityAndInvalidHeaderRejection();
  nestedTextBoundsAreAtomic();
  printf("RESULT PASS groups=%u\n", groups);
  return 0;
}
