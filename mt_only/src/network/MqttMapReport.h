// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../stock/mqtt/MapReport.h"
#include "pb_encode.h"

namespace mt { namespace mqtt {

// Bounded, side-effect-free map reporting primitives matching the stock
// MQTT::perhapsReportToMap field and privacy rules at the pinned revision.
class MqttMapReport {
 public:
  static constexpr uint32_t kDefaultPrecision = 14;
  static constexpr uint32_t kMinPrecision = 12;
  static constexpr uint32_t kMaxPrecision = 15;
  static constexpr uint32_t kDefaultIntervalSeconds = 60u * 60u;
  static constexpr uint32_t kMaxIntervalMillis = 0x7fffffffu;
  static constexpr size_t kMaxEncodedBytes = meshtastic_MapReport_size;
  static constexpr size_t kTopicCapacity = 160;
  static constexpr size_t kRootMaxBytes = 31;  // MQTTConfig.root[32] minus NUL

  enum class Decision : uint8_t {
    Ready,
    Disabled,
    LocationNotOptedIn,
    TransportUnavailable,
    EventChannelSuppressed,
    PositionUnavailable,
    IntervalNotElapsed,
    InvalidInput,
    OutputTooSmall,
    EncodeFailure
  };

  struct Gate {
    bool reportingEnabled = false;
    bool locationOptedIn = false;
    bool transportReady = false;  // Direct broker or an authorized stock proxy.
    bool suppressOnEventChannel = false;
    uint32_t configuredIntervalSeconds = 0;  // zero selects stock default
    uint32_t lastReportMillis = 0;
    uint32_t nowMillis = 0;
  };

  struct Source {
    const char *longName = nullptr;
    const char *shortName = nullptr;
    const char *firmwareVersion = nullptr;
    // Integers allow the boundary to reject invalid enum values without
    // evaluating an out-of-range C++ enum representation (UB under UBSan).
    int32_t role = _meshtastic_Config_DeviceConfig_Role_MIN;
    int32_t hwModel = _meshtastic_HardwareModel_MIN;
    int32_t region = _meshtastic_Config_LoRaConfig_RegionCode_MIN;
    int32_t modemPreset = _meshtastic_Config_LoRaConfig_ModemPreset_MIN;
    bool hasDefaultChannel = false;
    int32_t latitudeI = 0;
    int32_t longitudeI = 0;
    int32_t altitude = 0;
    uint16_t numOnlineLocalNodes = 0;
  };

  // Pure gate. Millis subtraction intentionally uses uint32_t wrap semantics.
  // As in stock, lastReportMillis==0 bypasses throttling for the first report.
  static Decision admission(const Gate &gate, int32_t latitudeI,
                            int32_t longitudeI) {
    if (!gate.reportingEnabled) return Decision::Disabled;
    if (!gate.locationOptedIn) return Decision::LocationNotOptedIn;
    if (!gate.transportReady) return Decision::TransportUnavailable;
    if (gate.suppressOnEventChannel) return Decision::EventChannelSuppressed;
    if (latitudeI == 0 && longitudeI == 0) return Decision::PositionUnavailable;
    uint32_t seconds = gate.configuredIntervalSeconds
                           ? gate.configuredIntervalSeconds
                           : kDefaultIntervalSeconds;
    // Stock NodeDB normalizes configured map intervals below the one-hour
    // minimum to the default. Default::getConfiguredOrDefaultMs then saturates
    // large seconds values at INT32_MAX milliseconds.
    if (seconds < kDefaultIntervalSeconds) seconds = kDefaultIntervalSeconds;
    const uint32_t intervalMillis = seconds > kMaxIntervalMillis / 1000u
                                        ? kMaxIntervalMillis
                                        : seconds * 1000u;
    if (gate.lastReportMillis != 0 &&
        uint32_t(gate.nowMillis - gate.lastReportMillis) < intervalMillis)
      return Decision::IntervalNotElapsed;
    return Decision::Ready;
  }

  // Stock rounds two's-complement latitude_i/longitude_i via unsigned mask
  // followed by half-quantum addition. Precision has already been clamped to
  // [12,15] by effectivePrecision().
  static uint32_t effectivePrecision(uint32_t configured) {
    return stock::MQTT::mapPrecision(configured);
  }

  static int32_t quantize(int32_t coordinate, uint32_t precision) {
    return stock::MQTT::mapCoordinate(coordinate, effectivePrecision(precision));
  }

  // Encodes MapReport protobuf bytes only. Metadata strings are copied with
  // stock bounds (24/4/17 data bytes plus NUL); shortName is bounded to its
  // four stock bytes. On any failure, out and *outLength are unchanged.
  static Decision encode(const Gate &gate, uint32_t configuredPrecision,
                         const Source &source, uint8_t *out, size_t capacity,
                         size_t *outLength) {
    if (!out || !outLength) return Decision::InvalidInput;
    const Decision allowed = admission(gate, source.latitudeI,
                                       source.longitudeI);
    if (allowed != Decision::Ready) return allowed;
    if (!source.longName || !source.shortName || !source.firmwareVersion ||
        !validEnum(source.role, _meshtastic_Config_DeviceConfig_Role_MIN,
                   _meshtastic_Config_DeviceConfig_Role_MAX) ||
        !validEnum(source.hwModel, _meshtastic_HardwareModel_MIN,
                   _meshtastic_HardwareModel_MAX) ||
        !validEnum(source.region, _meshtastic_Config_LoRaConfig_RegionCode_MIN,
                   _meshtastic_Config_LoRaConfig_RegionCode_MAX) ||
        !validEnum(source.modemPreset,
                   _meshtastic_Config_LoRaConfig_ModemPreset_MIN,
                   _meshtastic_Config_LoRaConfig_ModemPreset_MAX))
      return Decision::InvalidInput;

    meshtastic_MapReport report = meshtastic_MapReport_init_default;
    stock::MQTT::fillMapReport(report, source, configuredPrecision);

    size_t measured = 0;
    if (!pb_get_encoded_size(&measured, meshtastic_MapReport_fields, &report) ||
        measured == 0 || measured > kMaxEncodedBytes)
      return Decision::EncodeFailure;
    if (measured > capacity) return Decision::OutputTooSmall;
    uint8_t scratch[kMaxEncodedBytes]{};
    pb_ostream_t stream = pb_ostream_from_buffer(scratch, sizeof(scratch));
    if (!pb_encode(&stream, meshtastic_MapReport_fields, &report) ||
        stream.bytes_written != measured || stream.bytes_written > capacity)
      return Decision::EncodeFailure;
    memcpy(out, scratch, stream.bytes_written);
    *outLength = stream.bytes_written;
    return Decision::Ready;
  }

  // Stock map topic is <configured-root-or-msh>/2/map/.
  // Reject MQTT wildcards and embedded NUL is naturally bounded by C strings.
  static bool topic(const char *configuredRoot, char *out, size_t capacity,
                    size_t *outLength) {
    if (!out || !outLength || !capacity) return false;
    const char *root = configuredRoot && configuredRoot[0]
                           ? configuredRoot
                           : "msh";
    size_t rootLength = 0;
    while (rootLength < kRootMaxBytes && root[rootLength]) {
      if (root[rootLength] == '+' || root[rootLength] == '#') return false;
      ++rootLength;
    }
    // Stock root is char[32]: 31 data bytes plus a required NUL at index 31.
    // The 32nd data byte is rejected, but the valid 31-byte boundary is not.
    if (rootLength == kRootMaxBytes && root[rootLength] != '\0') return false;
    const char suffix[] = "/2/map/";
    const size_t needed = rootLength + sizeof(suffix) - 1;
    if (needed >= capacity || needed >= kTopicCapacity) return false;
    char scratch[kTopicCapacity]{};
    memcpy(scratch, root, rootLength);
    memcpy(scratch + rootLength, suffix, sizeof(suffix) - 1);
    memcpy(out, scratch, needed);
    out[needed] = '\0';
    *outLength = needed;
    return true;
  }

 private:
  static bool validEnum(int32_t value, int minimum, int maximum) {
    return value >= minimum && value <= maximum;
  }

};

}}  // namespace mt::mqtt
