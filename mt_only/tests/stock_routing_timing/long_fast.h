// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// SX126x, sub-GHz LONG_FAST (SF11, 250 kHz), +3 dB RX SNR, zero jitter.
// The pinned stock oracle recomputes these values from RadioInterface.cpp.
namespace stock_long_fast {
constexpr uint32_t slotMs = 44;
constexpr uint32_t initialMs = 704;
constexpr uint32_t duplicateMs = 3520;
constexpr uint32_t weakDuplicateMs = 1056; // Original frame received at -20 dB.
}
