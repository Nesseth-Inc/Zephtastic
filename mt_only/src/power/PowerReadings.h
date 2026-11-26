// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Observed.h"
namespace zt::telemetry {
// Independent measured channels, in wire volts and milliamperes.
struct PowerReadings { Observed<float> voltage[8]{}, current[8]{}; };
}
