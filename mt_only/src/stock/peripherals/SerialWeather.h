// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, SerialModule::processWXSerial.
#pragma once

#include "../../serial/Ws85Engine.h"
#include "meshtastic/telemetry.pb.h"
#include <math.h>

namespace mt::stock::serial {
constexpr double Pi = 3.1415926535897932384626433832795;

inline void addDirection(double degrees, double &sine, double &cosine)
{
    const double radians = degrees * Pi / 180;
    sine += sin(radians);
    cosine += cos(radians);
}

inline void averageWind(mt_serial::WeatherReport &report, float velocitySum, uint32_t velocityCount,
                        double directionSin, double directionCos, uint32_t directionCount)
{
    report.wind_speed = 1.0 * velocitySum / velocityCount;
    const double avgRadians = atan2(directionSin / directionCount, directionCos / directionCount);
    float dirAvg = avgRadians * 180 / Pi;
    if (dirAvg < 0)
        dirAvg += 360.0;
    report.wind_direction = dirAvg;
}

inline void weatherTelemetry(const mt_serial::WeatherReport &weather, meshtastic_Telemetry &m)
{
    m = {};
    m.which_variant = meshtastic_Telemetry_environment_metrics_tag;
    auto &v = m.variant.environment_metrics;
    v.has_wind_speed = weather.has_wind_speed;
    v.wind_speed = weather.wind_speed;
    v.has_wind_direction = weather.has_wind_direction;
    v.wind_direction = uint32_t(weather.wind_direction);
    v.has_wind_gust = weather.has_wind_gust;
    v.wind_gust = weather.wind_gust;
    v.has_wind_lull = weather.has_wind_lull;
    v.wind_lull = weather.wind_lull;
    // Absent station readings stay absent on the wire. Stock's buffer supplies
    // values for all fields once the station has supplied a full frame.
    v.has_temperature = weather.has_temperature;
    v.temperature = weather.temperature;
    v.has_voltage = weather.has_voltage;
    v.voltage = weather.voltage;
    v.has_rainfall_1h = weather.has_rainfall_1h;
    v.rainfall_1h = weather.rainfall_1h;
    v.has_rainfall_24h = weather.has_rainfall_24h;
    v.rainfall_24h = weather.rainfall_24h;
}
}
