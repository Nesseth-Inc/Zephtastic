// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ChannelBuckets.h"
#include "ChannelQuality.h"

namespace zt {
// Same six absolute ten-second buckets and fixed minute denominator as stock.
// Owner processing time ages known credits. percentAt keeps coverage quality;
// loggedPercentAt exports the valid-clock logged estimate independently of
// receive uncertainty. An empty logger starts at zero, not proof of RF silence.
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
  Observed<float> loggedPercentAt(uint64_t now) {
    if(!quality_.observe(now))return {};
    if(!buckets_.anchored()){if(!buckets_.record(now,0))return {};}
    return buckets_.percentAt(now);
  }
  Observed<float> smoothedLoggedPercentAt(uint64_t now) {
    auto raw=loggedPercentAt(now);
    return raw ? buckets_.smoothedPercentAt(now) : raw;
  }
  bool anchored()const{return buckets_.anchored();}
 private:
  ChannelBuckets buckets_;
  ChannelQuality quality_;
};
}
