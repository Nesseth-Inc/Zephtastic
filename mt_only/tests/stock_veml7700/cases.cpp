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
using Trace = std::vector<std::array<uint64_t,3>>;
struct Chip {
    uint16_t regs[6] = {0xa031,0,0,7,0,0}; // Preserve reserved config and power-save mode bits.
    std::vector<uint16_t> als{1000};
    size_t index = 0;
    unsigned pointer = 0;
    int failRead = -1, failWrite = -1;
    Trace trace;
    void fixture(const std::vector<uint16_t> &raw, uint16_t white) {
        als = raw; index = 0; regs[5] = white; trace.clear();
    }
    bool put(const uint8_t *p, size_t n) {
        assert(p[0] < 6 && (n == 3 || n == 1)); pointer = p[0];
        if (n == 1) return true;
        if (int(pointer) == failWrite) return false;
        regs[pointer] = uint16_t(p[2]) << 8 | p[1]; return true;
    }
    bool get(unsigned reg, uint8_t *p, size_t n, uint64_t now) {
        assert(n == 2 && reg < 6);
        if (reg == 4 || reg == 5) {
            assert(!(regs[0] & 1));
            trace.push_back({reg, regs[0], now});
        }
        if (int(reg) == failRead) return false;
        uint16_t value = regs[reg];
        if (reg == 4) { assert(index < als.size()); value = als[index++]; }
        p[0] = uint8_t(value); p[1] = value >> 8; return true;
    }
};
uint64_t donorMs = 0;
Chip donor, board;
struct WireModel : TwoWire {
    bool write(uint8_t a,const uint8_t *p,size_t n) override { assert(a == 0x10); return donor.put(p,n); }
    bool read(uint8_t a,uint8_t *p,size_t n) override { assert(a == 0x10); return donor.get(donor.pointer,p,n,donorMs); }
} donorWire;
struct Port : Bus {
    Chip chip;
    uint64_t ms = 0;
    int write(const uint8_t *p,size_t n) override { return chip.put(p,n) ? 0 : -EIO; }
    int read(uint8_t *,size_t) override { assert(false); return -EIO; }
    int readReg(uint8_t r,uint8_t *p,size_t n) override { return chip.get(r,p,n,ms) ? 0 : -EIO; }
    void wait(unsigned n) override { assert(n == 5); ms += n; }
    uint64_t now() override { return ms; }
    uint32_t utc() override { return 0; }
    int load(const char *,void *,size_t) override { return -ENOENT; }
    int save(const char *,const void *,size_t) override { return -ENOTSUP; }
};
VEML7700Sensor oracle; // Its production driver owns long-lived BusIO registers.
void differential() {
    Port bus; Veml7700 sensor(bus);
    assert(oracle.initDevice(&donorWire, nullptr));
    std::vector<std::vector<uint16_t>> paths = {
        {101},{10000},{10001,10000},{65535,65535,65535},{100,101},
        {0,0,101},{99,90,80,101},{100,100,100,100,101},
        {20,20,20,20,20,101},{0,0,0,0,0,0,0},{100,100,100,100,100,100,65535}
    };
    for (const auto &path : paths) for (uint16_t white : {0,100,101,32767,32768,65535}) {
        bus.chip.fixture(path, white); donor.fixture(path, white);
        meshtastic_Telemetry expected{};
        assert(oracle.getMetrics(&expected));
        VemlLight got{};
        unsigned count = 0; int result;
        while ((result=sensor.poll(got)) == -EAGAIN) { ++bus.ms; assert(++count < 14000); }
        assert(!result && got.hasVisible && got.hasWhite);
        assert(got.visible == expected.variant.environment_metrics.lux && got.white == expected.variant.environment_metrics.white_lux);
        assert(bus.chip.trace == donor.trace && bus.chip.index == path.size());
        assert(bus.chip.regs[0] == donor.regs[0] && bus.chip.regs[1] == 20000 && bus.chip.regs[2] == 10000 && bus.chip.regs[3] == 6);
    }
    puts("PASS 66 pinned Meshtastic/Adafruit comparisons, exact gain, integration, flush and read times, dark/bright branches and signed white thresholds");
}
void work() {
    mockKernel::timerTicks = 1;
    try { mockKernel::accessoryThread(nullptr,nullptr,nullptr); } catch (const mockKernel::NoJob &) {}
}
meshtastic_EnvironmentMetrics query(const meshtastic_EnvironmentMetrics &input) {
    uint8_t bytes[237]; size_t n=0;
    if (!mt::EnvironmentQuery::responseMetrics(input,42,bytes,sizeof(bytes),n,1234)) return {};
    meshtastic_Data data{}; auto in=pb_istream_from_buffer(bytes,n);
    assert(pb_decode(&in,meshtastic_Data_fields,&data) && data.request_id == 42);
    meshtastic_Telemetry reply{}; in=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
    assert(pb_decode(&in,meshtastic_Telemetry_fields,&reply) && reply.time == 1234);
    return reply.variant.environment_metrics;
}
void integration() {
    work(); assert(board.trace.empty());
    enableEnvironment(true);
    board.fixture(std::vector<uint16_t>(200,102),0);
    for (unsigned i=0;i<12;++i) work();
    auto snapshot=environment(k_uptime_get());
    assert(snapshot.optical.hasVisible && snapshot.optical.hasWhite && snapshot.optical.white==0);
    zt::telemetry::EnvironmentReadings readings;
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS)
    readings.channels.record(mt::stock::telemetry::EnvironmentChannels::Lux, 87);
    auto generic=zt::telemetry::toMeshtasticEnvironmentMetrics(readings);
    assert(generic.has_lux && generic.lux==87 && !generic.has_white_lux);
    assert(query(generic).lux==87);
#endif
    readings.accessory = snapshot;
    readings.accessory.hasLight = true; readings.accessory.lux = 123; readings.accessory.uvIndex = 42;
    meshtastic_Telemetry ann{};
    assert(mt::stock::telemetry::environmentReport(ann,1111,readings));
    auto reply=query(ann.variant.environment_metrics);
    assert(reply.has_lux && reply.lux==snapshot.optical.visible && reply.has_white_lux && reply.white_lux==0);
    assert(reply.has_uv_lux && reply.uv_lux==42);
    for (int fault : {4,5}) {
        board.failRead = fault;
        for (unsigned i=0;i<10;++i) work();
        const auto measured = environment(k_uptime_get()).optical;
        assert(measured.hasVisible==(fault==5) && measured.hasWhite==(fault==4));
        meshtastic_EnvironmentMetrics report{}; measured.apply(report);
        auto partial=query(report); assert(partial.has_lux==report.has_lux && partial.has_white_lux==report.has_white_lux);
    }
    board.failRead=-1; board.failWrite=0;
    for(unsigned i=0;i<10;++i) work();
    assert(!environment(k_uptime_get()).optical.hasWhite && !environment(k_uptime_get()).optical.hasVisible);
    board.failWrite=-1;
    for(unsigned i=0;i<10;++i) work();
    assert(environment(k_uptime_get()).optical.hasVisible && !environment(k_uptime_get()+90001).optical.hasVisible);
    enableEnvironment(false); assert(!environment(k_uptime_get()).optical.hasVisible);
    work(); assert(board.regs[0]&1);
    enableEnvironment(true); work(); assert(!environment(k_uptime_get()).optical.hasVisible);
    for(unsigned i=0;i<10;++i) work();
    assert(environment(k_uptime_get()).optical.hasVisible);
    meshtastic_EnvironmentMetrics invalid{}; invalid.has_lux=invalid.has_white_lux=true;
    invalid.lux=std::numeric_limits<float>::quiet_NaN(); invalid.white_lux=0;
    reply=query(invalid); assert(!reply.has_lux && reply.has_white_lux && reply.white_lux==0);
    invalid.lux=0;invalid.white_lux=-1;
    reply=query(invalid);assert(reply.has_lux && reply.lux==0 && !reply.has_white_lux);
    invalid={}; invalid.has_uv_lux=true;invalid.uv_lux=17;
    reply=query(invalid);assert(reply.has_uv_lux && reply.uv_lux==17 && !reply.has_lux && !reply.has_white_lux);
    puts("PASS periodic/phone fields, LTR390 coexistence, independent read failures, expiry, shutdown/re-enable, zero values and nonfinite rejection");
}
}
unsigned long millis(){return donorMs;}
void delay(unsigned long ms){donorMs+=ms;}
TwoWire &Wire=donorWire;
int i2c_write_dt(const i2c_dt_spec *s,const uint8_t *p,size_t n){assert(s->address==0x10);return board.put(p,n)?0:-EIO;}
int i2c_read_dt(const i2c_dt_spec *,uint8_t *,size_t){assert(false);return -EIO;}
int i2c_write_read_dt(const i2c_dt_spec *s,const uint8_t *p,size_t n,uint8_t *out,size_t len){
    assert(s->address==0x10 && n==1);return board.get(*p,out,len,k_uptime_get())?0:-EIO;
}
ssize_t settings_get_val_len(const char *){return -ENOENT;}
ssize_t settings_load_one(const char *,void *,size_t){return -ENOENT;}
int settings_save_one(const char *,const void *,size_t){return -ENOTSUP;}
int main(){differential();integration();}
