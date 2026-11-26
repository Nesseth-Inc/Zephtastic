// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d, modules/Telemetry/{AirQualityTelemetry,HealthTelemetry}.cpp.
#pragma once
#include <meshtastic/telemetry.pb.h>
#include <stddef.h>

namespace mt::stock::AirQualityTelemetryModule {
// Sensor instances are supplied by the board. Accumulate every available
// reading even if an earlier sensor succeeded. No readings means no packet.
template<class Sensor, size_t N>
bool getAirQualityTelemetry(meshtastic_AirQualityMetrics &m, const Sensor (&sensors)[N])
{
    bool valid = false;
    bool hasSensor = false;
    m = meshtastic_AirQualityMetrics_init_zero;
    for (auto sensor : sensors) {
        if (sensor) {
            bool sensor_get = sensor(m);
            valid = valid || sensor_get;
            hasSensor = true;
        }
    }
    return valid && hasSensor;
}
}

namespace mt::stock::HealthTelemetryModule {
template<class Optical, class Temperature>
bool getHealthTelemetry(meshtastic_HealthMetrics &m, bool hasOptical, Optical optical,
                        bool hasTemperature, Temperature temperature)
{
    bool valid = false;
    bool hasSensor = false;
    bool get_metrics;
    m = meshtastic_HealthMetrics_init_zero;

    if (hasOptical) {
        get_metrics = optical(m);
        valid = valid || get_metrics;
        hasSensor = true;
    }
    if (hasTemperature) {
        get_metrics = temperature(m);
        valid = valid || get_metrics;
        hasSensor = true;
    }
    return valid && hasSensor;
}
}

namespace mt::stock::telemetry {
// Both stock allocReply/sendTelemetry paths choose the variant before
// requesting measurements. ZT samples once and shares its timestamp-checked
// snapshot between the phone, mesh scheduler, and solicited reply.
template<class Snapshot>
bool allocReply(unsigned variant, const Snapshot &sample, uint32_t utc, meshtastic_Telemetry &m)
{
    m = meshtastic_Telemetry_init_zero;
    if (variant == meshtastic_Telemetry_air_quality_metrics_tag && sample.hasAir) {
        m.time = utc;
        m.which_variant = meshtastic_Telemetry_air_quality_metrics_tag;
        m.variant.air_quality_metrics = sample.air;
        return true;
    }
    if (variant == meshtastic_Telemetry_health_metrics_tag && sample.hasHealth) {
        m.time = utc;
        m.which_variant = meshtastic_Telemetry_health_metrics_tag;
        m.variant.health_metrics = sample.health;
        return true;
    }
    return false;
}
}
