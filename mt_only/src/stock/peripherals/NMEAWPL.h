// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../../serial/PositionOutput.h"

namespace mt::stock::nmea {
size_t printWPL(char *buf, size_t capacity, const mt_serial::Position &pos, const char *name,
                bool calTopo, bool localListing);
size_t printGGA(char *buf, size_t capacity, const mt_serial::Position &pos);
}
