// SPDX-License-Identifier: GPL-3.0-only
// The checked stock source and modem resolver seams have their own translation
// unit, keeping stock's global configuration separate from the real ZT Service.
#include "Default.cpp"

uint32_t stockInterval(unsigned role, uint32_t seconds, uint32_t online, unsigned sf, float bw, int8_t throttle) {
    config.device.role = static_cast<Role>(role);
    resolvedSF = sf;
    resolvedBW = bw;
    profile.telemetryThrottle = throttle;
    return Default::getConfiguredOrDefaultMsScaled(seconds, default_telemetry_broadcast_interval_secs,
                                                    online, TrafficType::TELEMETRY);
}
