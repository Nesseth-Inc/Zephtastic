// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "EnvironmentMetricsAdapter.h"
#include <stdint.h>

namespace zt::telemetry {

// Snapshot of the latest published environment readings.
// References the adapter's EnvironmentReadings type; does not redefine
// zt::Observed or EnvironmentReadings.
class EnvironmentSnapshot {
 public:
  // Copies values and time, marks known.
  void publish(const EnvironmentReadings &readings, uint64_t at) {
    captured_ = readings;
    at_ = at;
    known_ = true;
  }

  // Clears fields, time, and known.
  void invalidate() {
    captured_ = EnvironmentReadings{};
    at_ = 0;
    known_ = false;
  }

  // Initializes out empty. Rejects unknown, future (now < at_, checked
  // before any unsigned subtraction), and stale (now - at_ > maxAge).
  // Otherwise copies the exact reading and returns true. Preserves each
  // reading's individual valid flag and zero values; never infers all
  // three readings as valid.
  bool snapshot(uint64_t now, uint64_t maxAge, EnvironmentReadings &out) const {
    out = EnvironmentReadings{};
    if (!known_) {
      return false;
    }
    if (now < at_) {
      return false;
    }
    if (now - at_ > maxAge) {
      return false;
    }
    out = captured_;
    return true;
  }

 private:
  EnvironmentReadings captured_{};
  uint64_t at_ = 0;
  bool known_ = false;
};

}  // namespace zt::telemetry
