// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "stock/position/PositionPrecision.h"

namespace mt::PositionPrecision {
using stock::truncateCoordinate;
inline void apply(meshtastic_Position &position, uint32_t precision) {
    stock::applyPositionPrecision(position, precision);
}
}
