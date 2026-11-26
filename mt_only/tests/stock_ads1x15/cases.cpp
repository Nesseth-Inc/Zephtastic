// SPDX-License-Identifier: GPL-3.0-only
#include "EnvironmentQuery.h"
#include "stock/telemetry/accessory/Accessories.h"
#include "oracle.h"
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>

using namespace mt::stock::telemetry::accessory;
namespace {
struct Chip {
    uint8_t address, pointer = 0;
    uint16_t config = 0, wide[4]{}, narrow[4]{};
    unsigned checks = 0, faultAt = 0;
    bool stuck = false;
    std::vector<uint32_t> trace;
    explicit Chip(uint8_t a) : address(a) {}
    bool event(uint32_t value) {
        trace.push_back(value);
        return trace.size() != faultAt;
    }
    bool write(const uint8_t *bytes, size_t size) {
        assert(size == 1 || size == 3);
        if (size == 1) {
            if (!event(0x1000000 | bytes[0])) return false;
            pointer = bytes[0];
        } else {
            uint16_t value = uint16_t(bytes[1]) << 8 | bytes[2];
            if (!event(0x2000000 | (uint32_t(bytes[0]) << 16) | value)) return false;
            if (bytes[0] == 1) {
                config = value;
                checks = 0;
            } else {
                assert((bytes[0] == 3 && value == 0x8000) || (bytes[0] == 2 && value == 0));
            }
        }
        return true;
    }
    bool read(uint8_t *bytes, size_t size) {
        assert(size == 2);
        if (!event(0x3000000 | pointer)) return false;
        uint16_t value;
        if (pointer == 1) {
            value = (checks++ == 0 || stuck) ? config & 0x7fff : config;
        } else {
            assert(pointer == 0);
            unsigned channel = (config >> 12) & 3, gain = (config >> 9) & 7;
            value = gain ? narrow[channel] : wide[channel];
        }
        bytes[0] = value >> 8;
        bytes[1] = uint8_t(value);
        return true;
    }
};
struct WireModel : TwoWire {
    Chip *chip = nullptr;
    bool write(uint8_t a, const uint8_t *p, size_t n) override { assert(chip && a == chip->address); return chip->write(p,n); }
    bool read(uint8_t a, uint8_t *p, size_t n) override { assert(chip && a == chip->address); return chip->read(p,n); }
} wire;
struct Port : Bus {
    Chip &chip;
    uint64_t ms = 0;
    explicit Port(Chip &c) : chip(c) {}
    int write(const uint8_t *p, size_t n) override { return chip.write(p,n) ? 0 : -EIO; }
    int read(uint8_t *p, size_t n) override { return chip.read(p,n) ? 0 : -EIO; }
    int readReg(uint8_t,uint8_t *,size_t) override { assert(false); return -EIO; }
    void wait(unsigned n) override { ms += n; }
    uint64_t now() override { return ms; }
    uint32_t utc() override { return 0; }
    int load(const char *, void *,size_t) override { return -ENOENT; }
    int save(const char *,const void *,size_t) override { return -ENOTSUP; }
};
Chip primary(0x48), alternate(0x4b);
Chip &chipFor(const i2c_dt_spec *s) {
    assert(s->address == 0x48 || s->address == 0x4b);
    return s->address == 0x48 ? primary : alternate;
}
void fill(Chip &c, const std::array<uint16_t,4> &a, const std::array<uint16_t,4> &b) {
    for (unsigned i = 0; i < 4; ++i) { c.wide[i] = a[i]; c.narrow[i] = b[i]; }
}
template<class Oracle>
void compare(uint8_t address, bool twelveBit, const std::array<uint16_t,4> &wide, const std::array<uint16_t,4> &narrow) {
    Chip donor(address), candidate(address);
    fill(donor, wide, narrow);
    fill(candidate, wide, narrow);
    const bool alt = address != 0x48;
    Oracle oracle;
    oracle._deviceType = alt ? ScanI2C::DeviceType::ADS1X15_ALT : ScanI2C::DeviceType::ADS1X15;
    wire.chip = &donor;
    assert(oracle.ads1x15.begin(address, &wire));
    meshtastic_Telemetry expected{};
    assert(oracle.getMetrics(&expected));
    wire.chip = nullptr;
    Port bus(candidate);
    Ads1x15 adc(bus,twelveBit);
    AdcVoltages measured;
    assert(adc.sample(alt,measured));
    meshtastic_EnvironmentMetrics observed{};
    measured.apply(observed);
    assert(candidate.trace == donor.trace);
    for (unsigned i = 0; i < 8; ++i) {
        const auto &m = expected.variant.environment_metrics;
        assert(observed.*AdcVoltages::present[i] == m.*AdcVoltages::present[i]);
        assert(observed.*AdcVoltages::value[i] == m.*AdcVoltages::value[i]);
    }
}
void differential() {
    // Exercise both address groups under either donor build option, including
    // each strict threshold, both sign bits and nonzero low ADS1015 bits.
    const uint16_t boundaries[] = {0,1,1364,1365,1366,2729,2730,2731,5460,5461,5462,
                                  10921,10922,10923,21844,21845,21846,32767,32768,65535};
    for (uint8_t addr : {0x48,0x49,0x4a,0x4b}) {
        for (unsigned i = 0; i < 20; ++i) {
            std::array<uint16_t,4> a{},b{};
            for (unsigned ch = 0; ch < 4; ++ch) { a[ch] = boundaries[(i+ch)%20]; b[ch] = boundaries[(19-i+ch)%20]; }
            compare<oracle1115::ADS1X15Sensor>(addr,false,a,b);
            compare<oracle1015::ADS1X15Sensor>(addr,true,a,b);
        }
    }
    puts("PASS 160 stock ADS register-trace and eight-field differentials, both ADCs and four I2C addresses");
}
void failures() {
    Chip chip(0x48);
    Port bus(chip);
    Ads1x15 adc(bus,false);
    AdcVoltages out;
    assert(adc.sample(false,out));
    const size_t operations = chip.trace.size();
    for (size_t i = 1; i <= operations; ++i) {
        chip.trace.clear(); chip.faultAt = i;
        AdcVoltages previous;
        previous.mask = 0xf0;
        for (float &v : previous.volts) v = 5;
        assert(!adc.sample(false,previous) && previous.mask == 0xf0 && chip.trace.size() == i);
        for (float v : previous.volts) assert(v == 5);
    }
    chip.trace.clear(); chip.faultAt = 0; chip.stuck = true;
    const auto at = bus.now();
    assert(!adc.sample(false,out) && bus.now() - at == 20);
    chip.stuck = false;
    assert(adc.sample(false,out));
    puts("PASS NACK at each bus operation is atomic per four-channel group, conversion timeout and recovery");
}
void work() {
    mockKernel::timerTicks = 1;
    try { mockKernel::accessoryThread(nullptr,nullptr,nullptr); } catch (mockKernel::NoJob &) {}
}
void integration() {
    fill(primary,{0,1400,6000,22000},{17,5334,32456,1});
    fill(alternate,{0xfed0,0x5550,0x1000,0x7ff0},{0x111f,0x222a,0x333f,0xfedc});
    work();
    assert(primary.trace.empty() && alternate.trace.empty());
    enableEnvironment(true); work();
    auto snapshot = environment(k_uptime_get());
    assert(snapshot.adc.mask == 0xff);
    zt::telemetry::EnvironmentReadings input;
    input.accessory = snapshot;
    meshtastic_Telemetry ann{};
    assert(mt::stock::telemetry::environmentReport(ann,1234,input));
    uint8_t bytes[237];
    size_t n = 0;
    assert(mt::EnvironmentQuery::responseMetrics(ann.variant.environment_metrics,42,bytes,sizeof(bytes),n,1234));
    meshtastic_Data data{};
    auto in = pb_istream_from_buffer(bytes,n);
    assert(pb_decode(&in,meshtastic_Data_fields,&data) && data.request_id == 42);
    meshtastic_Telemetry reply{};
    in = pb_istream_from_buffer(data.payload.bytes,data.payload.size);
    assert(pb_decode(&in,meshtastic_Telemetry_fields,&reply) && reply.time == 1234);
    for (unsigned i = 0; i < 8; ++i) {
        assert(reply.variant.environment_metrics.*AdcVoltages::present[i]);
        assert(reply.variant.environment_metrics.*AdcVoltages::value[i] == snapshot.adc.volts[i]);
    }
    assert(!environment(k_uptime_get()+90001).adc.mask);
    alternate.stuck = true; work();
    assert(environment(k_uptime_get()).adc.mask == 0x0f);
    alternate.stuck = false; primary.stuck = true; work();
    assert(environment(k_uptime_get()).adc.mask == 0xf0);
    alternate.stuck = true; work();
    assert(!environment(k_uptime_get()).adc.mask);
    primary.stuck = alternate.stuck = false; work();
    assert(environment(k_uptime_get()).adc.mask == 0xff);
    enableEnvironment(false);
    const size_t transactions = primary.trace.size() + alternate.trace.size();
    work();
    assert(!environment(k_uptime_get()).adc.mask && primary.trace.size()+alternate.trace.size() == transactions);
    meshtastic_EnvironmentMetrics invalid{};
    invalid.has_adc_voltage_ch1 = true; invalid.adc_voltage_ch1 = std::numeric_limits<float>::quiet_NaN();
    assert(!mt::EnvironmentQuery::responseMetrics(invalid,42,bytes,sizeof(bytes),n));
    puts("PASS worker opt-in, independent failures, recovery, expiration, disable and eight ADC-only periodic/query wire fields");
}
}
TwoWire &Wire = wire;
int i2c_write_dt(const i2c_dt_spec *s,const uint8_t *p,size_t n) { return chipFor(s).write(p,n) ? 0 : -EIO; }
int i2c_read_dt(const i2c_dt_spec *s,uint8_t *p,size_t n) { return chipFor(s).read(p,n) ? 0 : -EIO; }
int i2c_write_read_dt(const i2c_dt_spec *,const uint8_t *,size_t,uint8_t *,size_t) { assert(false); return -EIO; }
ssize_t settings_get_val_len(const char *) { return -ENOENT; }
ssize_t settings_load_one(const char *,void *,size_t) { return -ENOENT; }
int settings_save_one(const char *,const void *,size_t) { return -ENOTSUP; }
int main() { differential(); failures(); integration(); }
