// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <meshtastic/admin.pb.h>
struct device;
namespace mt::stock::telemetry::scd4x {
int applySync(const meshtastic_SensorConfig &);
int submit(const meshtastic_SensorConfig &cfg,uint32_t session,uint32_t id);
bool take(uint32_t session,uint32_t &id,int &result);
bool lockSample(const device *dev);
void unlockSample(const device *dev);
}
