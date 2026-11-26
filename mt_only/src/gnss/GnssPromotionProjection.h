// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 ZephCore contributors
//
// Pure projection adapter: projects an immutable PromotionEvent snapshot
// (copied under the donor GPS mutex) into a meshtastic_Position.
// No allocation, no locks, no OS APIs, no I/O, no queue mutation,
// no fixedPosition, no public reply, no persistence.
//
// Caller contract: the caller holds the donor GPS mutex while copying
// the immutable event, unlocks, and calls projectPosition() on the
// main thread with the current uptime.

#pragma once

#include "GnssPromotionEvent.h"
#include "meshtastic/mesh.pb.h"

#include <cstdint>

namespace zt::gnss {

// Projects a copied PromotionEvent into a meshtastic_Position.
// Returns false and leaves out = meshtastic_Position{} when:
//   - event.valid is false
//   - capturedUptimeMs < 0 or nowUptimeMs < 0
//   - capturedUptimeMs > nowUptimeMs (future-dated)
//   - nowUptimeMs - capturedUptimeMs > maxAgeMs (stale; age == max OK)
//   - satellites < 4 (detail::kMinSatellites)
//   - latitude/longitude outside inclusive ±90e9 / ±180e9 ndeg
//   - (0,0)
// On success sets has_latitude_i/has_longitude_i and latitude_i/
// longitude_i (ndeg/100, i.e. 1e-7 degree units, signed truncation
// toward zero, checked to fit int32_t) and location_source =
// LOC_INTERNAL, and returns true. time and altitude are left unset:
// the event has no altitude-presence bit, so altitudeMm must not be
// projected as if valid.
inline bool projectPosition(const PromotionEvent& ev, int64_t nowUptimeMs,
                            int64_t maxAgeMs, meshtastic_Position& out) {
    out = meshtastic_Position{};
    if (!ev.valid) return false;
    if (ev.capturedUptimeMs < 0 || nowUptimeMs < 0) return false;
    if (ev.capturedUptimeMs > nowUptimeMs) return false;  // future-dated
    if (nowUptimeMs - ev.capturedUptimeMs > maxAgeMs) return false;  // stale
    if (ev.satellites < detail::kMinSatellites) return false;
    if (ev.latitudeNdeg < detail::kMinLatitudeNdeg ||
        ev.latitudeNdeg > detail::kMaxLatitudeNdeg) return false;
    if (ev.longitudeNdeg < detail::kMinLongitudeNdeg ||
        ev.longitudeNdeg > detail::kMaxLongitudeNdeg) return false;
    if (ev.latitudeNdeg == 0 && ev.longitudeNdeg == 0) return false;

    // ndeg (1e-9 deg) -> E7 (1e-7 deg): divide by 100. Signed integer
    // division truncates toward zero. The inclusive ndeg bounds (±90e9,
    // ±1.8e11) map to ±9e8 and ±1.8e9, both within int32_t; verify
    // anyway (checked conversion).
    const int64_t latE7 = ev.latitudeNdeg / 100;
    const int64_t lonE7 = ev.longitudeNdeg / 100;
    if (latE7 < INT32_MIN ||
        latE7 > INT32_MAX ||
        lonE7 < INT32_MIN ||
        lonE7 > INT32_MAX)
        return false;

    out.has_latitude_i = true;
    out.latitude_i = static_cast<int32_t>(latE7);
    out.has_longitude_i = true;
    out.longitude_i = static_cast<int32_t>(lonE7);
    out.location_source = meshtastic_Position_LocSource_LOC_INTERNAL;
    return true;
}

}  // namespace zt::gnss
