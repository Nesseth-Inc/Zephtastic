// tx_window.h
//
// Offline C++17 prototype: parent integration of the Qwen TX window draft.
//
// Mirrors the pinned stock review (commit 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c,
// all 8 files sha256-verified against review-next-telemetry/SOURCE-PIN.json):
//   - AirTime::Windows::syncNow / logAirtime(TX_LOG) / utilizationTXPercent()
//     in src/airtime.cpp (lines 65-137, 35-53, 215-226)
//   - test_tx_utilization_* vectors in test/test_airtime/test_main.cpp
//     (lines 942-1026)
//
// Semantics (stock is second-granular; the prototype is ms-granular):
//   * 60 x uint32 slots, slot index = (uptime_ms / 60000) % 60. The index
//     is absolute uptime phase, never an age.
//   * Every public entry first syncs to the given monotonic uptime: for
//     each minute (oldMin, nowMin] crossed since the last sync, slot
//     (oldMin + i) % 60 is cleared. A gap of >= 60 minutes clears the
//     whole ring (bounded: at most 60 clears). Rotation is absolute-phase
//     based, so poll rate (1 ms reads vs one big jump) and TX availability
//     do not change the result; idle reads age the window.
//   * record() credits the WHOLE estimated duration to the current
//     minute's slot (whole-packet attribution; a slot may hold more than
//     60000 ms - no per-bucket clamp that would lose a legitimate
//     cross-boundary completed TX).
//   * percentAt() sums all 60 slots and returns
//     float(sum) / float(3600000) * 100, bounded to 0..100. When a minute
//     boundary is crossed, sync clears the current minute's slot, so a
//     read at a new minute covers the 59 most recently completed minutes;
//     the stock minute-quantization defect is preserved, not fixed. The
//     denominator is the fixed MS_IN_HOUR (3600000 ms). The 600000 ms
//     window / 6000 ms budget in Service.h is a separate RF gate and is
//     NOT part of this window.
//
// Clock: explicit uint64_t monotonic uptime milliseconds. A future
// integration must supply a real 64-bit clock; casting the legacy uint32
// Service `now` is NOT acceptable (49.7-day wrap). This prototype does
// not wire that provider.
//
// Intentional, documented divergences from stock:
//   1. Backward clock. Stock is second-granular: a backward jump within
//      the same minute is a silent no-op (elapsed == 0); a backward jump
//      crossing a minute boundary underflows its uint32 elapsed to a huge
//      value and memsets the whole ring. The prototype rejects ANY
//      backward jump: no state change, lastError() == BackwardClock, and
//      percentAt()/windowSumMs() are unavailable until a forward call.
//      A wrapped uint32 ms value is therefore a backward clock, never a
//      small forward delta.
//   2. Overflow. Stock's unchecked uint32 add/sum wraps. The prototype
//      checks: a record that would overflow its slot is rejected
//      (lastError() == Overflow, no state change); the combined sum is
//      computed in uint64 and cannot overflow (60 * (2^32-1) * 100 <
//      2^64).
//   3. Domain bound. Stock may exceed 100 on over-counted inputs. This
//      prototype retains fractional float values in the physical domain
//      and bounds out-of-domain values to 100, plus an exact sum for tests.
//   4. Availability. Stock reports 0.0 before the first log. The
//      prototype reports "unavailable" (nullopt) until the first
//      successful record, so a real zero (recorded, then aged out) is
//      distinguishable from no data.
//
// Constraints: header-only, no heap, no NVS, no Zephyr, no libmath.
// RAM: 60 x uint32 slots, uint64 anchor, error enum and two flags.
// Member order avoids the original draft's extra alignment padding.
#pragma once

#include <cstddef>
#include <cstdint>
#include "Observed.h"

namespace zt {

class TxMinuteWindow {
 public:
  static constexpr std::uint64_t kBucketMs = 60000;
  static constexpr std::uint64_t kDenominatorMs = 3600000;  // MS_IN_HOUR
  static constexpr std::uint32_t kSlots = 60;
  enum class Error { None, BackwardClock, Overflow };
  static constexpr std::size_t staticRamBytes =
      kSlots * sizeof(std::uint32_t) + sizeof(std::uint64_t) + sizeof(Error) + 2;

  bool anchored() const { return anchored_; }
  bool everRecorded() const { return everRecorded_; }
  Error lastError() const { return lastError_; }
  std::uint64_t anchorMs() const { return anchor_; }

 private:
  std::uint32_t slots_[kSlots] = {0};
  std::uint64_t anchor_ = 0;
  Error lastError_ = Error::None;
  bool anchored_ = false;
  bool everRecorded_ = false;

  Error sync(std::uint64_t nowMs) {
    if (!anchored_) {
      anchored_ = true;
      anchor_ = nowMs;
      return Error::None;
    }
    if (nowMs < anchor_) {
      lastError_ = Error::BackwardClock;
      return lastError_;  // stock: same-minute no-op or underflow wipe
    }
    const std::uint64_t oldMin = anchor_ / kBucketMs;
    const std::uint64_t nowMin = nowMs / kBucketMs;
    const std::uint64_t gap = nowMin - oldMin;
    if (gap >= kSlots) {
      for (std::uint32_t i = 0; i < kSlots; ++i)
        slots_[i] = 0;
    } else {
      for (std::uint64_t i = 1; i <= gap; ++i)
        slots_[(oldMin + i) % kSlots] = 0;
    }
    anchor_ = nowMs;
    return Error::None;
  }

  bool readSum(std::uint64_t nowMs, std::uint64_t *out) {
    if (sync(nowMs) != Error::None)
      return false;
    lastError_ = Error::None;
    if (!everRecorded_)
      return false;
    std::uint64_t sum = 0;
    for (std::uint32_t i = 0; i < kSlots; ++i)
      sum += slots_[i];  // max 60*(2^32-1) < 2^64: exact
    *out = sum;
    return true;
  }

 public:
  // Credit an estimated TX duration (ms) that completed at nowMs.
  // The whole duration goes to the current minute's slot (no clamp).
  Error record(std::uint64_t nowMs, std::uint32_t durationMs) {
    if (anchored_ && nowMs < anchor_) {
      lastError_ = Error::BackwardClock;
      return lastError_;
    }
    const std::uint32_t slot =
        static_cast<std::uint32_t>((nowMs / kBucketMs) % kSlots);
    // Crossing any minute clears the destination slot. Only a same-minute
    // addition can overflow; reject it before rotating or committing time.
    const bool sameMinute = anchored_ && nowMs/kBucketMs==anchor_/kBucketMs;
    if (sameMinute && slots_[slot] >
        UINT32_MAX - durationMs) {
      lastError_ = Error::Overflow;
      return lastError_;
    }
    if (sync(nowMs) != Error::None)
      return lastError_;
    slots_[slot] += durationMs;
    everRecorded_ = true;
    lastError_ = Error::None;
    return Error::None;
  }

  // Age the window to nowMs without a TX (idle reads still age it).
  Error advance(std::uint64_t nowMs) {
    const Error e = sync(nowMs);
    if (e == Error::None)
      lastError_ = Error::None;
    return e;
  }

  // Stock float arithmetic in the physical domain; bounded to 0..100.
  // nullopt when
  // unavailable: no successful record yet, or the call was rejected.
  zt::Observed<float> percentAt(std::uint64_t nowMs) {
    std::uint64_t sum = 0;
    if (!readSum(nowMs, &sum))
      return {};
    const float pct = static_cast<float>(sum) /
                      static_cast<float>(kDenominatorMs) * 100.0f;
    return pct > 100.0f ? 100.0f : pct;
  }

  // Exact unclamped window sum (ms), for verification. nullopt as above.
  zt::Observed<std::uint64_t> windowSumMs(std::uint64_t nowMs) {
    std::uint64_t sum = 0;
    if (!readSum(nowMs, &sum))
      return {};
    return sum;
  }
};

static_assert(60ULL * UINT32_MAX * 100ULL <
                  UINT64_MAX,
              "combined sum * 100 cannot overflow uint64");
static_assert(TxMinuteWindow::staticRamBytes <= 256,
              "logical payload includes the error enum");
static_assert(sizeof(TxMinuteWindow) <= 256,"window storage must stay bounded");

}  // namespace zt
