// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../stock/modules/TrafficManagementModule.h"

namespace mt::traffic {
struct UnknownPacketWindow {
    struct State { uint8_t count = 0; uint8_t tick = 0; };
    static bool drop(State &state, uint32_t nowMs, uint32_t threshold) {
        return stock::TrafficManagementModule::shouldDropUnknown(state.count, state.tick,
            static_cast<uint8_t>((nowMs / 60000) & 0x0f), threshold);
    }
    static void reset(State &state) { state = {}; }
};
}
