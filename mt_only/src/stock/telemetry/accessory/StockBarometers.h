// SPDX-License-Identifier: GPL-3.0-only
// Measurements follow Meshtastic 8e6a88d and its pinned Adafruit sensor libraries.
#pragma once
#include "SensorBus.h"
#include "environment/EnvironmentSource.h"
#include "Bmp3Compensation.h"
#include "meshtastic/telemetry.pb.h"

namespace mt::stock::telemetry::accessory {
using BarometerOrigin = zt::telemetry::EnvironmentSource;
struct BarometerSample {
    BarometerOrigin temperatureOrigin = BarometerOrigin::Unspecified, humidityOrigin = BarometerOrigin::Unspecified,
                    pressureOrigin = BarometerOrigin::Unspecified;
    bool hasTemperature = false, hasHumidity = false, hasPressure = false, clearHumidity = false;
    float temperature = 0, humidity = 0, pressure = 0;
    void merge(const BarometerSample &later) {
        if (later.hasTemperature && (later.hasPressure || !hasTemperature)) {
            hasTemperature = true;
            temperature = later.temperature;
            temperatureOrigin = later.temperatureOrigin;
        }
        if (later.clearHumidity) {
            clearHumidity = true;
            hasHumidity = false;
            humidity = 0;
            humidityOrigin = later.humidityOrigin;
        }
        if (later.hasHumidity && !hasHumidity) {
            hasHumidity = true;
            humidity = later.humidity;
            humidityOrigin = later.humidityOrigin;
        }
        if (later.hasPressure) {
            hasPressure = true;
            pressure = later.pressure;
            pressureOrigin = later.pressureOrigin;
        }
    }
    void apply(meshtastic_EnvironmentMetrics &m) const {
        if (hasTemperature && (hasPressure || !m.has_temperature)) {
            m.has_temperature = true;
            m.temperature = temperature;
        }
        if (clearHumidity) {
            m.has_relative_humidity = false;
            m.relative_humidity = 0;
        }
        if (hasHumidity && !m.has_relative_humidity) {
            m.has_relative_humidity = true;
            m.relative_humidity = humidity;
        }
        if (hasPressure) {
            m.has_barometric_pressure = true;
            m.barometric_pressure = pressure;
        }
    }
};

class AhtSensor {
    Bus &bus;
    bool initialized = false;
    int idle(uint8_t &status) {
        for (unsigned retry = 0; retry < 11; ++retry) {
            const int rc = bus.read(&status, 1);
            if (rc) return rc;
            if (!(status & 0x80)) return 0;
            if (retry < 10) bus.wait(10);
        }
        return -ETIMEDOUT;
    }

  public:
    explicit AhtSensor(Bus &b) : bus(b) {}
    void stop() { initialized = false; }
    int measure(BarometerSample &out) {
        uint8_t status = 0;
        if (!initialized) {
            const uint8_t reset = 0xba, cal[] = {0xe1, 8, 0};
            bus.wait(20); // Stock power-up delay before soft reset.
            int rc = bus.write(&reset, 1);
            if (rc) return rc;
            bus.wait(20);
            if ((rc = idle(status))) return rc;
            // Newer AHT20s may NACK the legacy AHT10 calibration command;
            // the calibrated status bit is authoritative on both.
            (void)bus.write(cal, sizeof(cal));
            if ((rc = idle(status))) return rc;
            if (!(status & 8)) return -EIO;
            initialized = true;
        }
        const uint8_t trigger[] = {0xac, 0x33, 0};
        uint8_t bytes[6]{};
        int rc = bus.write(trigger, sizeof(trigger));
        if (!rc) rc = idle(status);
        if (!rc) rc = bus.read(bytes, sizeof(bytes));
        if (rc || ((bytes[0] & 0x88) != 8)) {
            stop();
            return rc ? rc : -EIO;
        }
        const uint32_t h = (uint32_t(bytes[1]) << 12) | (uint32_t(bytes[2]) << 4) | (bytes[3] >> 4);
        const uint32_t t = (uint32_t(bytes[3] & 15) << 16) | (uint32_t(bytes[4]) << 8) | bytes[5];
        if (!out.hasTemperature) {
            out.hasTemperature = true;
            out.temperatureOrigin = BarometerOrigin::Aht;
            out.temperature = (float(t) * 200 / 0x100000) - 50;
        }
        if (!out.hasHumidity) {
            out.hasHumidity = true;
            out.humidityOrigin = BarometerOrigin::Aht;
            out.humidity = float(h) * 100 / 0x100000;
        }
        return 0;
    }
};

class Bmp085 {
    Bus &bus;
    uint16_t cal[11]{};
    bool initialized = false;
    static uint16_t be(const uint8_t *v) { return uint16_t(v[0]) << 8 | v[1]; }
    int adjust(uint32_t ut, int64_t &b5) const {
        const int64_t x1 = ((int64_t(ut) - cal[5]) * cal[4]) >> 15;
        const auto divisor = x1 + int16_t(cal[10]);
        if (!divisor) return -ERANGE;
        b5 = x1 + int64_t(int16_t(cal[9])) * 2048 / divisor;
        return 0;
    }
    int rawTemp(int64_t &b5) {
        uint8_t bytes[2];
        int rc = bus.writeReg(0xf4, 0x2e);
        if (rc) return rc;
        bus.wait(5);
        rc = bus.readReg(0xf6, bytes, 2);
        return rc ? rc : adjust(be(bytes), b5);
    }

  public:
    explicit Bmp085(Bus &b) : bus(b) {}
    void stop() { initialized = false; }
    int measure(BarometerSample &out) {
        int rc;
        if (!initialized) {
            uint8_t id, bytes[22];
            if ((rc = bus.readReg(0xd0, &id, 1))) return rc;
            if (id != 0x55) return -ENODEV;
            if ((rc = bus.readReg(0xaa, bytes, sizeof(bytes)))) return rc;
            for (unsigned i = 0; i < 11; ++i)
                cal[i] = be(bytes + 2 * i);
            if (!cal[3] || !cal[4] || (cal[3] == 0xffff && cal[4] == 0xffff)) return -ENODATA;
            initialized = true;
        }
        int64_t temp, forPressure;
        if ((rc = rawTemp(temp)) || (rc = rawTemp(forPressure)) || (rc = bus.writeReg(0xf4, 0xf4))) {
            stop();
            return rc;
        }
        bus.wait(26);
        uint8_t raw[3];
        if ((rc = bus.readReg(0xf6, raw, 3))) {
            stop();
            return rc;
        }
        const int64_t up = (uint32_t(be(raw)) << 8 | raw[2]) >> 5;
        // OSS 3 matches Adafruit_BMP085::begin(0x77), which clamps mode to 3.
        const auto b6 = forPressure - 4000;
        if (b6 < -65535 || b6 > 65535) return -ERANGE;
        const auto square = (b6 * b6) >> 12;
        int64_t x1 = (int16_t(cal[7]) * square) >> 11, x2 = (int16_t(cal[1]) * b6) >> 11;
        const auto b3 = ((int64_t(int16_t(cal[0])) * 4 + x1 + x2) * 8 + 2) / 4;
        x1 = (int16_t(cal[2]) * b6) >> 13;
        x2 = (int16_t(cal[6]) * square) >> 16;
        const auto b4 = (cal[3] * (((x1 + x2 + 2) >> 2) + 32768)) >> 15;
        if (b4 <= 0 || up < b3) return -ERANGE;
        const auto b7 = (up - b3) * 6250;
        if (b7 > UINT32_MAX) return -ERANGE;
        int64_t p = b7 < 0x80000000u ? (b7 * 2) / b4 : (b7 / b4) * 2;
        if (p < 1000 || p > 200000) return -ERANGE;
        x1 = (((p >> 8) * (p >> 8)) * 3038) >> 16;
        x2 = (-7357 * p) >> 16;
        p += (x1 + x2 + 3791) >> 4;
        out.hasTemperature = out.hasPressure = true;
        out.temperatureOrigin = out.pressureOrigin = BarometerOrigin::Bmp085;
        out.temperature = float((temp + 8) >> 4) / 10.0f;
        out.pressure = float(p) / 100.0f;
        return 0;
    }
};
class Bmp3 {
    Bus &bus;
    bool initialized = false;
    bmp3math::bmp3_calib_data cal{};
    static uint16_t le(const uint8_t *b) { return uint16_t(b[1]) << 8 | b[0]; }
    static uint32_t le24(const uint8_t *b) { return uint32_t(b[2]) << 16 | le(b); }
    int initialize() {
        uint8_t id, status, trim[21];
        int rc = bus.readReg(0, &id, 1);
        if (rc) return rc;
        if (id != 0x50 && id != 0x60) return -ENODEV;
        if ((rc = bus.readReg(3, &status, 1))) return rc;
        if (!(status & 0x10)) return -EAGAIN;
        if ((rc = bus.writeReg(0x7e, 0xb6))) return rc;
        bus.wait(3);
        if ((rc = bus.readReg(2, &status, 1))) return rc;
        if (status & 2) return -EIO;
        if ((rc = bus.readReg(0x31, trim, sizeof(trim)))) return rc;
        bool nonzero = false, nonones = false;
        for (auto b : trim) {
            nonzero |= b != 0;
            nonones |= b != 255;
        }
        if (!nonzero || !nonones) return -ENODATA;
        // Exact binary scales from Bosch's parse_calib_data.
        cal.quantized_calib_data = {le(trim) * 256.0,
                                    le(trim + 2) * 0x1p-30,
                                    int8_t(trim[4]) * 0x1p-48,
                                    (int16_t(le(trim + 5)) - 16384) * 0x1p-20,
                                    (int16_t(le(trim + 7)) - 16384) * 0x1p-29,
                                    int8_t(trim[9]) * 0x1p-32,
                                    int8_t(trim[10]) * 0x1p-37,
                                    le(trim + 11) * 8.0,
                                    le(trim + 13) * 0x1p-6,
                                    int8_t(trim[15]) * 0x1p-8,
                                    int8_t(trim[16]) * 0x1p-15,
                                    int16_t(le(trim + 17)) * 0x1p-48,
                                    int8_t(trim[19]) * 0x1p-48,
                                    int8_t(trim[20]) * 0x1p-65,
                                    0};
        if ((rc = bus.writeReg(0x1c, 0x13)) || (rc = bus.writeReg(0x1d, 3)) || (rc = bus.writeReg(0x1f, 4)))
            return rc;
        initialized = true;
        return 0;
    }

  public:
    explicit Bmp3(Bus &b) : bus(b) {}
    void stop() { initialized = false; }
    int measure(BarometerSample &out) {
        const unsigned conversions = initialized ? 1 : 4;
        int rc;
        if (!initialized && (rc = initialize())) return rc;
        uint8_t raw[6];
        // The stock initializer takes three forced readings to settle the filter.
        // Do these on the first worker pass, before exposing the fourth sample.
        for (unsigned i = 0; i < conversions; ++i) {
            if ((rc = bus.writeReg(0x1b, 0x13))) {
                stop();
                return rc;
            }
            bus.wait(36);
            uint8_t status;
            if ((rc = bus.readReg(3, &status, 1)) || (status & 0x60) != 0x60 ||
                (rc = bus.readReg(4, raw, sizeof(raw)))) {
                stop();
                return rc ? rc : -ETIMEDOUT;
            }
        }
        const bmp3math::bmp3_uncomp_data sample{le24(raw), le24(raw + 3)};
        const float t = float(bmp3math::compensate_temperature(&sample, &cal));
        const float p = float(bmp3math::compensate_pressure(&sample, &cal)) / 100.0f;
        if (!validTemp(t) || !(p > 0 && p <= 2000)) return -ENODATA;
        out.hasTemperature = out.hasPressure = out.clearHumidity = true;
        out.temperatureOrigin = out.humidityOrigin = out.pressureOrigin = BarometerOrigin::Bmp3xx;
        out.hasHumidity = false;
        out.temperature = t;
        out.pressure = p;
        return 0;
    }
};

class DpsSensor {
    Bus &bus;
    bool initialized = false, touched = false;
    int32_t c0 = 0, c1 = 0, c00 = 0, c10 = 0, c01 = 0, c11 = 0, c20 = 0, c21 = 0, c30 = 0;
    static int32_t signedBits(uint32_t v, unsigned bits) {
        return int32_t(v) - ((v & (1u << (bits - 1))) ? int32_t(1u << bits) : 0);
    }
    static int32_t be16(const uint8_t *b) { return signedBits(uint32_t(b[0]) << 8 | b[1], 16); }
    static int32_t be24(const uint8_t *b) {
        return signedBits(uint32_t(b[0]) << 16 | uint32_t(b[1]) << 8 | b[2], 24);
    }
    int initialize() {
        uint8_t id, status, source, c[18];
        int rc = bus.readReg(0x0d, &id, 1);
        if (rc) return rc;
        if (id != 0x10) return -ENODEV;
        if ((rc = bus.writeReg(0x0c, 0x89))) return rc;
        touched = true;
        bus.wait(10);
        for (unsigned i = 0; i < 100; i++) {
            if ((rc = bus.readReg(8, &status, 1))) return rc;
            if ((status & 0xc0) == 0xc0) break;
            if (i == 99) return -ETIMEDOUT;
            bus.wait(1);
        }
        if ((rc = bus.readReg(0x10, c, sizeof(c))) || (rc = bus.readReg(0x28, &source, 1))) return rc;
        c0 = signedBits(uint32_t(c[0]) << 4 | c[1] >> 4, 12);
        c1 = signedBits(uint32_t(c[1] & 15) << 8 | c[2], 12);
        c00 = signedBits(uint32_t(c[3]) << 12 | uint32_t(c[4]) << 4 | c[5] >> 4, 20);
        c10 = signedBits(uint32_t(c[5] & 15) << 16 | uint32_t(c[6]) << 8 | c[7], 20);
        c01 = be16(c + 8);
        c11 = be16(c + 10);
        c20 = be16(c + 12);
        c21 = be16(c + 14);
        c30 = be16(c + 16);
        // Stock requests 1 Hz, 4x on both channels; the factory coefficients
        // select the internal/external temperature source and no result shifting.
        if ((rc = bus.writeReg(6, 2)) || (rc = bus.writeReg(7, (source & 0x80) | 2)) ||
            (rc = bus.update(9, 0x0c, 0)) || (rc = bus.writeReg(8, 7)))
            return rc;
        initialized = true;
        return 0;
    }

  public:
    explicit DpsSensor(Bus &b) : bus(b) {}
    void stop() {
        initialized = false;
        if (touched && !bus.writeReg(8, 0)) touched = false;
    }
    int measure(BarometerSample &out) {
        int rc;
        if (!initialized && (rc = initialize())) return rc;
        uint8_t status, raw[6];
        if ((rc = bus.readReg(8, &status, 1))) {
            stop();
            return rc;
        }
        if ((status & 0x30) != 0x30) return -EAGAIN;
        if ((rc = bus.readReg(0, raw, sizeof(raw)))) {
            stop();
            return rc;
        }
        const float t = float(be24(raw + 3)) / 3670016, p = float(be24(raw)) / 3670016;
        const float temp = t * c1 + c0 / 2.0;
        const float hpa = (c00 + p * (c10 + p * (c20 + p * c30)) + t * (c01 + p * (c11 + p * c21))) / 100.0f;
        if (!validTemp(temp) || !(hpa > 0 && hpa <= 2000)) return -ENODATA;
        out.temperatureOrigin = out.pressureOrigin = BarometerOrigin::Dps310;
        out.hasPressure = out.hasTemperature = true;
        out.pressure = hpa;
        out.temperature = temp;
        return 0;
    }
};
} // namespace mt::stock::telemetry::accessory
