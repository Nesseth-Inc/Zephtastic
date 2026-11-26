// SPDX-License-Identifier: GPL-3.0-only
// Stock TSL2591/TSL2561 readout using the Adafruit 1.4.5/1.1.3 transfer and lux rules.
#pragma once
#include "SensorBus.h"

namespace mt::stock::telemetry::accessory {
inline bool tsl2591Lux(uint16_t full, uint16_t ir, float &lux) {
    if (!full || full == UINT16_MAX || ir == UINT16_MAX) return false;
    constexpr float countsPerLux = 100.0f / 408.0f;
    lux = (float(full) - float(ir)) * (1.0f - float(ir) / float(full)) / countsPerLux;
    return true;
}
inline float tsl2561Lux(uint16_t rawFull, uint16_t rawIr) {
    // Adafruit's getEvent signals saturation but stock still copies its finite wire sentinel.
    if (rawFull > 37000 || rawIr > 37000) return 65536;
    constexpr uint32_t scale = 0x0fe7u << 4;
    const uint32_t full = (rawFull * scale) >> 10;
    const uint32_t ir = (rawIr * scale) >> 10;
    const uint32_t ratio = full ? (((ir << 10) / full + 1) >> 1) : 0;
    struct Band { uint16_t maxRatio, broadband, infrared; };
    constexpr Band bands[] = {
        {0x40,0x1f2,0x1be}, {0x80,0x214,0x2d1}, {0xc0,0x23f,0x37b},
        {0x100,0x270,0x3fe}, {0x138,0x16f,0x1fc}, {0x19a,0xd2,0xfb}, {0x29a,0x18,0x12}
    };
    for (const auto &band : bands) {
        if (ratio > band.maxRatio) continue;
        const uint32_t visible = full * band.broadband, infrared = ir * band.infrared;
        return visible > infrared ? ((visible - infrared + 8192) >> 14) : 0;
    }
    return 0;
}
template<bool Is2591> class TslLight {
    Bus &bus;
    bool configured=false, active=false;
    static constexpr uint8_t control = Is2591 ? 0xa0 : 0x80;
    int read(uint8_t reg, uint8_t *bytes, size_t n) {
        if constexpr (Is2591) return bus.readReg(reg,bytes,n);
        const int error = bus.write(&reg,1); // Stock 2561 Wire uses a STOP between pointer selection and read.
        return error ? error : bus.read(bytes,n);
    }
    int powerDown() {
        const int error = bus.writeReg(control,0);
        if (!error) active=false;
        return error;
    }
public:
    explicit TslLight(Bus &port):bus(port) {}
    void stop() { configured=false; if (active) powerDown(); }
    bool sample(float &lux) {
        if (active && powerDown()) return false;
        if (!configured) {
            uint8_t id=0;
            if (read(Is2591 ? 0xb2 : 0x8a,&id,1)) return false;
            if (Is2591 ? id!=0x50 : (id>>4)!=5) return false;
        }
        active=true;
        int error=bus.writeReg(control,Is2591 ? 0x93 : 0x03);
        if (!error && !configured) {
            error=bus.writeReg(control+1,Is2591 ? 0 : 1); // 1x gain, 100 or 101 ms.
            if (!error) {
                error=powerDown();
                if (!error) { active=true;error=bus.writeReg(control,Is2591 ? 0x93 : 0x03); }
            }
        }
        uint8_t full[2]{}, ir[2]{};
        if (!error) {
            bus.wait(120); // Both pinned stock drivers wait 120 ms after enabling integration.
            error=read(Is2591 ? 0xb4 : 0xac,full,2);
            if (!error) error=read(Is2591 ? 0xb6 : 0xae,ir,2);
        }
        const int shutdown=powerDown();
        configured=!error && !shutdown;
        if (!configured) return false;
        const uint16_t wide=uint16_t(full[1])<<8 | full[0], infrared=uint16_t(ir[1])<<8 | ir[0];
        if constexpr (Is2591) return tsl2591Lux(wide,infrared,lux);
        lux=tsl2561Lux(wide,infrared);
        return true;
    }
};
}
