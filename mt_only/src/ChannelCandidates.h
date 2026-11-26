// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace mt {

struct ChannelCandidates {
    enum : uint8_t {
        DISABLED  = 0,
        PRIMARY   = 1,
        SECONDARY = 2,
    };

    static uint8_t mask(const uint8_t roles[8],
                        const uint8_t hashes[8],
                        uint8_t wanted) {
        if (roles == nullptr || hashes == nullptr) {
            return 0;
        }
        uint8_t m = 0;
        for (int i = 0; i < 8; ++i) {
            const uint8_t role = roles[i];
            if (role == PRIMARY || role == SECONDARY) {
                if (hashes[i] == wanted) {
                    m |= static_cast<uint8_t>(1u << i);
                }
            }
        }
        return m;
    }
};

} // namespace mt

