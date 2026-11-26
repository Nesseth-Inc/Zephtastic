// SPDX-License-Identifier: MIT
#pragma once

#include "MqttAtomic.h"

namespace mt::mqtt {

// Tracks whether the platform realtime clock has been set from a plausible
// Unix timestamp. Invalid inputs do not disturb an already-valid clock state.
class CertificateClock {
 public:
  using SetRealtime = int (*)(uint32_t unixSeconds);
  static constexpr uint32_t kEarliestPlausibleUnixSeconds = 1577836800u;  // 2020-01-01 UTC

  bool sync(uint32_t unixSeconds, SetRealtime setRealtime) {
    if (unixSeconds < kEarliestPlausibleUnixSeconds || setRealtime == nullptr) return false;
    const int result = setRealtime(unixSeconds);
    ready_.store(result == 0, AtomicOrder::Release);
    return result == 0;
  }

  bool ready() const { return ready_.load(AtomicOrder::Acquire); }

 private:
  Atomic<bool> ready_{false};
};

}  // namespace mt::mqtt
