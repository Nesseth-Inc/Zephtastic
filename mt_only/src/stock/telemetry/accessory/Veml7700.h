// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d VEML7700Sensor and Adafruit VEML7700 2.1.6.
#pragma once
#include "SensorBus.h"
#include <float.h>
#include "meshtastic/telemetry.pb.h"

namespace mt::stock::telemetry::accessory {
struct VemlLight {
    bool hasVisible = false, hasWhite = false;
    float visible = 0, white = 0;
    void apply(meshtastic_EnvironmentMetrics &out,bool allowVisible=true) const {
        if (allowVisible && hasVisible && visible >= 0 && visible <= FLT_MAX) { out.has_lux = true; out.lux = visible; }
        if (hasWhite && white >= 0 && white <= FLT_MAX) { out.has_white_lux = true; out.white_lux = white; }
    }
};

class Veml7700 {
    enum Phase : uint8_t { Idle, Wake, First, Dim, Bright, White };
    static constexpr uint8_t gains[] = {2, 3, 0, 1};
    static constexpr uint8_t times[] = {12, 8, 0, 1, 2, 3};
    static constexpr uint16_t configMask = (3 << 11) | (15 << 6);
    Bus &bus;
    Phase phase = Idle;
    uint8_t gain = 0, time = 2;
    uint16_t config = 0;
    uint64_t due = 0;
    bool initialized = false, touched = false;
    VemlLight pending{};

    static int duration(unsigned code) {
        for (unsigned i = 0; i < sizeof(times); ++i)
            if (times[i] == code) return 25 << i;
        return -1;
    }
    int read(unsigned reg, uint16_t &value) {
        uint8_t bytes[2];
        int error = bus.readReg(reg, bytes, sizeof(bytes));
        if (!error) value = uint16_t(bytes[1]) << 8 | bytes[0];
        return error;
    }
    int write(unsigned reg, uint16_t value) {
        const uint8_t bytes[] = {uint8_t(reg), uint8_t(value), uint8_t(value >> 8)};
        return bus.write(bytes, sizeof(bytes));
    }
    int restart(int error) {
        initialized = false;
        phase = Idle;
        pending = {};
        return error;
    }
    int begin() {
        uint16_t old;
        int error = read(0, old);
        if (error) return restart(error);
        const int flush = duration((old >> 6) & 15);
        if (flush < 0) return restart(-EIO);
        touched = true;
        config = (old & ~(configMask | 0x33)) | (2 << 11);
        if ((error = write(0, old | 1)) || (error = write(0, config | 1))) return restart(error);
        due = bus.now() + flush;
        phase = Wake;
        return -EAGAIN;
    }
    int wake() {
        uint16_t power;
        int error = read(3, power);
        if (error || (error = write(3, power & ~1)) || (error = write(0, config))) return restart(error);
        bus.wait(5); // Oscillator startup, as in enable(true).
        config |= 2;
        if ((error = write(2, 10000)) || (error = write(1, 20000)) || (error = write(0, config))) return restart(error);
        initialized = true;
        gain = 0;
        time = 2;
        return expose(First, 0, 2, true);
    }
    int expose(Phase next, uint8_t newGain, uint8_t newTime, bool setTime) {
        // The driver flushes the old integration cycle on every setIntegrationTime,
        // including autoLux's reset to 100 ms at the start of each sample.
        const unsigned flush = setTime ? (25u << time) : 0;
        config = (config & ~configMask) | (gains[newGain] << 11) | (times[newTime] << 6);
        int error = write(0, config);
        if (error) return restart(error);
        gain = newGain;
        time = newTime;
        due = bus.now() + flush + (50u << time);
        phase = next;
        return -EAGAIN;
    }
    float lux(uint16_t raw, bool correct) const {
        constexpr float factor[] = {0.125f, 0.25f, 1.0f, 2.0f};
        float value = 0.0036f * (800.f / (25u << time)) * (2.f / factor[gain]) * raw;
        if (correct)
            value = (((6.0135e-13 * value - 9.3924e-9) * value + 8.1488e-5) * value + 1.0023) * value;
        return value;
    }
public:
    explicit Veml7700(Bus &device) : bus(device) {}
    // Each poll performs at most one conversion step. Leave the worker free for
    // other devices throughout the integration and old-cycle flush windows.
    int poll(VemlLight &out) {
        if (phase == Idle) {
            pending = {};
            return initialized ? expose(First, 0, 2, true) : begin();
        }
        if (bus.now() < due) return -EAGAIN;
        if (phase == Wake) return wake();
        uint16_t raw;
        if (phase == White) {
            const int error = read(5, raw);
            if (!error) {
                pending.hasWhite = true;
                // Stock compares its int16_t white count before converting it back to uint16_t.
                pending.white = lux(raw, raw > 100 && raw <= INT16_MAX);
            }
            phase = Idle;
            out = pending;
            return out.hasVisible || out.hasWhite ? 0 : error;
        }
        if (!read(4, raw)) {
            if (phase == First) phase = raw <= 100 ? Dim : Bright;
            if (phase == Dim && raw <= 100) {
                if (gain < 3) return expose(Dim, gain+1, time, false);
                if (time < 5) return expose(Dim, gain, time+1, true);
            }
            if (phase == Bright && raw > 10000 && time > 0) return expose(Bright, gain, time-1, true);
            pending.hasVisible = true;
            pending.visible = lux(raw, phase == Bright);
        }
        phase = White;
        due = bus.now() + (50u << time);
        return -EAGAIN;
    }
    void stop() {
        restart(0);
        if (touched && !write(0, config | 1)) touched = false;
    }
};
}
