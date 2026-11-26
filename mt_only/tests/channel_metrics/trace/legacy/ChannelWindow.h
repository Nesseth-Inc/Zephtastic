// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ChannelBuckets.h"
#include "ChannelQuality.h"

namespace zt {
// Same six absolute ten-second buckets and fixed minute denominator as stock.
// Records use owner processing time; uncertainty suppresses a whole minute,
// while known credits continue to be retained. Reads never create idle proof.
class ChannelWindow {
 public:
  bool record(uint64_t now,uint32_t amount) {
    if(!quality_.observe(now))return false;
    return buckets_.record(now,amount);
  }
  void invalidate(uint64_t now){quality_.invalidate(now);}
  Observed<uint64_t> windowSumMs(uint64_t now) {
    if(!quality_.availableAt(now))return {};
    return buckets_.windowSumMs(now);
  }
  Observed<float> percentAt(uint64_t now) {
    if(!quality_.availableAt(now))return {};
    return buckets_.percentAt(now);
  }
  bool anchored()const{return buckets_.anchored();}
 private:
  ChannelBuckets buckets_;
  ChannelQuality quality_;
};
}
