// SPDX-License-Identifier: GPL-3.0-only
#include "network/MqttClientProxy.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace cp = mt::mqtt::client_proxy;

static std::vector<uint8_t> fixture(const char *path) {
  std::ifstream input(path);
  assert(input);
  std::string hex, token;
  while (input >> token) hex += token;
  assert(!(hex.size() & 1));
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < hex.size(); i += 2)
    bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return bytes;
}

static cp::Message binaryMessage(uint8_t seed) {
  cp::Message message{};
  std::strcpy(message.topic, "msh/2/e/lab/!00000001");
  message.kind = cp::PayloadKind::Binary;
  message.length = 4;
  message.payload[0] = seed;
  message.payload[1] = 0;
  message.payload[2] = 0xff;
  message.payload[3] = 0x42;
  message.retained = true;
  return message;
}

static void testStockGoldenWrappers() {
  auto wire = fixture("mt_only/tests/mqtt_client_proxy/fixtures/toradio-binary-retained.hex");
  meshtastic_ToRadio scratch = meshtastic_ToRadio_init_zero;
  cp::Message message{};
  assert(cp::decodeToRadio(wire.data(), wire.size(), scratch, message));
  assert(message.kind == cp::PayloadKind::Binary && message.retained);
  assert(!std::strcmp(message.topic, "msh/2/e/lab/!00000001"));
  assert(message.length == 4 && message.payload[0] == 0 && message.payload[1] == 0x7f &&
         message.payload[2] == 0xff && message.payload[3] == 0x42);
  std::array<uint8_t, cp::kMaxProtobufBytes> encoded{};
  size_t written = 0;
  assert(cp::encodeToRadio(message, scratch, encoded.data(), encoded.size(), written));
  assert(written == wire.size() && !std::memcmp(encoded.data(), wire.data(), written));

  wire = fixture("mt_only/tests/mqtt_client_proxy/fixtures/fromradio-text-retained.hex");
  meshtastic_FromRadio from = meshtastic_FromRadio_init_zero;
  auto input = pb_istream_from_buffer(wire.data(), wire.size());
  assert(pb_decode(&input, meshtastic_FromRadio_fields, &from) && !input.bytes_left);
  assert(from.which_payload_variant == meshtastic_FromRadio_mqttClientProxyMessage_tag);
  cp::Message text{};
  std::strcpy(text.topic, "msh/2/e/lab/!00000001");
  std::memcpy(text.payload, "hello", 5);
  text.length = 5;
  text.kind = cp::PayloadKind::Text;
  text.retained = true;
  from = meshtastic_FromRadio_init_zero;
  assert(cp::fillFromRadio(text, 7, from));
  input = pb_istream_from_buffer(wire.data(), wire.size());
  meshtastic_FromRadio expected = meshtastic_FromRadio_init_zero;
  assert(pb_decode(&input, meshtastic_FromRadio_fields, &expected) && !input.bytes_left);
  assert(from.id == expected.id && from.id == 7);
  assert(from.which_payload_variant == expected.which_payload_variant);
  assert(!std::memcmp(&from.mqttClientProxyMessage, &expected.mqttClientProxyMessage,
                      sizeof(from.mqttClientProxyMessage)));
}

static void testBoundsAndMalformedInputs() {
  cp::Message message = binaryMessage(1);
  assert(cp::valid(message));
  message.topic[sizeof(message.topic) - 1] = 'x';
  assert(!cp::valid(message));
  message = binaryMessage(1);
  message.length = cp::kMaxBinaryBytes + 1;
  assert(!cp::valid(message));

  cp::Message text = binaryMessage(1);
  text.kind = cp::PayloadKind::Text;
  text.length = 5;
  text.payload[1] = 0;
  assert(!cp::valid(text));
  text = binaryMessage(1);
  text.kind = cp::PayloadKind::Text;
  text.length = cp::kMaxTextBytes;
  std::memset(text.payload, 't', text.length);
  assert(cp::valid(text));
  text.length++;
  assert(!cp::valid(text));

  auto bad = fixture("mt_only/tests/mqtt_client_proxy/fixtures/fromradio-text-retained.hex");
  meshtastic_ToRadio scratch = meshtastic_ToRadio_init_zero;
  cp::Message decoded{};
  assert(!cp::decodeToRadio(bad.data(), bad.size(), scratch, decoded));
  assert(!cp::decodeToRadio(nullptr, 1, scratch, decoded));
  assert(!cp::decodeToRadio(bad.data(), cp::kMaxProtobufBytes + 1, scratch, decoded));

  meshtastic_MqttClientProxyMessage proto = meshtastic_MqttClientProxyMessage_init_zero;
  std::strcpy(proto.topic, "topic");
  proto.which_payload_variant = meshtastic_MqttClientProxyMessage_text_tag;
  proto.payload_variant.text[0] = 'a';
  proto.payload_variant.text[1] = '\0';
  proto.payload_variant.text[2] = 'b';
  assert(!cp::validProtobuf(proto));
  proto = meshtastic_MqttClientProxyMessage_init_zero;
  std::strcpy(proto.topic, "topic");
  proto.which_payload_variant = meshtastic_MqttClientProxyMessage_data_tag;
  proto.payload_variant.data.size = cp::kMaxBinaryBytes + 1;
  assert(!cp::validProtobuf(proto));
}

static void testSingleSlotBridgePolicy() {
  cp::Queue<1> queue;
  static_assert(sizeof(cp::Message) <= 512);
  static_assert(sizeof(cp::Queue<1>) <= 544);
  assert(queue.capacity() == 1 && queue.size() == 0);
  cp::Message first = binaryMessage(1), second = binaryMessage(2);
  assert(queue.push(first) == cp::Queue<1>::PushResult::Accepted);
  assert(queue.push(second) == cp::Queue<1>::PushResult::AcceptedDroppedOldest);
  assert(queue.size() == 1 && queue.droppedOldest() == 1);
  meshtastic_MqttClientProxyMessage out = meshtastic_MqttClientProxyMessage_init_zero;
  assert(queue.popToProtobuf(out));
  assert(out.which_payload_variant == meshtastic_MqttClientProxyMessage_data_tag);
  assert(out.payload_variant.data.size == second.length && out.payload_variant.data.bytes[0] == 2);
  assert(queue.size() == 0 && !queue.popToProtobuf(out));

  first.kind = static_cast<cp::PayloadKind>(99);
  assert(queue.push(first) == cp::Queue<1>::PushResult::InvalidMessage);
}

int main() {
  testStockGoldenWrappers();
  testBoundsAndMalformedInputs();
  testSingleSlotBridgePolicy();
}
