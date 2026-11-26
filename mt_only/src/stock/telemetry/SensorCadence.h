// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d AirQualityTelemetry::runOnce and HealthTelemetry::runOnce.
// The Zephyr owner supplies fresh measurements and a monotonic 64-bit clock.
#pragma once
#include <stdint.h>

namespace mt::stock::telemetry {
class SensorCadence {
    uint64_t nextPoll_ = 0;
    uint64_t lastMesh_ = 0;
    uint32_t configuredSeconds_ = 0;
    bool enabled_ = false;
    bool sent_ = false;
public:
    void configure(bool enabled, uint32_t seconds, uint64_t now, uint32_t firstDelay)
    {
        if (enabled_ == enabled && configuredSeconds_ == seconds) return;
        enabled_ = enabled;
        configuredSeconds_ = seconds;
        nextPoll_ = now + firstDelay;
        sent_ = false;
    }
    uint32_t configuredSeconds() const { return configuredSeconds_; }
    uint64_t nextPoll() const { return nextPoll_; }
    bool beginPoll(uint64_t now)
    {
        if (!enabled_ || now < nextPoll_) return false;
        nextPoll_ = now + 60000;  // Both stock producers poll at the phone-report cadence.
        return true;
    }
    bool meshDue(uint64_t now, uint32_t intervalMs) const
    {
        return !sent_ || (now >= lastMesh_ && now - lastMesh_ >= intervalMs);
    }
    void queued(uint64_t now) { lastMesh_ = now; sent_ = true; }
};
}
