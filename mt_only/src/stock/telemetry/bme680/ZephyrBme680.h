// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
namespace zt::telemetry {struct EnvironmentReadings;}
namespace mt::stock::telemetry::bme680 {
void enable(bool enabled);
void clock(uint32_t utc);
void merge(zt::telemetry::EnvironmentReadings &out,uint64_t now);
int retire();
}
