// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Bounded codec and single-owner queue for the stock MQTT client-proxy
// protobuf. Transport and broker policy are supplied by MqttRuntime.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../stock/mqtt/ClientProxy.h"
#include <pb_decode.h>
#include <pb_encode.h>

namespace mt::mqtt::client_proxy {

constexpr size_t kTopicStorageBytes = 60;
constexpr size_t kMaxTopicBytes = kTopicStorageBytes - 1;
constexpr size_t kPayloadStorageBytes = 435;
constexpr size_t kMaxBinaryBytes = kPayloadStorageBytes;
constexpr size_t kMaxTextBytes = kPayloadStorageBytes - 1;
constexpr size_t kMaxProtobufBytes = 512;
constexpr size_t kSerialFrameHeaderBytes = 4;
constexpr size_t kMaxSerialFrameBytes = kMaxProtobufBytes + kSerialFrameHeaderBytes;

static_assert(sizeof(((meshtastic_MqttClientProxyMessage *)nullptr)->topic) == kTopicStorageBytes,
              "Pinned stock MQTT proxy topic size changed");
static_assert(sizeof(((meshtastic_MqttClientProxyMessage *)nullptr)->payload_variant.data.bytes) ==
                  kPayloadStorageBytes,
              "Pinned stock MQTT proxy binary payload bound changed");
static_assert(sizeof(((meshtastic_MqttClientProxyMessage *)nullptr)->payload_variant.text) ==
                  kPayloadStorageBytes,
              "Pinned stock MQTT proxy text payload bound changed");
static_assert(meshtastic_MqttClientProxyMessage_size == 501,
              "Pinned stock MQTT proxy protobuf bound changed");
static_assert(meshtastic_ToRadio_mqttClientProxyMessage_tag == 6,
              "Pinned stock ToRadio proxy tag changed");
static_assert(meshtastic_FromRadio_mqttClientProxyMessage_tag == 14,
              "Pinned stock FromRadio proxy tag changed");
static_assert(meshtastic_MqttClientProxyMessage_size + 9 <= kMaxProtobufBytes,
              "Largest stock FromRadio proxy wrapper exceeds Companion body bound");
static_assert(kMaxSerialFrameBytes == 516, "Companion frame limit changed");

enum class PayloadKind : uint8_t { Binary, Text };

// Wire-independent owned message. Binary `length` is authoritative. Text
// payloads are bytes here and are NUL-terminated only when encoded to protobuf.
struct Message {
  char topic[kTopicStorageBytes]{};
  uint8_t payload[kPayloadStorageBytes]{};
  uint16_t length = 0;
  PayloadKind kind = PayloadKind::Binary;
  bool retained = false;
};
static_assert(__is_trivially_copyable(Message),
              "MQTT proxy message must remain fixed-copyable");
static_assert(sizeof(Message) <= 512,
              "One stock proxy message must stay within the bounded core storage budget");

inline void wipe(void *ptr, size_t size) {
  volatile uint8_t *p = static_cast<volatile uint8_t *>(ptr);
  while (size--) *p++ = 0;
}

inline void clear(Message &message) { wipe(&message, sizeof(message)); }

inline bool boundedCString(const char *text, size_t capacity, size_t &length) {
  if (!text || !capacity) return false;
  size_t size = 0;
  while (size < capacity && text[size] != '\0') ++size;
  if (size == capacity) return false;
  // Reject decoded C strings with hidden suffix bytes; a later protobuf encode
  // would otherwise silently shorten the received topic or text payload.
  for (size_t i = size + 1; i < capacity; ++i)
    if (text[i] != '\0') return false;
  length = size;
  return true;
}

inline bool valid(const Message &message) {
  size_t topicLength = 0;
  if (!boundedCString(message.topic, sizeof(message.topic), topicLength) ||
      topicLength == 0 || topicLength > kMaxTopicBytes ||
      message.length > kMaxBinaryBytes)
    return false;
  if (message.kind == PayloadKind::Text) {
    if (message.length > kMaxTextBytes) return false;
    for (size_t i = 0; i < message.length; ++i)
      if (message.payload[i] == 0) return false;
  } else if (message.kind != PayloadKind::Binary) {
    return false;
  }
  return true;
}

template<class Receive>
inline bool dispatchProtobuf(const meshtastic_MqttClientProxyMessage &message, Receive receive) {
  size_t length = 0;
  return boundedCString(message.topic, sizeof(message.topic), length) && length &&
         stock::MQTT::onClientProxyReceive(message, receive);
}

inline bool validProtobuf(const meshtastic_MqttClientProxyMessage &message) {
  return dispatchProtobuf(message, [](const char *, const uint8_t *, size_t) { return true; });
}

inline bool fromProtobuf(const meshtastic_MqttClientProxyMessage &input, Message &output) {
  clear(output);
  return dispatchProtobuf(input, [&](const char *topic, const uint8_t *payload, size_t length) {
    memcpy(output.topic, topic, sizeof(output.topic));
    output.retained = input.retained;
    output.kind = input.which_payload_variant == meshtastic_MqttClientProxyMessage_data_tag
                      ? PayloadKind::Binary : PayloadKind::Text;
    output.length = static_cast<uint16_t>(length);
    memcpy(output.payload, payload, length);
    return true;
  });
}

inline bool toProtobuf(const Message &input, meshtastic_MqttClientProxyMessage &output) {
  output = meshtastic_MqttClientProxyMessage_init_zero;
  if (!valid(input)) return false;
  size_t topicLength = 0;
  if (!boundedCString(input.topic, sizeof(input.topic), topicLength)) return false;
  memcpy(output.topic, input.topic, topicLength + 1);
  output.retained = input.retained;
  if (input.kind == PayloadKind::Binary) {
    output.which_payload_variant = meshtastic_MqttClientProxyMessage_data_tag;
    output.payload_variant.data.size = input.length;
    if (input.length)
      memcpy(output.payload_variant.data.bytes, input.payload, input.length);
  } else {
    output.which_payload_variant = meshtastic_MqttClientProxyMessage_text_tag;
    if (input.length) memcpy(output.payload_variant.text, input.payload, input.length);
    output.payload_variant.text[input.length] = '\0';
  }
  return true;
}

inline bool fromToRadio(const meshtastic_ToRadio &input, Message &output) {
  clear(output);
  if (input.which_payload_variant != meshtastic_ToRadio_mqttClientProxyMessage_tag)
    return false;
  return fromProtobuf(input.mqttClientProxyMessage, output);
}

inline bool fillToRadio(const Message &input, meshtastic_ToRadio &output) {
  output = meshtastic_ToRadio_init_zero;
  if (!toProtobuf(input, output.mqttClientProxyMessage)) return false;
  output.which_payload_variant = meshtastic_ToRadio_mqttClientProxyMessage_tag;
  return true;
}

inline bool fillFromRadio(const Message &input, uint32_t sequence,
                          meshtastic_FromRadio &output) {
  output = meshtastic_FromRadio_init_zero;
  if (!toProtobuf(input, output.mqttClientProxyMessage)) return false;
  output.id = sequence;
  output.which_payload_variant = meshtastic_FromRadio_mqttClientProxyMessage_tag;
  return true;
}

inline bool decodeToRadio(const uint8_t *wire, size_t wireLength,
                          meshtastic_ToRadio &scratch, Message &output) {
  clear(output);
  if (!wire || !wireLength || wireLength > kMaxProtobufBytes) return false;
  scratch = meshtastic_ToRadio_init_zero;
  auto stream = pb_istream_from_buffer(wire, wireLength);
  if (!pb_decode(&stream, meshtastic_ToRadio_fields, &scratch) || stream.bytes_left ||
      !fromToRadio(scratch, output)) {
    wipe(&scratch, sizeof(scratch));
    clear(output);
    return false;
  }
  wipe(&scratch, sizeof(scratch));
  return true;
}

inline bool encodeToRadio(const Message &input, meshtastic_ToRadio &scratch,
                          uint8_t *wire, size_t capacity, size_t &written) {
  written = 0;
  if (!wire || !capacity || capacity > kMaxProtobufBytes) return false;
  scratch = meshtastic_ToRadio_init_zero;
  if (!fillToRadio(input, scratch)) {
    wipe(&scratch, sizeof(scratch));
    return false;
  }
  auto stream = pb_ostream_from_buffer(wire, capacity);
  const bool ok = pb_encode(&stream, meshtastic_ToRadio_fields, &scratch) &&
                  stream.bytes_written <= kMaxProtobufBytes;
  if (ok) written = stream.bytes_written;
  wipe(&scratch, sizeof(scratch));
  return ok;
}

inline bool encodeFromRadio(const Message &input, uint32_t sequence,
                            meshtastic_FromRadio &scratch, uint8_t *wire,
                            size_t capacity, size_t &written) {
  written = 0;
  if (!wire || !capacity || capacity > kMaxProtobufBytes) return false;
  scratch = meshtastic_FromRadio_init_zero;
  if (!fillFromRadio(input, sequence, scratch)) {
    wipe(&scratch, sizeof(scratch));
    return false;
  }
  auto stream = pb_ostream_from_buffer(wire, capacity);
  const bool ok = pb_encode(&stream, meshtastic_FromRadio_fields, &scratch) &&
                  stream.bytes_written <= kMaxProtobufBytes;
  if (ok) written = stream.bytes_written;
  wipe(&scratch, sizeof(scratch));
  return ok;
}

// Stock MeshService uses eight entries and drops the oldest at capacity. The
// T096 path is constrained, so integration selects Capacity=1 unless storage
// accounting later qualifies a larger PSRAM-backed proxy queue.
template <size_t Capacity = 1>
class Queue {
  static_assert(Capacity > 0, "MQTT proxy queue must have storage");
 public:
  enum class PushResult : uint8_t { Accepted, AcceptedDroppedOldest, InvalidMessage };
  Queue() = default;
  Queue(const Queue &) = delete;
  Queue &operator=(const Queue &) = delete;
  ~Queue() { clear(); }

  PushResult push(const Message &message) {
    if (!valid(message)) return PushResult::InvalidMessage;
    bool dropped = false;
    if (count_ == Capacity) {
      client_proxy::clear(slots_[head_]);
      head_ = (head_ + 1) % Capacity;
      --count_;
      ++droppedOldest_;
      dropped = true;
    }
    const size_t tail = (head_ + count_) % Capacity;
    memcpy(&slots_[tail], &message, sizeof(message));
    ++count_;
    return dropped ? PushResult::AcceptedDroppedOldest : PushResult::Accepted;
  }

  bool pop(Message &output) {
    client_proxy::clear(output);
    if (!count_) return false;
    memcpy(&output, &slots_[head_], sizeof(output));
    client_proxy::clear(slots_[head_]);
    head_ = (head_ + 1) % Capacity;
    --count_;
    return true;
  }

  bool popToProtobuf(meshtastic_MqttClientProxyMessage &output) {
    output = meshtastic_MqttClientProxyMessage_init_zero;
    if (!count_) return false;
    const bool ok = toProtobuf(slots_[head_], output);
    client_proxy::clear(slots_[head_]);
    head_ = (head_ + 1) % Capacity;
    --count_;
    if (!ok) output = meshtastic_MqttClientProxyMessage_init_zero;
    return ok;
  }

  void clear() {
    for (auto &slot : slots_) client_proxy::clear(slot);
    head_ = count_ = droppedOldest_ = 0;
  }
  size_t size() const { return count_; }
  size_t droppedOldest() const { return droppedOldest_; }
  static constexpr size_t capacity() { return Capacity; }

 private:
  Message slots_[Capacity]{};
  size_t head_ = 0, count_ = 0, droppedOldest_ = 0;
};
static_assert(sizeof(Queue<1>) <= 544,
              "T096 core proxy bridge must remain a single bounded slot");

}  // namespace mt::mqtt::client_proxy
