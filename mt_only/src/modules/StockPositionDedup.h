// SPDX-License-Identifier: GPL-3.0-only
// Per-source position gate from pinned Meshtastic 8e6a88d. Uses the
// grid-center coordinates and shifted four-bit packed fingerprint; zero
// fingerprints map to 0xff. The owning cache must expire the compact ticks.
#pragma once
#include <stdint.h>
#include "../stock/position/PositionPrecision.h"
#include "../stock/modules/TrafficManagementModule.h"
#include "meshtastic/config.pb.h"

namespace mt {
namespace traffic {

class StockPositionDedup {
public:
  static uint8_t fingerprint(int32_t latitude_i, int32_t longitude_i,
                             uint32_t configuredPrecisionBits) {
    const uint8_t p = stock::TrafficManagementModule::sanitizePositionPrecision(static_cast<uint8_t>(configuredPrecisionBits));
    return stock::TrafficManagementModule::computePositionFingerprint(stock::truncateCoordinate(latitude_i, p),
                                                                       stock::truncateCoordinate(longitude_i, p), p);
  }

  bool shouldDrop(bool hasLatitude, bool hasLongitude, int32_t latitude_i,
                  int32_t longitude_i, uint32_t configuredPrecisionBits,
                  uint32_t configuredMinIntervalSeconds, uint32_t roleCode,
                  uint8_t nowPositionTick) {
    // Missing coordinate: do not drop, do not mutate the slot.
    if (!hasLatitude || !hasLongitude) return false;

    // Role caps shorten (never lengthen) nonzero intervals, in seconds.
    uint32_t intervalSec = configuredMinIntervalSeconds;
    if (intervalSec != 0) {
      if (roleCode == kRoleTracker || roleCode == kRoleTakTracker) {
        if (intervalSec > kTrackerCapSeconds) intervalSec = kTrackerCapSeconds;
      } else if (roleCode == kRoleLostAndFound) {
        if (intervalSec > kLostAndFoundCapSeconds)
          intervalSec = kLostAndFoundCapSeconds;
      }
    }
    uint32_t intervalMs = saturatingSecondsToMs(intervalSec);

    // Zero ms -> windowTicks 0 (dropping disabled); nonzero -> clamped ticks.
    uint8_t windowTicks = 0;
    if (intervalMs != 0) {
      uint32_t ticks = intervalMs / kPosTimeTickMs;
      if (ticks < 1) ticks = 1;
      if (ticks > 255) ticks = 255;
      windowTicks = static_cast<uint8_t>(ticks);
    }

    uint8_t currentFp =
        fingerprint(latitude_i, longitude_i, configuredPrecisionBits);

    bool drop = false;
    if (initialized_ && windowTicks > 0 && fingerprint_ == currentFp) {
      uint8_t delta = static_cast<uint8_t>(nowPositionTick - positionTick_);
      if (delta < windowTicks) drop = true;
    }

    // Stamp only when allowed; a denied duplicate must not slide the window.
    if (!drop) {
      fingerprint_ = currentFp;
      positionTick_ = nowPositionTick;
      initialized_ = true;
    }
    return drop;
  }

  void reset() {
    fingerprint_ = 0;
    positionTick_ = 0;
    initialized_ = false;
  }

private:
  uint8_t fingerprint_ = 0;
  uint8_t positionTick_ = 0;
  bool initialized_ = false;

  static constexpr uint32_t kPosTimeTickMs = 360000;
  static constexpr uint32_t kTrackerCapSeconds = 3600;
  static constexpr uint32_t kLostAndFoundCapSeconds = 900;

  // Numeric role codes from the generated config.pb.h (confirmed values).
  // Compared numerically so the header never constructs an out-of-range enum.
  static constexpr uint32_t kRoleTracker =
      static_cast<uint32_t>(meshtastic_Config_DeviceConfig_Role_TRACKER);  // 5
  static constexpr uint32_t kRoleTakTracker =
      static_cast<uint32_t>(meshtastic_Config_DeviceConfig_Role_TAK_TRACKER);  // 10
  static constexpr uint32_t kRoleLostAndFound =
      static_cast<uint32_t>(meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND);  // 9

  // Saturating seconds -> milliseconds (uint32).
  static uint32_t saturatingSecondsToMs(uint32_t seconds) {
    if (seconds > UINT32_MAX / 1000u) return UINT32_MAX;
    return seconds * 1000u;
  }
};

}  // namespace traffic
}  // namespace mt
