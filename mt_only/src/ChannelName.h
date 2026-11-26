// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>

namespace mt {
namespace ChannelName {

inline bool hash(const char* name, const uint8_t* key, size_t keyLen, uint8_t& out) {
    if (name == nullptr || key == nullptr) return false;
    if (keyLen != 16 && keyLen != 32) return false;
    size_t len = 0;
    while (len < 11) {
        const uint8_t c = static_cast<uint8_t>(name[len]);
        if (c == 0) break;
        if (c < 0x20 || c > 0x7e) return false;
        ++len;
    }
    if (len == 0 || (len == 11 && name[11] != 0)) return false;
    uint8_t h = 0;
    for (size_t i = 0; i < len; ++i) h ^= static_cast<uint8_t>(name[i]);
    for (size_t i = 0; i < keyLen; ++i) h ^= key[i];
    out = h;
    return true;
}

// Stock licensed plaintext channels have an empty effective PSK, so their
// canonical channel hash is XOR(name bytes) with no key contribution.
// Zero is valid for both explicit modes; neither hash alone selects a mode.
inline bool licensedPlaintextHash(const char *name, uint8_t &out) {
    if (!name) return false;
    size_t len = 0;
    while (len < 11) {
        const uint8_t c = static_cast<uint8_t>(name[len]);
        if (c == 0) break;
        if (c < 0x20 || c > 0x7e) return false;
        ++len;
    }
    if (!len || (len == 11 && name[11] != 0)) return false;
    uint8_t h = 0;
    for (size_t i = 0; i < len; ++i) h ^= static_cast<uint8_t>(name[i]);
    out = h;
    return true;
}

}  // namespace ChannelName
}  // namespace mt
