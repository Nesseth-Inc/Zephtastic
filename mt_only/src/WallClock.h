// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <errno.h>

namespace mt {
// Local UTC for displays, packet timestamps and calendar-based accessory maintenance.
// The owner serializes access. Radio and retry deadlines use monotonic time.
class WallClock {
  uint64_t anchorMs_=0;
  uint32_t anchorUtc_=0;
public:
  static constexpr uint64_t CorrectionMs=30u*60u*1000u;
  uint32_t at(uint64_t monotonicMs)const {
    if(!anchorUtc_||monotonicMs<anchorMs_)return 0;
    const uint64_t seconds=(monotonicMs-anchorMs_)/1000u;
    if(seconds>uint64_t(UINT32_MAX-anchorUtc_))return 0;
    return anchorUtc_+uint32_t(seconds);
  }
  int set(uint32_t seconds,uint64_t monotonicMs) {
    if(!seconds)return -EINVAL; // Zero is reserved for unknown on this endpoint.
    if(anchorUtc_) {
      if(monotonicMs<anchorMs_)return -EINVAL;
      if(at(monotonicMs)==seconds)return 0; // Do not extend the correction guard.
      if(monotonicMs-anchorMs_<CorrectionMs)return -EAGAIN;
    }
    anchorUtc_=seconds;anchorMs_=monotonicMs;return 0;
  }
};
}
