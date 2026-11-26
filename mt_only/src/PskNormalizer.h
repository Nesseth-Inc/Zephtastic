// SPDX-License-Identifier: GPL-3.0-only
// Stock stored-key expansion; secondary inheritance is resolved by channel lookup.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt {
struct PskNormalizer {
    static bool normalize(const uint8_t* stored, size_t storedSize,
                          const uint8_t* defaultKey, size_t defaultKeySize,
                          uint8_t* out, size_t outCapacity, size_t& outSize)
    {
        if (out == nullptr || outCapacity < 32 || storedSize > 32 ||
            (storedSize != 0 && stored == nullptr)) {
            return false;
        }

        uint8_t candidate[32] = {};
        size_t candidateSize = 0;
        bool valid = true;
        if (storedSize == 0) {
            candidateSize = 0;
        } else if (storedSize == 1) {
            const uint8_t alias = stored[0];
            if (alias == 0) {
                candidateSize = 0;
            } else if (defaultKey == nullptr || defaultKeySize != 16) {
                valid = false;
            } else {
                memcpy(candidate, defaultKey, 16);
                candidate[15] = static_cast<uint8_t>(candidate[15] + alias - 1u);
                candidateSize = 16;
            }
        } else {
            candidateSize = storedSize <= 16 ? 16 : 32;
            memcpy(candidate, stored, storedSize);
        }

        if (valid) {
            memcpy(out, candidate, sizeof(candidate));
            outSize = candidateSize;
        }
        volatile uint8_t* wipe = candidate;
        for (size_t i = 0; i < sizeof(candidate); ++i) {
            wipe[i] = 0;
        }
        return valid;
    }
};
} // namespace mt
