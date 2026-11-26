// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include "meshtastic/module_config.pb.h"

namespace mt::modules {

// Canonical, fixed-size representation for the two stock GPIO module
// submessages. It does not map pins to hardware offsets or authorize enabling
// either module; the runtime caller must check reviewed board capabilities.
class GpioModuleSettings {
public:
  static constexpr size_t Size = 152;
  static constexpr size_t HeaderSize = 8;
  static constexpr size_t RemoteCapacity = meshtastic_ModuleConfig_RemoteHardwareConfig_size;
  static constexpr size_t DetectionCapacity = meshtastic_ModuleConfig_DetectionSensorConfig_size;
  static constexpr size_t RemoteOffset = HeaderSize;
  static constexpr size_t DetectionOffset = RemoteOffset + RemoteCapacity;
  static constexpr size_t CrcOffset = Size - 4;
  static constexpr uint8_t Version = 1;
  static constexpr uint32_t RemoteTag = meshtastic_ModuleConfig_remote_hardware_tag;
  static constexpr uint32_t DetectionTag = meshtastic_ModuleConfig_detection_sensor_tag;

private:
  meshtastic_ModuleConfig_RemoteHardwareConfig remote_{};
  meshtastic_ModuleConfig_DetectionSensorConfig detection_{};

  static uint16_t get16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
  }
  static void put16(uint8_t *p, uint16_t value) {
    p[0] = uint8_t(value); p[1] = uint8_t(value >> 8);
  }
  static uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  }
  static void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
  }
  static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < n; ++i) {
      crc ^= p[i];
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ (0xedb88320u & uint32_t(-int32_t(crc & 1)));
    }
    return ~crc;
  }
  template <class E>
  static bool enumValue(const E &value, uint32_t &out) {
    using U = __underlying_type(E);
    static_assert(sizeof(U) <= sizeof(uint32_t), "protobuf enum storage width");
    U raw{};
    memcpy(&raw, &value, sizeof(raw));
    // Converting a negative signed representation to uint64_t exceeds the
    // uint32_t domain. This also works with short enums and needs no STL.
    if (uint64_t(raw) > UINT32_MAX) return false;
    out = static_cast<uint32_t>(raw);
    return true;
  }
  static bool validBool(const bool &value) {
    static_assert(sizeof(bool) == 1, "nanopb bool representation expected");
    uint8_t raw = 0;
    memcpy(&raw, &value, sizeof(raw));
    return raw <= 1;
  }
  template <size_t N>
  static bool canonicalText(const char (&text)[N]) {
    const size_t length = strnlen(text, N);
    if (length == N) return false;
    for (size_t i = length + 1; i < N; ++i)
      if (text[i] != 0) return false;
    return true;
  }
  static bool zeroPin(const meshtastic_RemoteHardwarePin &pin) {
    uint32_t type = 0;
    if (!enumValue(pin.type, type) || pin.gpio_pin != 0 || type != 0)
      return false;
    for (char ch : pin.name) if (ch != 0) return false;
    return true;
  }
  static bool validRemote(const meshtastic_ModuleConfig_RemoteHardwareConfig &c) {
    if (!validBool(c.enabled) || !validBool(c.allow_undefined_pin_access) ||
        c.available_pins_count > 4) return false;
    for (pb_size_t i = 0; i < c.available_pins_count; ++i) {
      const auto &pin = c.available_pins[i];
      uint32_t type = 0;
      if (pin.gpio_pin >= 64 || !canonicalText(pin.name) ||
          !enumValue(pin.type, type) || type > uint32_t(meshtastic_RemoteHardwarePinType_DIGITAL_WRITE))
        return false;
    }
    for (pb_size_t i = c.available_pins_count; i < 4; ++i)
      if (!zeroPin(c.available_pins[i])) return false;
    return true;
  }
  static bool validDetection(const meshtastic_ModuleConfig_DetectionSensorConfig &c) {
    uint32_t trigger = 0;
    return validBool(c.enabled) && validBool(c.send_bell) && validBool(c.use_pullup) &&
           canonicalText(c.name) && c.monitor_pin < 64 &&
           enumValue(c.detection_trigger_type, trigger) && trigger <= 5;
  }
  template <class Message>
  static bool encodeMessage(const pb_msgdesc_t *fields, const Message &message,
                            uint8_t *out, size_t capacity, size_t &length) {
    auto stream = pb_ostream_from_buffer(out, capacity);
    if (!pb_encode(&stream, fields, &message)) return false;
    length = stream.bytes_written;
    return length <= capacity;
  }
  template <class Message>
  static bool decodeMessage(const pb_msgdesc_t *fields, const uint8_t *input,
                            size_t length, Message &message) {
    auto stream = pb_istream_from_buffer(input, length);
    return pb_decode(&stream, fields, &message) && stream.bytes_left == 0;
  }
  static bool build(const meshtastic_ModuleConfig_RemoteHardwareConfig &remote,
                    const meshtastic_ModuleConfig_DetectionSensorConfig &detection,
                    uint8_t *out) {
    if (!out || !validRemote(remote) || !validDetection(detection)) return false;
    memset(out, 0, Size);
    out[0] = 'Z'; out[1] = 'G'; out[2] = 'M'; out[3] = Version;
    size_t remoteLength = 0, detectionLength = 0;
    if (!encodeMessage(meshtastic_ModuleConfig_RemoteHardwareConfig_fields,
                       remote, out + RemoteOffset, RemoteCapacity, remoteLength) ||
        !encodeMessage(meshtastic_ModuleConfig_DetectionSensorConfig_fields,
                       detection, out + DetectionOffset, DetectionCapacity, detectionLength))
      return false;
    put16(out + 4, uint16_t(remoteLength));
    put16(out + 6, uint16_t(detectionLength));
    put32(out + CrcOffset, crc32(out, CrcOffset));
    return true;
  }

public:
  GpioModuleSettings() {
    remote_ = meshtastic_ModuleConfig_RemoteHardwareConfig_init_zero;
    detection_ = meshtastic_ModuleConfig_DetectionSensorConfig_init_zero;
    // Match pinned stock NodeDB defaults, which differ from the generated
    // nanopb initializer for minimum interval and trigger type.
    detection_.minimum_broadcast_secs = 45;
    detection_.detection_trigger_type =
        meshtastic_ModuleConfig_DetectionSensorConfig_TriggerType_LOGIC_HIGH;
  }

  bool valid() const {
    uint8_t record[Size];
    return build(remote_, detection_, record);
  }
  bool same(const GpioModuleSettings &other) const {
    uint8_t a[Size], b[Size];
    return build(remote_, detection_, a) && build(other.remote_, other.detection_, b) &&
           memcmp(a, b, Size) == 0;
  }
  const meshtastic_ModuleConfig_RemoteHardwareConfig &remote() const { return remote_; }
  const meshtastic_ModuleConfig_DetectionSensorConfig &detection() const { return detection_; }

  bool get(meshtastic_ModuleConfig &out, unsigned tag) const {
    if (!valid()) return false;
    meshtastic_ModuleConfig candidate{};
    if (tag == RemoteTag) {
      candidate.which_payload_variant = RemoteTag;
      candidate.payload_variant.remote_hardware = remote_;
    } else if (tag == DetectionTag) {
      candidate.which_payload_variant = DetectionTag;
      candidate.payload_variant.detection_sensor = detection_;
    } else {
      return false;
    }
    out = candidate;
    return true;
  }

  bool set(const meshtastic_ModuleConfig &input) {
    GpioModuleSettings candidate = *this;
    if (input.which_payload_variant == RemoteTag) {
      if (!validRemote(input.payload_variant.remote_hardware)) return false;
      candidate.remote_ = input.payload_variant.remote_hardware;
    } else if (input.which_payload_variant == DetectionTag) {
      if (!validDetection(input.payload_variant.detection_sensor)) return false;
      candidate.detection_ = input.payload_variant.detection_sensor;
    } else {
      return false;
    }
    if (!candidate.valid()) return false;
    *this = candidate;
    return true;
  }

  bool encode(uint8_t *out, size_t capacity) const {
    if (!out || capacity < Size) return false;
    uint8_t candidate[Size];
    if (!build(remote_, detection_, candidate)) return false;
    memcpy(out, candidate, Size);
    return true;
  }

  static bool decode(const uint8_t *input, size_t length, GpioModuleSettings &out) {
    if (!input || length != Size || input[0] != 'Z' || input[1] != 'G' ||
        input[2] != 'M' || input[3] != Version ||
        get32(input + CrcOffset) != crc32(input, CrcOffset)) return false;
    const size_t remoteLength = get16(input + 4);
    const size_t detectionLength = get16(input + 6);
    if (remoteLength > RemoteCapacity || detectionLength > DetectionCapacity) return false;
    for (size_t i = RemoteOffset + remoteLength; i < DetectionOffset; ++i)
      if (input[i]) return false;
    for (size_t i = DetectionOffset + detectionLength; i < CrcOffset; ++i)
      if (input[i]) return false;

    GpioModuleSettings candidate;
    // Storage is protobuf data, so absent fields must decode as protobuf
    // zero. The constructor's stock-facing defaults are applied only when no
    // journal record exists, not while decoding a persisted submessage.
    candidate.remote_ = meshtastic_ModuleConfig_RemoteHardwareConfig_init_zero;
    candidate.detection_ = meshtastic_ModuleConfig_DetectionSensorConfig_init_zero;
    if (!decodeMessage(meshtastic_ModuleConfig_RemoteHardwareConfig_fields,
                       input + RemoteOffset, remoteLength, candidate.remote_) ||
        !decodeMessage(meshtastic_ModuleConfig_DetectionSensorConfig_fields,
                       input + DetectionOffset, detectionLength, candidate.detection_) ||
        !candidate.valid()) return false;
    uint8_t canonical[Size];
    if (!candidate.encode(canonical, sizeof(canonical)) ||
        memcmp(canonical, input, Size) != 0) return false;
    out = candidate;
    return true;
  }
};

static_assert(GpioModuleSettings::RemoteCapacity == 96);
static_assert(GpioModuleSettings::DetectionCapacity == 44);
static_assert(GpioModuleSettings::Size == 8 + 96 + 44 + 4);

}  // namespace mt::modules
