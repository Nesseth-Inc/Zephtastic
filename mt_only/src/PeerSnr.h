// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace mt {

// SX126x packet-status raw octet: signed two's-complement quarter-dB value.
// Returns the signed value divided by 4.0f (dB). Range: -32.0f..31.75f.
constexpr float packetSnrDb(uint8_t raw) noexcept {
    return static_cast<float>(static_cast<int32_t>(raw) - 256 * (static_cast<int32_t>(raw) > 127 ? 1 : 0)) / 4.0f;
}

}  // namespace mt
