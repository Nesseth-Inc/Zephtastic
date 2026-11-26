// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
namespace mt::pki {
// Contact capacity is independent of immutable v2 baseline wire layout.
inline constexpr size_t ContactLimit=16;
inline constexpr size_t RuntimeSlots=4;
}
