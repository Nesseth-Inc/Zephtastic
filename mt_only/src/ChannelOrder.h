// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

namespace mt::ChannelOrder {

// Index plan only (not a full promotion/reorder implementation): swap
// logical slot 0 with `selectedSlot`. On success, logicalToPhysical[i]
// holds the physical slot of logical slot i. selectedSlot in [0,8) ->
// true; otherwise false and the caller's buffer is left unchanged.
inline bool permutation(unsigned selectedSlot, uint8_t (&logicalToPhysical)[8]) {
    if (selectedSlot >= 8) {
        return false;
    }
    uint8_t staged[8];
    for (unsigned i = 0; i < 8; ++i) {
        staged[i] = static_cast<uint8_t>(i);
    }
    staged[0] = static_cast<uint8_t>(selectedSlot);
    staged[selectedSlot] = 0;
    for (unsigned i = 0; i < 8; ++i) {
        logicalToPhysical[i] = staged[i];
    }
    return true;
}

}  // namespace mt::ChannelOrder
