// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "EnvironmentMetricsAdapter.h"
#include "power/PowerReadings.h"
#include <stdint.h>
namespace mt::environment {
// Shared main-thread producer; donor initialization and per-device fetch occur
// once for both independently enabled caches, outside the service mutex.
#if defined(CONFIG_MT_ENVIRONMENT_SENSORS)
void poll(bool environmentEnabled,bool powerEnabled,uint64_t now);
zt::telemetry::EnvironmentReadings snapshot(uint64_t now);
zt::telemetry::PowerReadings powerSnapshot(uint64_t now);
#else
inline void poll(bool,bool,uint64_t){}
inline zt::telemetry::EnvironmentReadings snapshot(uint64_t){return {};}
inline zt::telemetry::PowerReadings powerSnapshot(uint64_t){return {};}
#endif
inline void poll(bool enabled,uint64_t now){poll(enabled,false,now);}
}
