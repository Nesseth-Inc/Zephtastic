// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "SensorBus.h"
#include <meshtastic/telemetry.pb.h>

namespace mt::stock::telemetry::accessory {
struct Formaldehyde {
    float ppb = 0, humidity = 0, celsius = 0;
    void apply(meshtastic_AirQualityMetrics &m) const {
        m.has_form_formaldehyde = m.has_form_humidity = m.has_form_temperature = true;
        m.form_formaldehyde = ppb;
        m.form_humidity = humidity;
        m.form_temperature = celsius;
    }
};

// Meshtastic SFA30Sensor and Sensirion SFA3x 1.0.0. Leave the worker free
// during the one-second reset settle and ten-second measurement warm-up.
class Sfa30 {
    Bus &bus;
    enum State { Idle, Resetting, Active } state = Idle;
    uint64_t ready = 0;
    uint8_t readFailures = 0;
public:
    explicit Sfa30(Bus &b) : bus(b) {}
    int measure(Formaldehyde &out) {
        if (state == Idle) {
            bus.wait(20);
            const int error = bus.command(0xd304);
            if (error) return error;
            state = Resetting;
            ready = bus.now() + 1000;
            return -EAGAIN;
        }
        if (bus.now() < ready) return -EAGAIN;
        if (state == Resetting) {
            const int error = bus.command(0x0006, 1);
            if (error) return error;
            state = Active;
            ready = bus.now() + 10000;
            readFailures = 0;
            return -EAGAIN;
        }
        uint16_t words[3];
        const int error = bus.words(0x0327, words, 3, 5);
        if (error) {
            if (++readFailures >= 3) state = Idle;
            return error;
        }
        readFailures = 0;
        out = {int16_t(words[0])/5.f, int16_t(words[1])/100.f, int16_t(words[2])/200.f};
        return 0;
    }
    void stop() {
        if (state == Active && bus.command(0x0104, 50)) return;
        state = Idle;
    }
};

struct SoilReadings {
    bool valid = false;
    uint8_t moisture = 0;
    float celsius = 0;
    uint64_t at = 0;
};

// Meshtastic enables its donor only on RAK4631. An explicitly wired accessory
// on other boards has a dedicated reset line and a continuously powered rail.
class Rak12035 {
    Bus &bus;
    int (*reset)(bool asserted);
    bool initialized = false;
    uint16_t dry = 0, wet = 0;
    uint64_t next = 0, bootDeadline = 0;
    bool waking = false;
    SoilReadings cached{};
    int read(uint8_t reg, uint8_t *p, size_t count) {
        int e = bus.write(&reg, 1);
        if (e) return e;
        bus.wait(20);
        return bus.read(p, count);
    }
    int readWord(uint8_t reg, uint16_t &out) {
        uint8_t bytes[2];
        int e = read(reg, bytes, 2);
        if (!e) out = (uint16_t(bytes[0]) << 8) | bytes[1];
        return e;
    }
    int calibration(uint8_t reg, uint16_t value) {
        const uint8_t frame[] = {reg, uint8_t(value >> 8), uint8_t(value)};
        int e = bus.write(frame, sizeof(frame));
        if (e) return e;
        bus.wait(200);
        uint16_t verify = 0;
        e = readWord(reg == 0x08 ? 0x0b : 0x0a, verify);
        return e ? e : verify == value ? 0 : -EIO;
    }
    int configure() {
        uint16_t zero, full;
        int e = readWord(0x0b, zero);
        if (!e) e = readWord(0x0a, full);
        if (e) return e;
        if (zero == 0) {
            zero = 510;
            if ((e = calibration(0x08, zero))) return e;
        }
        const uint16_t storedFull = full;
        if (full == 0 || full >= zero) {
            full = 390;
            if (storedFull != full && (e = calibration(0x07, full))) return e;
        }
        dry = zero;
        wet = full;
        return 0;
    }
    int wake(uint8_t &version) {
        if (!waking) {
            int e = reset(true);
            if (e) return e;
            bus.wait(250);
            if ((e = reset(false))) return e;
            bus.wait(500);
            waking = true;
            bootDeadline = bus.now() + 5000;
        }
        int e = read(0x04, &version, 1);
        if ((e || !version) && bus.now() < bootDeadline) return -EAGAIN;
        waking = false;
        return e ? e : version ? 0 : -ENODEV;
    }
    int moisture(uint8_t version, uint8_t &percent) {
        if (version > 2) return read(0x09, &percent, 1);
        uint16_t raw;
        int e = readWord(0x01, raw);
        if (e) return e;
        // Equal calibration points produce zero span in the v1/v2 algorithm.
        if (dry == wet) return -ERANGE;
        // RAK library supports slopes in both directions.
        const uint16_t lo = dry < wet ? dry : wet, hi = dry < wet ? wet : dry;
        if (raw < lo) raw = lo;
        if (raw > hi) raw = hi;
        percent = uint8_t((hi - raw) / ((hi - lo) / 100.0));
        return 0;
    }
public:
    Rak12035(Bus &b, int (*pin)(bool)) : bus(b), reset(pin) {}
    void stop() {
        if (waking) bus.writeReg(0x06, 0);
        cached = {}; next = 0; initialized = false; waking = false;
    }
    SoilReadings measure() {
        if (bus.now() < next) return cached;
        uint8_t version = 0, percent = 0;
        uint16_t temperature = 0;
        int e = wake(version);
        if (e) {
            cached = {};
            next = bus.now() + (waking ? 250 : 3000);
            return cached;
        }
        if (!initialized) { e = configure(); initialized = !e; }
        if (!e) {
            bus.wait(200);
            e = moisture(version, percent);
        }
        if (!e) {
            bus.wait(200);
            e = readWord(0x05, temperature);
            bus.wait(200);
        }
        const int sleep = bus.writeReg(0x06, 0);
        cached = {};
        if (!e && !sleep) cached = {true, percent, temperature / 10.f, bus.now()};
        // The worker polls once a second. Avoid resetting a sleeping soil
        // probe every second, while refreshing inside the 90-second cache.
        next = bus.now() + (cached.valid ? 60000 : 3000);
        return cached;
    }
};
}
