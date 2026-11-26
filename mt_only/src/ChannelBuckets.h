// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include "Observed.h"

namespace zt {

// Six fixed 10000 ms absolute-phase buckets for channel-usage accounting.
// The caller supplies a known non-decreasing uint64 monotonic millisecond
// clock; clock quality is a separate parent concern. No heap, no standard
// optional, no strings, no libm, no logging, no allocation, no persistence.
class ChannelBuckets {
 public:
  // Store amountMs in the bucket containing nowMs; the whole event belongs to
  // the processing-time bucket (no backdated split). nowMs == UINT64_MAX is
  // rejected. record(now, 0) anchors a known idle ring; reads never anchor.
  // Returns false if the target bucket is overflow-flagged or the addition
  // would overflow uint32; true on stored credit.
  bool record(uint64_t nowMs, uint32_t amountMs) {
    if (nowMs == UINT64_MAX) return false;
    const uint64_t ordinal = nowMs / 10000;
    age(ordinal);
    anchored_ = true;
    Slot &s = slots_[ordinal % 6];
    if (!s.valid) {
      s.amount = amountMs;
      s.valid = true;
      s.overflow = false;
      return true;
    }
    if (s.overflow) return false;
    if (amountMs > UINT32_MAX - s.amount) {
      s.overflow = true;
      return false;
    }
    s.amount += amountMs;
    return true;
  }

  // Aged sum of the six buckets at nowMs. Absent before the first record or
  // when the aggregate exceeds UINT32_MAX; a fully-aged or idle anchored ring
  // yields a present zero.
  Observed<uint64_t> windowSumMs(uint64_t nowMs) {
    if (nowMs == UINT64_MAX) return Observed<uint64_t>();
    age(nowMs / 10000);
    if (!anchored_) return Observed<uint64_t>();
    const uint64_t sum = rawSum();
    return sum > UINT32_MAX ? Observed<uint64_t>() : Observed<uint64_t>(sum);
  }

  // float(sum)/float(60000)*100 for the same representable sum; no clamp/EMA.
  Observed<float> percentAt(uint64_t nowMs) {
    if (nowMs == UINT64_MAX) return Observed<float>();
    age(nowMs / 10000);
    if (!anchored_) return Observed<float>();
    const uint64_t sum = rawSum();
    return sum > UINT32_MAX ? Observed<float>() : Observed<float>(float(sum) / float(60000) * 100);
  }

  Observed<float> smoothedPercentAt(uint64_t nowMs) {
    auto raw=percentAt(nowMs);
    return raw && hasAverage_ ? Observed<float>(average_) : raw;
  }

  // True once any record has observed the ring; reads never anchor.
  bool anchored() const { return anchored_; }

 private:
  struct Slot {
    uint32_t amount = 0;
    bool valid = false;
    bool overflow = false;
  };
  // AirTime::Windows::foldChannelUtil, pinned Meshtastic 8e6a88d. On
  // crossing a bucket boundary, fold the previous minute before pruning.
  void fold(float sample,uint64_t steps) {
    if(!steps)return;
    if(!hasAverage_){average_=sample;hasAverage_=true;--steps;}
    if(!steps)return;
    float base=1.0f-1.0f/128.0f, retained=1.0f;
    for(;steps;steps>>=1) { if(steps&1u)retained*=base; base*=base; }
    average_=sample+(average_-sample)*retained;
  }
  void age(uint64_t ordinal) {
    if(!anchored_){lastOrdinal_=ordinal;return;}
    const uint64_t elapsed=ordinal-lastOrdinal_;
    const unsigned crossed=elapsed>6 ? 6 : unsigned(elapsed);
    for(unsigned i=1;i<=crossed;++i) {
      const uint64_t sum=rawSum();
      if(sum<=UINT32_MAX)fold(float(sum)/60000.0f*100,1);
      else hasAverage_=false;
      auto &expired=slots_[(lastOrdinal_+i)%6];
      expired.valid=expired.overflow=false;
    }
    fold(0.0f,elapsed-crossed);
    lastOrdinal_=ordinal;
  }
  // Sum of valid, non-overflow buckets (overflow buckets are unavailable).
  uint64_t rawSum() const {
    uint64_t sum = 0;
    for (int i = 0; i < 6; ++i) {
      if (slots_[i].valid && slots_[i].overflow) return UINT64_MAX;
      if (slots_[i].valid) sum += slots_[i].amount;
    }
    return sum;
  }
  Slot slots_[6];
  uint64_t lastOrdinal_=0;
  float average_=0;
  bool hasAverage_=false,anchored_=false;
};

}  // namespace zt
