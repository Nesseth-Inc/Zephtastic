// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <meshtastic/admin.pb.h>
#include <meshtastic/telemetry.pb.h>
#include "RegisterDevices.h"
namespace mt::stock::telemetry::accessory {
int submit(const meshtastic_SensorConfig &,uint32_t session,uint32_t id);
int submitScale(uint32_t kg,uint32_t session,uint32_t id);
bool pending();
bool take(uint32_t session,uint32_t &id,int &result);
#if defined(CONFIG_MT_MLX90614)
bool mlxAvailable();
void enableMlx(bool);
bool objectTemperature(uint64_t now, meshtastic_HealthMetrics &);
#endif
bool airAvailable();
void clock(uint32_t utc);
void enableAir(bool enabled,uint32_t interval);
#if defined(CONFIG_MT_PMSA003I)
void airPollDeadline(uint64_t deadline);
#endif
bool air(uint64_t now,meshtastic_AirQualityMetrics &out);
#if defined(CONFIG_MT_LARK_WEATHER)
void requestWeather();
bool dropzoneWeather(Environment &);
#endif
#if defined(CONFIG_MT_STOCK_SHT)
int selectShtAccuracy(unsigned level);
#endif
void enableEnvironment(bool enabled);
Environment environment(uint64_t now);
}
