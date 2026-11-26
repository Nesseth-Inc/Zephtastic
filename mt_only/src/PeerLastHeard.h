// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
namespace mt {
constexpr uint32_t receivedLastHeard(uint32_t previous, uint32_t capturedUtc) {
  return capturedUtc != 0 ? capturedUtc : previous;
}
}  // namespace mt
