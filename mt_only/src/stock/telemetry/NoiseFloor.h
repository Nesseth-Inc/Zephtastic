// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

namespace mt::stock::telemetry {
// RadioLibInterface::updateNoiseFloor and getAverageNoiseFloorInternal,
// pinned Meshtastic 8e6a88d. Samples are integer dBm in [-127,-1].
class NoiseFloor {
 public:
  template<class Idle, class Read> void poll(uint32_t now, Idle idle, Read read) {
    if (now - last_ < 5000 || !idle()) return;
    last_ = now;
    const int16_t value = read();
    if (value >= 0 || value < -127) return;
    samples_[next_] = static_cast<int8_t>(value);
    if (++next_ == Capacity) next_ = 0;
    if (count_ < Capacity) ++count_;
  }
  int8_t measured() const {
    int sum = 0;
    for (unsigned i = 0; i < count_; ++i) sum += samples_[i];
    return count_ ? static_cast<int8_t>(sum / count_) : 0;
  }
 private:
  static constexpr unsigned Capacity = 20;
  uint32_t last_ = 0;
  int8_t samples_[Capacity]{};
  uint8_t next_ = 0, count_ = 0;
};
}
