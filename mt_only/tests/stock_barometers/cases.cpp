// SPDX-License-Identifier: GPL-3.0-only
#include "EnvironmentQuery.h"
#include "stock/telemetry/accessory/Accessories.h"
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace mt::stock::telemetry::accessory;
meshtastic_EnvironmentMetrics donorSample(int, const uint8_t *, const uint8_t *, uint32_t);
namespace {
enum Part { Aht, Bmp085Part, Dps, Bmp3Part };
struct Device : Bus {
    Part kind;
    std::array<uint8_t, 22> trim{};
    std::array<uint8_t, 6> raw{};
    int secondTemp = 0, reads = 0, failReg = -1, id = 0, status = 8;
    int dpMode = 0, conversions = 0, source = 0, dpCfg = 0;
    bool absent = false, nackCalibration = false;
    unsigned waited = 0, busy = 0;
    std::vector<unsigned> writes;
    explicit Device(Part p) : kind(p) { fixture(0); }
    void fixture(unsigned n) {
        if (kind == Aht) {
            const uint32_t h = (n * 7919) & 0xfffff, t = (n * 3571) & 0xfffff;
            raw = {
                8,         uint8_t(h >> 12), uint8_t(h >> 4), uint8_t((h << 4) | (t >> 16)), uint8_t(t >> 8),
                uint8_t(t)};
        } else if (kind == Bmp085Part) {
            const int c[] = {408, -72, -14383, 32741, 32757, 23153, 6190, 4, -32768, -8711, 2868};
            for (size_t i = 0; i < 11; i++) {
                trim[2 * i] = uint16_t(c[i]) >> 8;
                trim[2 * i + 1] = c[i];
            }
            const uint32_t t = 24500 + 43 * n, p = (110000 + n * 1546) << 5;
            raw = {uint8_t(t >> 8), uint8_t(t), uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p), 0};
            secondTemp = t + 25;
            reads = 0;
            id = 0x55;
        } else if (kind == Dps) {
            // Packed signed c0,c1,c00,c10,c01,c11,c20,c21,c30; the donor decodes these independently.
            trim = {0x02, 0xdf, 0xf0, 0x17, 0x92, 0xff, 0xe8, 0xb1, 0xff,
                    0x7b, 0,    0x4e, 0x01, 0x23, 0xff, 0xce, 0xff, 0xf4};
            const uint32_t p = (n * 23473 - 0x149abc) & 0xffffff, t = (n * 54991 - 0x20abcd) & 0xffffff;
            raw = {uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p),
                   uint8_t(t >> 16), uint8_t(t >> 8), uint8_t(t)};
            id = 0x10;
            status = 0xf0;
            source = n % 2 ? 0 : 0x80;
        } else {
            trim = {0x78, 0x69, 0x0f, 0x40, 0xf4, 0xfa, 0x42, 0x2d, 0x41, 0xfe, 0x02,
                    0xe0, 0x2e, 0x11, 0x02, 0xe7, 0x03, 0xee, 0xff, 0x01, 0xff};
            const uint32_t p = 1000000 + n * 36122, t = 7000000 + n * 24122;
            raw = {uint8_t(p), uint8_t(p >> 8), uint8_t(p >> 16),
                   uint8_t(t), uint8_t(t >> 8), uint8_t(t >> 16)};
            id = n % 2 ? 0x50 : 0x60;
            status = 0x70;
        }
    }
    int write(const uint8_t *p, size_t n) override {
        if (absent || failReg == p[0]) return -EIO;
        writes.push_back(n == 2 ? (unsigned(p[0]) << 8 | p[1]) : p[0]);
        if (kind == Aht) {
            assert((n == 1 && p[0] == 0xba) || (n == 3 && p[0] == 0xe1 && p[1] == 8 && p[2] == 0) ||
                   (n == 3 && p[0] == 0xac && p[1] == 0x33 && p[2] == 0));
            return nackCalibration && p[0] == 0xe1 ? -EIO : 0;
        }
        assert(n == 2);
        if (kind == Bmp085Part) {
            assert(p[0] == 0xf4 && (p[1] == 0x2e || p[1] == 0xf4));
        }
        if (kind == Dps && p[0] == 8) dpMode = p[1];
        if (kind == Dps && p[0] == 7) dpCfg = p[1];
        if (kind == Bmp3Part && p[0] == 0x1b) {
            assert(p[1] == 0x13);
            ++conversions;
        }
        return 0;
    }
    int read(uint8_t *b, size_t n) override {
        if (absent || failReg == 0x100) return -EIO;
        assert(kind == Aht);
        if (n == 1) {
            b[0] = busy ? (--busy, 0x88) : status;
            return 0;
        }
        assert(n == 6);
        memcpy(b, raw.data(), n);
        return 0;
    }
    int readReg(uint8_t reg, uint8_t *b, size_t n) override {
        if (absent || failReg == reg) return -EIO;
        if (kind == Dps) {
            if (reg == 0x0d) {
                *b = id;
                return 0;
            }
            if (reg == 8) {
                *b = status;
                return 0;
            }
            if (reg == 0x28) {
                *b = source;
                return 0;
            }
            if (reg == 9) {
                *b = 0;
                return 0;
            }
            if (reg == 0x10 && n == 18) {
                memcpy(b, trim.data(), n);
                return 0;
            }
            assert(reg == 0 && n == 6);
            memcpy(b, raw.data(), n);
            return 0;
        }
        if (kind == Bmp085Part) {
            if (reg == 0xd0) {
                *b = id;
                return 0;
            }
            if (reg == 0xaa && n == 22) {
                memcpy(b, trim.data(), n);
                return 0;
            }
            assert(reg == 0xf6);
            if (n == 2) {
                if (reads++ % 2 == 0)
                    memcpy(b, raw.data(), 2);
                else {
                    b[0] = secondTemp >> 8;
                    b[1] = secondTemp;
                }
                return 0;
            }
            assert(n == 3);
            memcpy(b, raw.data() + 2, 3);
            return 0;
        }
        assert(kind == Bmp3Part);
        if (reg == 0) {
            *b = id;
            return 0;
        }
        if (reg == 3) {
            *b = status;
            return 0;
        }
        if (reg == 2) {
            *b = 0;
            return 0;
        }
        if (reg == 0x31 && n == 21) {
            memcpy(b, trim.data(), 21);
            return 0;
        }
        assert(reg == 4 && n == 6);
        memcpy(b, raw.data(), 6);
        return 0;
    }
    bool wrote(unsigned v) const {
        for (auto w : writes)
            if (w == v) return true;
        return false;
    }
    void wait(unsigned ms) override { waited += ms; }
    uint64_t now() override { return waited; }
    uint32_t utc() override { return 0; }
    int load(const char *, void *, size_t) override { return -ENOENT; }
    int save(const char *, const void *, size_t) override { return -ENOTSUP; }
};
std::array<Device, 4> boards{Device(Aht), Device(Bmp085Part), Device(Dps), Device(Bmp3Part)};
Device &board(int addr) {
    if (addr == 0x38) return boards[Aht];
    if (addr == 0x76) return boards[Dps];
    assert(addr == 0x77);
#ifdef BMP3_PROFILE
    return boards[Bmp3Part];
#else
    return boards[Bmp085Part];
#endif
}
void same(const meshtastic_EnvironmentMetrics &got, const meshtastic_EnvironmentMetrics &want) {
    assert(got.has_temperature == want.has_temperature &&
           got.has_relative_humidity == want.has_relative_humidity &&
           got.has_barometric_pressure == want.has_barometric_pressure);
    assert(got.temperature == want.temperature && got.relative_humidity == want.relative_humidity &&
           got.barometric_pressure == want.barometric_pressure);
}
template <class Sensor> void differential(Part p) {
    for (unsigned n = 0; n < 112; n++) {
        Device d(p);
        d.fixture(n);
        Sensor driver(d);
        BarometerSample sample;
        auto want = donorSample(p, d.trim.data(), d.raw.data(), d.secondTemp);
        if (p == Aht) {
            d.busy = n % 10;
            d.nackCalibration = n % 2;
        }
        int rc = driver.measure(sample);
        if (rc) std::fprintf(stderr, "sensor %d fixture %u rc %d\n", p, n, rc);
        assert(rc == 0);
        meshtastic_EnvironmentMetrics got{};
        if (p == Bmp3Part) {
            got.has_relative_humidity = true;
            got.relative_humidity = 82;
            assert(d.conversions == 4 && d.wrote(0x1c13) && d.wrote(0x1d03) && d.wrote(0x1f04));
        }
        sample.apply(got);
        same(got, want);
        if (p == Aht) {
            assert(d.wrote(0xba) && d.wrote(0xe1) && d.wrote(0xac));
            got.has_relative_humidity = true;
            got.relative_humidity = 17;
            got.temperature = 31;
            sample.apply(got);
            assert(got.temperature == 31 && got.relative_humidity == 17);
        }
        if (p == Bmp085Part) assert(d.reads == 2 && d.wrote(0xf4f4) && d.waited == 36);
        if (p == Dps) {
            assert(d.dpMode == 7 && d.dpCfg == (d.source | 2) && d.wrote(0x0602));
            d.failReg = 8;
            driver.stop();
            assert(d.dpMode == 7);
            d.failReg = -1;
            driver.stop();
            assert(d.dpMode == 0);
        }
    }
}
void faults() {
    BarometerSample out;
    Device a(Aht);
    AhtSensor ah(a);
    a.busy = 30;
    assert(ah.measure(out) == -ETIMEDOUT && !out.hasTemperature && a.waited == 140);
    a.busy = 0;
    a.status = 0;
    assert(ah.measure(out) == -EIO);
    Device b(Bmp085Part);
    Bmp085 bmp(b);
    b.id = 0x58;
    assert(bmp.measure(out) == -ENODEV);
    b.id = 0x55;
    b.trim = {};
    assert(bmp.measure(out) == -ENODATA);
    Device c(Bmp3Part);
    Bmp3 bmp3(c);
    c.id = 0x55;
    assert(bmp3.measure(out) == -ENODEV);
    c.fixture(0);
    c.status = 0;
    assert(bmp3.measure(out) == -EAGAIN && c.conversions == 0);
    c.status = 0x70;
    c.trim = {};
    assert(bmp3.measure(out) == -ENODATA);
    c.fixture(1);
    c.status = 0x10;
    assert(bmp3.measure(out) == -ETIMEDOUT);
    Device d(Dps);
    DpsSensor dps(d);
    d.id = 0x11;
    assert(dps.measure(out) == -ENODEV);
    d.fixture(1);
    d.status = 0xc0;
    assert(dps.measure(out) == -EAGAIN);
    d.status = 0xf0;
    d.failReg = 0;
    assert(dps.measure(out) == -EIO && d.dpMode == 0 && !out.hasTemperature);
}
void work() {
    mockKernel::timerTicks = 1;
    try {
        mockKernel::accessoryThread(nullptr, nullptr, nullptr);
    } catch (const mockKernel::NoJob &) {
    }
}
meshtastic_EnvironmentMetrics report() {
    auto e = environment(k_uptime_get());
    zt::telemetry::EnvironmentReadings readings;
    readings.accessory = e;
    meshtastic_Telemetry periodic{};
    if (!mt::stock::telemetry::environmentReport(periodic, 111, readings)) return {};
    uint8_t bytes[237];
    size_t n = 0;
    assert(mt::EnvironmentQuery::responseMetrics(periodic.variant.environment_metrics, 45, bytes,
                                                 sizeof(bytes), n, 777));
    meshtastic_Data data{};
    auto in = pb_istream_from_buffer(bytes, n);
    assert(pb_decode(&in, meshtastic_Data_fields, &data) && data.request_id == 45);
    meshtastic_Telemetry reply{};
    in = pb_istream_from_buffer(data.payload.bytes, data.payload.size);
    assert(pb_decode(&in, meshtastic_Telemetry_fields, &reply) && reply.time == 777);
    same(periodic.variant.environment_metrics, reply.variant.environment_metrics);
    return reply.variant.environment_metrics;
}
void integration() {
    work();
    for (auto &b : boards)
        assert(b.writes.empty());
    enableEnvironment(true);
#ifdef BMP3_PROFILE
    work();
    auto &b = boards[Bmp3Part];
    same(report(), donorSample(Bmp3Part, b.trim.data(), b.raw.data(), 0));
    assert(environment(k_uptime_get()).barometer.humidityOrigin == BarometerOrigin::Bmp3xx);
    b.failReg = 4;
    work();
    assert(!report().has_temperature);
    b.failReg = -1;
    for (unsigned i = 0; i < 4; ++i)
        work();
#else
    auto &a = boards[Aht], &p = boards[Bmp085Part], &d = boards[Dps];
    a.fixture(27);
    p.fixture(14);
    d.fixture(72);
    auto want = donorSample(Dps, d.trim.data(), d.raw.data(), 0);
    want.has_relative_humidity = true;
    want.relative_humidity = donorSample(Aht, nullptr, a.raw.data(), 0).relative_humidity;
    work();
    same(report(), want);
    const auto mixed = environment(k_uptime_get()).barometer;
    assert(mixed.temperatureOrigin == BarometerOrigin::Dps310 &&
           mixed.pressureOrigin == BarometerOrigin::Dps310 &&
           mixed.humidityOrigin == BarometerOrigin::Aht);
    d.status = 0xc0;
    work();
    same(report(), want); // one sensor waiting must retain the last fresh value
    d.absent = true;
    work();
    auto fallback = donorSample(Bmp085Part, p.trim.data(), p.raw.data(), p.secondTemp);
    fallback.has_relative_humidity = true;
    fallback.relative_humidity = want.relative_humidity;
    same(report(), fallback);
    p.absent = true;
    work();
    same(report(), donorSample(Aht, nullptr, a.raw.data(), 0));
    a.absent = true;
    work();
    assert(!report().has_temperature);
    a.absent = p.absent = d.absent = false;
    d.status = 0xf0;
    work();
    same(report(), want);
#endif
    assert(!environment(k_uptime_get() + 90001).barometer.hasTemperature);
    enableEnvironment(false);
    assert(!report().has_temperature);
    work();
#ifndef BMP3_PROFILE
    assert(boards[Dps].dpMode == 0);
#endif
    enableEnvironment(true);
    assert(!report().has_temperature);
    for (unsigned i = 0; i < 4; i++)
        work();
    assert(report().has_temperature);
}
} // namespace
int i2c_write_dt(const i2c_dt_spec *s, const uint8_t *p, size_t n) {
    return board(s->address).write(p, n);
}
int i2c_read_dt(const i2c_dt_spec *s, uint8_t *p, size_t n) {
    return board(s->address).read(p, n);
}
int i2c_write_read_dt(const i2c_dt_spec *s, const uint8_t *r, size_t n, uint8_t *p, size_t len) {
    assert(n == 1);
    return board(s->address).readReg(*r, p, len);
}
ssize_t settings_get_val_len(const char *) {
    return -ENOENT;
}
ssize_t settings_load_one(const char *, void *, size_t) {
    return -ENOENT;
}
int settings_save_one(const char *, const void *, size_t) {
    return -ENOTSUP;
}
#ifdef TEST_NATIVE_COMPENSATION
extern "C" void nativeSample(int, const uint8_t *, const uint8_t *, float *, float *);
void nativeDifference(Part part) {
    unsigned dTemp = 0, dPressure = 0;
    for (unsigned n = 0; n < 112; n++) {
        Device device(part);
        device.fixture(n);
        // Match the raw temperature in both algorithms to isolate compensation from acquisition.
        const uint16_t temp = uint16_t(device.raw[0]) << 8 | device.raw[1];
        auto expected = donorSample(part, device.trim.data(), device.raw.data(), temp);
        float nativeTemp, nativePressure;
        nativeSample(part, device.trim.data(), device.raw.data(), &nativeTemp, &nativePressure);
        dTemp += nativeTemp != expected.temperature;
        dPressure += nativePressure != expected.barometric_pressure;
    }
    assert(dPressure > 0);
    std::printf("Native %s versus stock: %u/112 pressure and %u/112 temperature field differences\n",
                part == Bmp085Part ? "BMP180" : "BMP388", dPressure, dTemp);
}
#endif
int main() {
#ifdef TEST_NATIVE_COMPENSATION
    nativeDifference(Bmp085Part);
    nativeDifference(Bmp3Part);
#endif
    differential<AhtSensor>(Aht);
    differential<Bmp085>(Bmp085Part);
    differential<DpsSensor>(Dps);
    differential<Bmp3>(Bmp3Part);
    faults();
    integration();
    puts("PASS 448 complete pinned sensor/driver comparisons, worker freshness, faults, disable, encoded "
         "phone responses");
}
