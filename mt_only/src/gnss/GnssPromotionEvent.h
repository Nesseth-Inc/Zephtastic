// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 ZephCore contributors
//
// Pure value/handoff adapter for promoting a GNSS fix to a target app.
// Fixed-size value types only: no allocation, no locks, no OS APIs,
// no callbacks, no logging.
//
// Caller contract (mirrors the donor ZephyrGPSManager.cpp):
//  - The caller must hold the donor GPS mutex around publishPromotion()
//    and takePromotion(); this file adds no internal mutex.
//  - Publish only from the validated producer callback, under its GPS mutex,
//    after the three-consecutive-good-fix promotion gate
//    (gps_post_fix_locked equivalent). Never publish raw current_pos:
//    the raw getter becomes valid on an early fix and retains data
//    across failure/disable, so it is not promotion or freshness evidence.
//  - The consumer copies the immutable snapshot (lat/lon/uptime plus
//    altitude/satellites) outside the mutex and publishes the copied
//    fields to the application; the system-workqueue callback never invokes
//    application, persistence or radio work directly.
//
// Adapter primitive only: no driver integration, no freshness policy.

#pragma once

#include <stdint.h>

namespace zt::gnss {

// Immutable snapshot of one promoted fix. Coordinates in nanodegrees
// (1e-9 degrees), altitude in millimetres, uptime in milliseconds.
struct PromotionEvent {
    int64_t latitudeNdeg, longitudeNdeg, capturedUptimeMs;
    int32_t altitudeMm;
    uint16_t satellites;
    bool valid;
    // Parsed matched GGA/RMC measurements travel with this exact fix.
    // Defaults preserve existing synthetic/unmeasured producers' absence.
    uint32_t speedMmPerSecond=0,utcSeconds=0;
    int32_t geoidMm=0;
    bool measured=false,hasUtc=false;
    uint16_t hdopMilli=0; // Native milli-DOP, in the existing tail padding.
    // Zephyr navigation_data::bearing is milli-degrees. Stored here as
    // Meshtastic Position ground_track units (degrees * 1e5).
    uint32_t groundTrackE5=0;
    bool hasGroundTrack=false;
};

// Single-slot handoff between the producer (consumer-path publisher) and
// the main-thread taker. Guarded by the caller-held donor GPS mutex.
struct PendingPromotion {
    PromotionEvent event;
    bool pending;
};

namespace detail {
constexpr int64_t kMinLatitudeNdeg = -90000000000LL;  // -90e9 ndeg
constexpr int64_t kMaxLatitudeNdeg = 90000000000LL;   // +90e9 ndeg
constexpr int64_t kMinLongitudeNdeg = -180000000000LL; // -180e9 ndeg
constexpr int64_t kMaxLongitudeNdeg = 180000000000LL; // +180e9 ndeg
constexpr int32_t kMinSatellites = 4;                  // donor GPS_MIN_SATELLITES
}  // namespace detail

// Accept only a valid candidate: captured uptime >= 0, satellites >= 4,
// latitude within [-90e9, +90e9] ndeg, longitude within [-180e9, +180e9]
// ndeg, and not (0,0) (donor policy). An invalid candidate returns false
// and leaves any pending event unchanged. Refuses to overwrite an
// already-pending event. On success, copies all fields and marks pending.
inline bool publishPromotion(PendingPromotion& slot, const PromotionEvent& candidate) {
    if (!candidate.valid) return false;
    if (candidate.capturedUptimeMs < 0) return false;
    if (candidate.satellites < detail::kMinSatellites) return false;
    if (candidate.latitudeNdeg < detail::kMinLatitudeNdeg ||
        candidate.latitudeNdeg > detail::kMaxLatitudeNdeg) return false;
    if (candidate.longitudeNdeg < detail::kMinLongitudeNdeg ||
        candidate.longitudeNdeg > detail::kMaxLongitudeNdeg) return false;
    if (candidate.latitudeNdeg == 0 && candidate.longitudeNdeg == 0) return false;
    if (slot.pending) return false;
    slot.event = candidate;
    slot.pending = true;
    return true;
}

// Take the exact immutable snapshot. On an empty slot, clears `out` to
// PromotionEvent{} and returns false; otherwise copies the stored event,
// clears it with PromotionEvent{}, clears pending, and returns true.
inline bool takePromotion(PendingPromotion& slot, PromotionEvent& out) {
    if (!slot.pending) {
        out = PromotionEvent{};
        return false;
    }
    out = slot.event;
    slot.event = PromotionEvent{};
    slot.pending = false;
    return true;
}

}  // namespace zt::gnss
