// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace zt {
// Owner-serialized observation time. Bad clocks cannot start a recovery timer;
// recovery starts at the first subsequent trustworthy nondecreasing sample.
class ChannelQuality {
 public:
  bool observe(uint64_t now) {
    if(now==UINT64_MAX||(haveTime_&&now<highWater_)) {
      tainted_=true;pending_=true;return false;
    }
    highWater_=now;haveTime_=true;
    if(pending_){taintedAt_=now;pending_=false;}
    return true;
  }
  void invalidate(uint64_t now) {
    if(observe(now)){tainted_=true;taintedAt_=now;}
  }
  bool availableAt(uint64_t now) {
    if(!observe(now))return false;
    if(tainted_&&now-taintedAt_>=60000)tainted_=false;
    return !tainted_;
  }
 private:
  uint64_t highWater_=0,taintedAt_=0;
  bool haveTime_=false,tainted_=false,pending_=false;
};
}
