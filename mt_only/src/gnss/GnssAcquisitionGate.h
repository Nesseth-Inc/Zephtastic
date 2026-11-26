// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 ZephCore contributors
//
// Three-consecutive-good-fix acquisition gate (donor GPS_GOOD_FIX_COUNT),
// aligned with the selected GnssPromotionEvent.h candidate rules.
// Pure state: one saturating counter. No allocation, no locks, no clock,
// no OS/driver/RF/NVS, no logging. Caller holds the donor GPS mutex;
// the main-thread taker consumes the immutable snapshot via takePromotion().

#pragma once

#include "GnssPromotionEvent.h"

namespace zt::gnss {

// Gate between raw fix callbacks and the single-slot promotion handoff.
// State: the consecutive good-fix count, saturating at kGoodFixCount.
class AcquisitionGate {
public:
    uint8_t goodFixCount() const { return _goodFixCount; }

    // Resets only the consecutive count; the pending slot is untouched.
    void reset() { _goodFixCount = 0; }

    // One fix callback, under the caller-held donor GPS mutex.
    //  - disabled: count=0 and the pending slot is fully cleared (all
    //    six event fields + pending); returns false.
    //  - invalid candidate (fails any selected-header rule): count=0,
    //    the pending slot is untouched; returns false.
    //  - good candidate: the count saturates at 3; the first two good
    //    events never publish. At 3, publishPromotion() is called and
    //    true is returned only if a new event was published. A full
    //    slot is never overwritten and the count stays at 3, so the
    //    next good callback after the caller takes the slot publishes.
    bool consider(const PromotionEvent& e, bool enabled, PendingPromotion& slot) {
        if (!enabled) {
            _goodFixCount = 0;
            slot.event = PromotionEvent{};
            slot.pending = false;
            return false;
        }
        if (!isGoodCandidate(e)) {
            _goodFixCount = 0;
            return false;
        }
        if (_goodFixCount < kGoodFixCount) ++_goodFixCount;
        if (_goodFixCount != kGoodFixCount) return false;
        return publishPromotion(slot, e);
    }

private:
    static constexpr uint8_t kGoodFixCount = 3;  // donor GPS_GOOD_FIX_COUNT

    // "Good" = passes the selected header's complete candidate rules
    // (valid, uptime >= 0, satellites >= 4, inclusive geo bounds, not
    // (0,0)): reuse publishPromotion() on an empty scratch slot so the
    // gate's notion of good can never drift from the header's rules.
    static bool isGoodCandidate(const PromotionEvent& e) {
        PendingPromotion scratch{};
        return publishPromotion(scratch, e);
    }

    uint8_t _goodFixCount = 0;
};

}  // namespace zt::gnss
