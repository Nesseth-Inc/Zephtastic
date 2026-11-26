// SPDX-License-Identifier: GPL-3.0-only
// Wire transactions and units from Meshtastic RCWL9620Sensor and CGRadSensSensor.
#pragma once
#include "SensorBus.h"

namespace mt::stock::telemetry::accessory {
inline uint32_t bigEndian24(const uint8_t (&bytes)[3]) {
    return uint32_t(bytes[0]) << 16 | uint32_t(bytes[1]) << 8 | bytes[2];
}

inline bool staticRadiation(Bus &bus, float &microRoentgensPerHour) {
    uint8_t bytes[3];
    const uint8_t staticIntensity = 0x06;
    // The stock sensor sends STOP after selecting the intensity register.
    if (bus.write(&staticIntensity, 1) || bus.read(bytes, sizeof(bytes))) return false;
    microRoentgensPerHour = float(bigEndian24(bytes)) / 10.0f;
    return true;
}

inline bool ultrasonicRange(Bus &bus, float &millimeters) {
    const uint8_t start = 0x01;
    if (bus.write(&start, 1)) return false;
    bus.wait(100);
    uint8_t bytes[3];
    if (bus.read(bytes, sizeof(bytes))) return false;
    const float measured = float(bigEndian24(bytes)) / 1000.0f;
    millimeters = measured > 4500.0f ? 4500.0f : measured;
    return true;
}
}
