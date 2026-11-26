// SPDX-License-Identifier: GPL-3.0-only
// PeerDeviceMetrics: pure-RAM 21-byte holder for volatile peer device metrics.
//
// Fixed layout, all scalar transfers via memcpy (no casts, no
// alignment-dependent reads, no heap, no external state):
//   bytes_[0]     presence flags, bits 0..4 in field order:
//                 bit0 battery_level, bit1 voltage, bit2 channel_utilization,
//                 bit3 air_util_tx, bit4 uptime_seconds
//   bytes_[1..4]  battery_level        (uint32_t; >100 means powered, never clamped)
//   bytes_[5..8]  voltage              (float)
//   bytes_[9..12] channel_utilization  (float, percent)
//   bytes_[13..16] air_util_tx        (float, percent)
//   bytes_[17..20] uptime_seconds     (uint32_t)
#pragma once

#include <stdint.h>
#include <string.h>
#include <float.h>

static_assert(sizeof(float) == 4, "float must be 4 bytes");
static_assert(sizeof(uint32_t) == 4, "uint32_t must be 4 bytes");

namespace mt {

class PeerDeviceMetrics {
  uint8_t bytes_[21]{};

 public:
  // True iff any optional field is present.
  bool present() const { return (bytes_[0] & 0x1Fu) != 0; }

  // Validates all present values before touching self; rejects empty
  // snapshots. Absent field values are never inspected. On success the whole
  // holder is replaced by a zero-initialized temporary carrying exactly the
  // present fields (no merge; absent fields clear prior values).
  template <class Metrics>
  bool assign(const Metrics &m) {
    uint8_t flags = 0;
    uint32_t battery = 0;
    uint32_t uptime = 0;
    float voltage = 0.0f;
    float channel = 0.0f;
    float air = 0.0f;
    if (m.has_battery_level) {
      memcpy(&battery, &m.battery_level, 4);
      flags |= 0x01u;
    }
    if (m.has_voltage) {
      memcpy(&voltage, &m.voltage, 4);
      if (!(voltage >= 0.0f && voltage <= FLT_MAX)) return false;
      flags |= 0x02u;
    }
    if (m.has_channel_utilization) {
      memcpy(&channel, &m.channel_utilization, 4);
      if (!(channel >= 0.0f && channel <= 100.0f)) return false;
      flags |= 0x04u;
    }
    if (m.has_air_util_tx) {
      memcpy(&air, &m.air_util_tx, 4);
      if (!(air >= 0.0f && air <= 100.0f)) return false;
      flags |= 0x08u;
    }
    if (m.has_uptime_seconds) {
      memcpy(&uptime, &m.uptime_seconds, 4);
      flags |= 0x10u;
    }
    if (flags == 0) return false;
    PeerDeviceMetrics next;
    next.bytes_[0] = flags;
    if (flags & 0x01u) memcpy(&next.bytes_[1], &battery, 4);
    if (flags & 0x02u) memcpy(&next.bytes_[5], &voltage, 4);
    if (flags & 0x04u) memcpy(&next.bytes_[9], &channel, 4);
    if (flags & 0x08u) memcpy(&next.bytes_[13], &air, 4);
    if (flags & 0x10u) memcpy(&next.bytes_[17], &uptime, 4);
    memcpy(bytes_, next.bytes_, 21);
    return true;
  }

  // Zero-initializes the whole destination, then mirrors the presence flags
  // and present scalar slots from the holder. Unknown/absent scalars remain
  // zero. Never mutates the holder.
  template <class Metrics>
  void exportTo(Metrics &m) const {
    m = Metrics{};
    const uint8_t flags = bytes_[0];
    m.has_battery_level = (flags & 0x01u) != 0;
    m.has_voltage = (flags & 0x02u) != 0;
    m.has_channel_utilization = (flags & 0x04u) != 0;
    m.has_air_util_tx = (flags & 0x08u) != 0;
    m.has_uptime_seconds = (flags & 0x10u) != 0;
    if (m.has_battery_level) memcpy(&m.battery_level, &bytes_[1], 4);
    if (m.has_voltage) memcpy(&m.voltage, &bytes_[5], 4);
    if (m.has_channel_utilization) memcpy(&m.channel_utilization, &bytes_[9], 4);
    if (m.has_air_util_tx) memcpy(&m.air_util_tx, &bytes_[13], 4);
    if (m.has_uptime_seconds) memcpy(&m.uptime_seconds, &bytes_[17], 4);
  }
};

static_assert(sizeof(PeerDeviceMetrics) == 21, "Volatile peer metrics budget");

}  // namespace mt
