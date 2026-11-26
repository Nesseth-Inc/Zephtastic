// SPDX-License-Identifier: GPL-3.0-only
#include "EnvironmentQuery.h"
#include "stock/telemetry/ZephyrShtAdmin.h"
#include "oracle.h"
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace mt::stock::telemetry::accessory;
#ifdef TEST_NATIVE_SHT
extern "C" void nativeSample(unsigned, uint16_t, uint16_t, float *, float *);
#endif
namespace {
using M = ShtModel;
uint8_t frameCrc(const uint8_t *b, unsigned n, uint8_t initial) {
    for (unsigned i = 0; i < n; i++) {
        initial ^= b[i];
        for (int j = 0; j < 8; j++)
            initial = (initial & 128) ? (initial << 1) ^ 0x31 : initial << 1;
    }
    return initial;
}
struct Channel : Bus {
    M actual = M::None;
    uint16_t t = 19508, h = 31051, cmd = 0;
    bool badSerial = false, badT = false, badH = false, invertStatus = false, wrongId = false, codec = false;
    int manufacturer = 0, die = -1;
    uint64_t ms = 0;
    std::vector<std::array<unsigned, 2>> dataCommands;
    int write(const uint8_t *b, size_t n) override {
        assert(n == 1 || n == 2);
        cmd = n == 2 ? (b[0] << 8 | b[1]) : b[0];
        const bool ok = actual == M::Sht2   ? (cmd == 0xf3 || cmd == 0xf5 || cmd == 0xfa0f || cmd == 0xfcc9)
                        : actual == M::Sht4 ? (cmd == 0xfd || cmd == 0xf6 || cmd == 0xe0)
                        : actual == M::ShtC ? cmd == 0x7866
                        : actual == M::Sht3 || actual == M::Sht3Alt
                            ? (cmd == 0x2400 || cmd == 0x240b || cmd == 0x2416)
                            : false;
        if (ok && cmd != 0xfa0f && cmd != 0xfcc9) dataCommands.push_back({cmd, unsigned(ms)});
        return ok ? 0 : -EIO;
    }
    int read(uint8_t *out, size_t n) override {
        if (cmd == 0xfa0f) {
            assert(n == 8);
            for (unsigned i = 0; i < 8; i += 2) {
                out[i] = 42 + i;
                out[i + 1] = frameCrc(out + i, 1, 0);
            }
            if (badSerial) out[1] ^= 1;
            return 0;
        }
        if (cmd == 0xfcc9) {
            assert(n == 6);
            for (unsigned i = 0; i < 6; i += 3) {
                out[i] = 11 + i;
                out[i + 1] = 75;
                out[i + 2] = frameCrc(out + i, 2, 0);
            }
            return 0;
        }
        assert(n == 3 || n == 6);
        const bool split = actual == M::Sht2;
        auto word = [&](uint8_t *b, uint16_t v, bool hum) {
            if (split) v = (v & 0xfffc) | (hum != invertStatus ? 2 : 0);
            b[0] = v >> 8;
            b[1] = v;
            b[2] = frameCrc(b, 2, split ? 0 : 0xff);
            if (hum ? badH : badT) b[2] ^= 1;
        };
        if (n == 3) {
            assert(split);
            const bool humidity = cmd == 0xf5;
            word(out, humidity ? h : t, humidity);
        } else {
            assert(!split);
            word(out, t, false);
            word(out + 3, h, true);
        }
        return 0;
    }
    int readReg(uint8_t r, uint8_t *v, size_t n) override {
        if (r == 0x7e && wrongId) {
            assert(n == 2);
            v[0] = 0x54;
            v[1] = 0x49;
            return 0;
        }
        if ((r == 0xfe && manufacturer) || (r == 0xff && die >= 0)) {
            assert(n == 2);
            const auto id = r == 0xfe ? manufacturer : die;
            v[0] = id >> 8;
            v[1] = id;
            return 0;
        }
        if (codec && (r == 0x3d || r == 0x3e)) {
            assert(n == 1);
            *v = r == 0x3d ? 0x72 : 0x10;
            return 0;
        }
        return -EIO;
    }
    void wait(unsigned n) override { ms += n; }
    uint64_t now() override { return ms; }
    uint32_t utc() override { return 0; }
    int load(const char *, void *, size_t) override { return -ENOENT; }
    int save(const char *, const void *, size_t) override { return -ENOTSUP; }
};
struct Fixture {
    Channel p40, p44, p45, p70;
    Channel &at(uint8_t a) {
        if (a == 0x40) return p40;
        if (a == 0x44) return p44;
        if (a == 0x45) return p45;
        assert(a == 0x70);
        return p70;
    }
    void one(M model) {
        int addr = model == M::Sht2 ? 0x40 : model == M::Sht3Alt ? 0x45 : model == M::ShtC ? 0x70 : 0x44;
        at(addr).actual = model;
    }
    StockSht stock() { return {&p44, &p40, &p45, &p70}; }
};
Fixture native, board;
SHTSensor::SHTAccuracy level(unsigned x) {
    return x == 0   ? SHTSensor::SHT_ACCURACY_LOW
           : x == 1 ? SHTSensor::SHT_ACCURACY_MEDIUM
                    : SHTSensor::SHT_ACCURACY_HIGH;
}
void exact(M m) {
    Fixture f;
    f.one(m);
    native = {};
    native.one(m);
    auto owner = f.stock();
    SHTXXSensor donor;
    ScanI2C::FoundDevice found{};
    assert(donor.initDevice(&Wire, &found));
    int addr = m == M::Sht2 ? 0x40 : m == M::Sht3Alt ? 0x45 : m == M::ShtC ? 0x70 : 0x44;
    auto &stockChip = native.at(addr), &chip = f.at(addr);
    unsigned tMismatch = 0, hMismatch = 0;
    for (unsigned n = 0; n < 96; n++) {
        chip.t = stockChip.t = n == 95 ? 65535 : n * 607;
        chip.h = stockChip.h = n == 95 ? 65535 : n * 668;
        unsigned accuracy = n % 3;
        assert(donor.setAccuracy(level(accuracy)) == StockSht::adjustable(m));
        meshtastic_Telemetry expected{};
        assert(donor.getMetrics(&expected));
        ShtSample got;
        assert(owner.measure(accuracy, got) == 0 && owner.model() == m && got.present);
        const auto &v = expected.variant.environment_metrics;
        assert(v.has_temperature && v.has_relative_humidity && got.temperature == v.temperature &&
               got.humidity == v.relative_humidity);
#ifdef TEST_NATIVE_SHT
        if (m != M::Sht2) {
            float temp, h;
            nativeSample(unsigned(m), chip.t, chip.h, &temp, &h);
            tMismatch += temp != got.temperature;
            hMismatch += h != got.humidity;
        }
#endif
        const auto &x = chip.dataCommands.back(), &y = stockChip.dataCommands.back();
        assert(x[0] == y[0]); // last data command and completed conversion wait are stock-selected
        assert(chip.ms - x[1] == stockChip.ms - y[1]);
    }
#ifdef TEST_NATIVE_SHT
    if (m != M::Sht2) {
        assert(tMismatch > 0 && hMismatch > 0);
        std::printf("Native model %u / 96: %u temperature and %u humidity wire differences\n", unsigned(m),
                    tMismatch, hMismatch);
    }
#endif
    chip.badH = stockChip.badH = true;
    ShtSample bad;
    meshtastic_Telemetry no{};
    assert(owner.measure(2, bad) == -EBADMSG && !bad.present && !donor.getMetrics(&no));
}
void priorities() {
    Fixture f;
    f.p40.actual = M::Sht2;
    f.p44.actual = M::Sht4;
    f.p45.actual = M::Sht3Alt;
    f.p70.actual = M::ShtC;
    auto owner = f.stock();
    ShtSample v;
    assert(!owner.measure(2, v) && owner.model() == M::Sht4);
    f.p44.actual = M::None;
    v = {};
    assert(owner.measure(2, v) != 0 && !v.present); // no reselection on a transient failure
    owner.stop();
    assert(!owner.measure(2, v) && owner.model() == M::Sht2);
    f.p40.badSerial = true;
    owner.stop();
    assert(!owner.measure(2, v) && owner.model() == M::Sht3Alt);
    f.p45.wrongId = true;
    owner.stop();
    assert(!owner.measure(2, v) && owner.model() == M::ShtC);
    f.p70.actual = M::None;
    f.p44.actual = M::Sht3;
    owner.stop();
    assert(!owner.measure(2, v) && owner.model() == M::Sht3);
    f.p44.actual = M::None;
    f.p40.badSerial = false;
    for (unsigned other = 0; other < 3; other++) {
        f.p40.manufacturer = other == 0 ? 0x5449 : other == 1 ? 0x190f : 0;
        f.p40.die = 0;
        f.p40.codec = other == 2;
        owner.stop();
        assert(owner.measure(2, v) == -ENODEV);
    }
    f.p40.manufacturer = 0;
    f.p40.codec = false;
    f.p40.invertStatus = true;
    owner.stop();
    assert(owner.measure(2, v) == -ENODEV);
    native = {};
    native.p40.actual = M::Sht2;
    assert(detectSHT21SerialNumber(&Wire, 0x40));
    native.p40.badSerial = true;
    assert(!detectSHT21SerialNumber(&Wire, 0x40));
    f = {};
    f.p45.actual = M::Sht4;
    owner = f.stock();
    assert(owner.measure(2, v) == -ENODEV);
}
void work() {
    mockKernel::timerTicks = 1;
    try {
        mockKernel::accessoryThread(nullptr, nullptr, nullptr);
    } catch (const mockKernel::NoJob &) {
    }
}
meshtastic_EnvironmentMetrics report() {
    zt::telemetry::EnvironmentReadings read;
    read.accessory = environment(k_uptime_get());
    meshtastic_Telemetry m{};
    if (!mt::stock::telemetry::environmentReport(m, 80, read)) return {};
    uint8_t bytes[237];
    size_t n = 0;
    assert(mt::EnvironmentQuery::responseMetrics(m.variant.environment_metrics, 55, bytes, sizeof(bytes), n,
                                                 987));
    meshtastic_Data d{};
    auto in = pb_istream_from_buffer(bytes, n);
    assert(pb_decode(&in, meshtastic_Data_fields, &d) && d.request_id == 55);
    meshtastic_Telemetry r{};
    in = pb_istream_from_buffer(d.payload.bytes, d.payload.size);
    assert(pb_decode(&in, meshtastic_Telemetry_fields, &r) && r.time == 987);
    const auto &a = m.variant.environment_metrics, &b = r.variant.environment_metrics;
    assert(a.has_temperature == b.has_temperature && a.temperature == b.temperature &&
           a.has_relative_humidity == b.has_relative_humidity && a.relative_humidity == b.relative_humidity);
    return b;
}
void integration() {
    board.one(M::Sht4);
    work();
    assert(board.p44.dataCommands.empty());
    meshtastic_SensorConfig q{};
    q.has_shtxx_config = true;
    q.shtxx_config.has_set_accuracy = true;
    auto set = [&](unsigned v) {
        q.shtxx_config.set_accuracy = v;
        return mt::stock::telemetry::setShtAccuracy(q);
    };
    assert(set(2) == -ENODEV);
    enableEnvironment(true);
    work();
    assert(report().has_relative_humidity && report().has_temperature);
    for (unsigned n = 0; n < 3; n++) {
        assert(!set(n));
        work();
        const uint16_t commands[] = {0xe0, 0xf6, 0xfd};
        assert(board.p44.dataCommands.back()[0] == commands[n]);
    }
    assert(set(3) == -EINVAL);
    board.p44.h = 0;
    work();
    assert(!report().has_relative_humidity && report().has_temperature);
    board.p44.h = 65535;
    work();
    assert(!report().has_relative_humidity && report().has_temperature);
    board.p44.badT = true;
    work();
    assert(!report().has_temperature);
    board.p44.badT = false;
    board.p44.h = 32768;
    work();
    assert(report().has_temperature);
    assert(!environment(k_uptime_get() + 90001).sht.present);
    assert(!set(0));
    enableEnvironment(false);
    assert(!report().has_temperature && set(0) == -ENODEV);
    enableEnvironment(true);
    work();
    assert(board.p44.dataCommands.back()[0] == 0xe0);
    enableEnvironment(false);
    board = {};
    board.one(M::Sht2);
    enableEnvironment(true);
    work();
    assert(report().has_temperature && set(0) == -ENOTSUP);
}
} // namespace
TwoWire Wire;
void TwoWire::beginTransmission(uint8_t a) {
    addr = a;
    n = 0;
}
size_t TwoWire::write(uint8_t v) {
    assert(n < sizeof(tx));
    tx[n++] = v;
    return 1;
}
int TwoWire::endTransmission() {
    return native.at(addr).write(tx, n) ? 1 : 0;
}
uint8_t TwoWire::requestFrom(uint8_t a, uint8_t len) {
    received = native.at(a).read(rx, len) ? 0 : len;
    offset = 0;
    return received;
}
int TwoWire::available() {
    return received - offset;
}
int TwoWire::read() {
    assert(offset < received);
    return rx[offset++];
}
void delay(unsigned long ms) {
    for (auto addr : {0x40, 0x44, 0x45, 0x70})
        native.at(addr).ms += ms;
}
int i2c_write_dt(const i2c_dt_spec *spec, const uint8_t *p, size_t n) {
    return board.at(spec->address).write(p, n);
}
int i2c_read_dt(const i2c_dt_spec *spec, uint8_t *p, size_t n) {
    return board.at(spec->address).read(p, n);
}
int i2c_write_read_dt(const i2c_dt_spec *s, const uint8_t *cmd, size_t n, uint8_t *out, size_t len) {
    assert(n == 1);
    return board.at(s->address).readReg(*cmd, out, len);
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
int main() {
    for (M m : {M::Sht4, M::Sht2, M::Sht3, M::Sht3Alt, M::ShtC})
        exact(m);
    priorities();
    integration();
    puts("PASS 480 compiled SHTXX/Sensirion comparisons, stock scan order, CRC/status, repeatability, "
         "periodic/phone bytes, disable and faults");
}
