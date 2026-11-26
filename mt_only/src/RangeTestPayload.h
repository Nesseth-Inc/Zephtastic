// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "stock/modules/RangeTestModule.h"

namespace mt {
struct RangeTestPayload {
    static bool encode(uint32_t sequence, uint8_t *out, size_t capacity, size_t &size) {
        size = 0;
        return stock::RangeTestModuleRadio::sendPayload(sequence, [&](const uint8_t *payload, size_t length) {
            if (!out || length > capacity) return false;
            memcpy(out, payload, length);
            size = length;
            return true;
        });
    }
};
}
