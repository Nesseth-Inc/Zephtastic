// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "SensorBus.h"
#include <meshtastic/telemetry.pb.h>

namespace mt::stock::telemetry::accessory {
// Meshtastic HM330XSensor and Seeed driver 2704ca2.
struct StandardPm {
    uint16_t pm1, pm25, pm10;
    void apply(meshtastic_AirQualityMetrics &m) const {
        m.has_pm10_standard = m.has_pm25_standard = m.has_pm100_standard = true;
        m.pm10_standard = pm1;
        m.pm25_standard = pm25;
        m.pm100_standard = pm10;
    }
};
class Hm330x {
    Bus &bus;
    bool active = false;
    uint64_t ready = 0;
public:
    explicit Hm330x(Bus &b) : bus(b) {}
    void stop() { active = false; }
    int measure(StandardPm &out) {
        if (!active) {
            const uint8_t select = 0x88;
            int e = bus.write(&select, 1);
            if (e) return e;
            active = true;
            ready = bus.now() + 30000;
        }
        if (bus.now() < ready) return -EAGAIN;
        uint8_t bytes[29];
        int e = bus.read(bytes, sizeof(bytes));
        if (e) { active = false; return e; }
        uint8_t sum = 0;
        for (unsigned i = 0; i < 28; ++i) sum += bytes[i];
        if (sum != bytes[28]) return -EIO;
        auto word = [&](unsigned i) { return uint16_t((uint16_t(bytes[i]) << 8) | bytes[i+1]); };
        out = {word(4), word(6), word(8)};
        return 0;
    }
};
}
