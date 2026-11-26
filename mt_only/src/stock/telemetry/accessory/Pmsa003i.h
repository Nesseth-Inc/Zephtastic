// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Hm330x.h"

namespace mt::stock::telemetry::accessory {
struct PlantowerPm {
    StandardPm standard{};
    uint16_t atmospheric[3]{}, counts[6]{};
    void apply(meshtastic_AirQualityMetrics &m) const {
        standard.apply(m);
        m.has_pm10_environmental = m.has_pm25_environmental = m.has_pm100_environmental = true;
        m.pm10_environmental = atmospheric[0];
        m.pm25_environmental = atmospheric[1];
        m.pm100_environmental = atmospheric[2];
        m.has_particles_03um = m.has_particles_05um = m.has_particles_10um = true;
        m.has_particles_25um = m.has_particles_50um = m.has_particles_100um = true;
        m.particles_03um = counts[0]; m.particles_05um = counts[1]; m.particles_10um = counts[2];
        m.particles_25um = counts[3]; m.particles_50um = counts[4]; m.particles_100um = counts[5];
    }
};
class Pmsa003i {
    Bus &bus;
    int (*set)(bool);
    bool active = false, pinKnown = false, completed = false, warmed = false, haveFallback = false;
    uint32_t ready = 0, completedFor = 0, fallbackDue = 0;
    int frame(PlantowerPm &out) {
        uint8_t bytes[32];
        const int e = bus.read(bytes,sizeof(bytes));
        if (e) return e;
        if (bytes[0] != 0x42 || bytes[1] != 0x4d) return -EIO;
        auto word = [&](unsigned i) { return uint16_t((uint16_t(bytes[i]) << 8) | bytes[i+1]); };
        uint16_t sum = 0;
        for (unsigned i=0; i<30; ++i) sum += bytes[i];
        if (word(30) != sum) return -EIO;
        out.standard = {word(4), word(6), word(8)};
        for (unsigned i=0;i<3;++i) out.atmospheric[i]=word(10+i*2);
        for (unsigned i=0;i<6;++i) out.counts[i]=word(16+i*2);
        return 0;
    }
public:
    Pmsa003i(Bus &b,int (*enable)(bool)) : bus(b), set(enable) {}
    void stop() {
        if (set && (!pinKnown || active)) {
            if (set(false)) return;
            pinKnown=true;
        }
        active=false; completed=false; warmed=false; haveFallback=false;
    }
    int measure(PlantowerPm &out, uint32_t seconds, uint32_t deadline, bool scheduled) {
        if (!set) return frame(out);
        const uint32_t now=uint32_t(bus.now());
        if (!scheduled && !haveFallback) { fallbackDue=now+30000;haveFallback=true; }
        const uint32_t target=scheduled?deadline:fallbackDue;
        const bool cycle=seconds>30;
        if (!active) {
            if (cycle && ((completed && completedFor==target) || int32_t(target-now)>32000)) {
                if (!pinKnown) {
                    const int e=set(false);if(e)return e;
                    pinKnown=true;
                }
                return -EAGAIN;
            }
            const int e=set(true);if(e)return e;
            active=true;pinKnown=true;warmed=false;ready=now+30000;
        }
        if (!warmed) {
            if (int32_t(now-ready)<0) return -EAGAIN;
            warmed=true;
        }
        const int e=frame(out);
        if (e) return e;
        if (cycle && int32_t(target-now)<=2000) {
            // Allow the owner one worker tick to pick up a fresh reading before its poll.
            const int sleep=set(false);if(sleep)return sleep;
            active=false;completed=true;completedFor=target;
            fallbackDue=target+(seconds<60?seconds:60)*1000;
        }
        return 0;
    }
};
}
