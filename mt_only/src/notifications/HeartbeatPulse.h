// SPDX-License-Identifier: GPL-3.0-only
// HeartbeatPulse.h
// M110 heartbeat LED: pure policy/timing helper.
// Dependency-free C++17 header only: no GPIO, Zephyr, workqueue, storage,
// or threading. The caller owns master disable, the unread snapshot,
// radio-held priority, GPIO, and scheduling; this file only maps
// (mode, unread) to a Plan and provides wrap-safe timing math.
//
// Donor policy (helpers/led_gate.h, helpers/ui/ui_common.c):
//   LEDS_HB_ALL=0, LEDS_HB_HB=1, LEDS_HB_UNREAD=2, LEDS_HB_OFF=3
//   LED_CYCLE_MS=4000, LED_ON_MS=20, LED_ON_MSG_MS=200
//   should_light: OFF -> false; UNREAD -> unread>0; else true
//   pulse_ms: HB -> 20; else 200 if unread>0, else 20
//   on handler schedules off after pulse width; off handler schedules
//   next on after 4000 - width; OFF still keeps cycle timing.

#pragma once

#include <stdint.h>

namespace mt::heartbeat {

enum class Mode : uint8_t { All = 0, Hb = 1, Unread = 2, Off = 3 };

struct Plan {
    bool light;
    uint16_t pulseMs;
    uint16_t nextAfterOffMs;
};

namespace detail {
inline constexpr uint16_t kCycleMs = 4000;      // LED_CYCLE_MS
inline constexpr uint16_t kPulseMs = 20;        // LED_ON_MS
inline constexpr uint16_t kPulseUnreadMs = 200; // LED_ON_MSG_MS
}  // namespace detail

// Maps (mode, unread snapshot) to the donor policy. No mode validation:
// Mode is a closed 4-value set, matching the donor default behavior.
constexpr Plan plan(Mode mode, bool unread) {
    const bool lit =
        mode == Mode::Off ? false
        : mode == Mode::Unread ? unread
        : true;  // All and Hb always light
    const uint16_t pulse =
        mode == Mode::Hb ? detail::kPulseMs
        : (unread ? detail::kPulseUnreadMs : detail::kPulseMs);
    return Plan{lit, pulse, static_cast<uint16_t>(detail::kCycleMs - pulse)};
}

// True iff the modular delta (now - deadline) is < 2^31, i.e. `deadline`
// lies at or before `now` within the past half-wrap. Unsigned modular
// arithmetic only; no signed casts. Well defined for durations < 2^31.
constexpr bool deadlineReached(uint32_t now, uint32_t deadline) {
    return (now - deadline) < (static_cast<uint32_t>(1) << 31);
}

// next = now + delayMs, wrapping modulo 2^32. The caller invokes this
// once from the current `now` (no catch-up loop); documented durations
// are small (< 2^31), so a single rearm is well defined.
constexpr uint32_t rearmFrom(uint32_t now, uint32_t delayMs) {
    return now + delayMs;
}

}  // namespace mt::heartbeat
