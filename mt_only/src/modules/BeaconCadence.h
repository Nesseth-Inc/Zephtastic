// SPDX-License-Identifier: GPL-3.0-only
#ifndef MT_BEACON_BEACON_CADENCE_H_
#define MT_BEACON_BEACON_CADENCE_H_
#include <stdint.h>
namespace mt { namespace beacon {
class BeaconCadence {
 public:
  static uint64_t intervalMs(uint32_t rawSeconds) {
    const uint64_t seconds = rawSeconds < 3600u ? 3600u : rawSeconds;
    return seconds * 1000ULL;
  }
  bool due(uint64_t uptimeMs, uint32_t utcSeconds, bool sentBefore,
           uint32_t lastSentUtc, uint32_t rawSeconds) const {
    if (uptimeMs == UINT64_MAX) return false;
    if (attempted_) {
      if (uptimeMs < lastAttemptMs_) return false;
      if (uptimeMs - lastAttemptMs_ < intervalMs(rawSeconds)) return false;
    }
    if (!sentBefore) return true;
    if (utcSeconds != 0u && lastSentUtc != 0u && utcSeconds >= lastSentUtc) {
      const uint64_t delta = static_cast<uint64_t>(utcSeconds) - lastSentUtc;
      const uint64_t seconds = rawSeconds < 3600u ? 3600u : rawSeconds;
      return delta >= seconds;
    }
    return uptimeMs >= intervalMs(rawSeconds);
  }
  bool attempted(uint64_t uptimeMs) {
    if (uptimeMs == UINT64_MAX || (attempted_ && uptimeMs < lastAttemptMs_))
      return false;
    attempted_ = true; lastAttemptMs_ = uptimeMs; return true;
  }
 private:
  bool attempted_ = false;
  uint64_t lastAttemptMs_ = 0;
};
}}
#endif
