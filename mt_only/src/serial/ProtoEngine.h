// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mt_serial {
constexpr size_t kMaxPhoneApiPayload = 512; // stock MAX_TO_FROM_RADIO_SIZE
using ToRadio = void (*)(void *context, const uint8_t *protobuf, size_t size);
using FromRadio = size_t (*)(void *context, uint8_t *protobuf, size_t capacity);
using ProtoWrite = void (*)(void *context, const uint8_t *bytes, size_t size);

// Stock StreamAPI framing: 0x94 0xC3, then a 16-bit big-endian protobuf size.
// Payloads are delegated to the existing PhoneAPI owner; no mesh packet or
// radio path is synthesized here.
class ProtoEngine {
public:
  bool receive(const uint8_t *bytes, size_t size, ToRadio toRadio, void *context);
  bool poll(FromRadio fromRadio, ProtoWrite write, void *context);
  bool poll(FromRadio fromRadio, void *fromContext,
            ProtoWrite write, void *writeContext);
  static bool emit(const uint8_t *protobuf, size_t size, ProtoWrite write, void *context);
  static size_t frame(const uint8_t *protobuf, size_t size, uint8_t *out, size_t capacity);
  void reset() { state_=State::Marker0; payloadUsed_=expected_=0; }
  size_t pending() const { return payloadUsed_; }

private:
  enum class State : uint8_t { Marker0, Marker1, LengthHigh, LengthLow, Payload };
  State state_ = State::Marker0;
  uint8_t payload_[kMaxPhoneApiPayload]{};
  size_t payloadUsed_ = 0;
  size_t expected_ = 0;
  bool consume(uint8_t byte, ToRadio callback, void *context);
};
} // namespace mt_serial
