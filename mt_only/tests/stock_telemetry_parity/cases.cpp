// SPDX-License-Identifier: GPL-3.0-only
#ifdef NDEBUG
#error Telemetry qualification requires active assertions
#endif
#include "endpoint_fixture.h"
#include "ModuleController.h"
#include "EnvironmentQuery.h"
#include <cstdio>

uint32_t stockInterval(unsigned, uint32_t, uint32_t, unsigned, float, int8_t);

namespace reference {
// Simulate measured inputs. Reports, admission decisions, and utilization
// thresholds are compiled from the checked upstream source.
uint32_t utc, uptime;
uint32_t getTime() { return utc; }
struct Time { static uint32_t getUptimeSecs() { return uptime; } };
struct AirTime {
    float tx, channel;
    bool allowed;
    float utilizationTXPercent() const { return tx; }
    float channelUtilizationPercent() const { return channel; }
    bool isTxAllowedAirUtil() const { return allowed; }
    bool isTxAllowedChannelUtil(bool);
    struct Held { explicit Held(AirTime*) {} };
    struct Window {
        const AirTime &owner;
        float channelUtilizationPercent(const Held&) const { return owner.channel; }
    } w{*this};
#include "channel_limits.inc"
} meter, *airTime = &meter;
struct Power {
    bool present, charging;
    int percent, mv;
    bool getHasBattery() const { return present; }
    bool getIsCharging() const { return charging; }
    int getBatteryChargePercent() const { return percent; }
    int getBatteryVoltageMv() const { return mv; }
} power, *powerStatus = &power;

struct DeviceTelemetryModule { meshtastic_Telemetry getDeviceTelemetry(); };
struct TelemetrySensor {
    meshtastic_EnvironmentMetrics values{};
    bool valid = false;
    bool getMetrics(meshtastic_Telemetry *out) const {
        out->variant.environment_metrics = values;
        return valid;
    }
};
struct EnvironmentTelemetryModule {
    std::vector<TelemetrySensor*> sensors;
    bool getEnvironmentTelemetry(meshtastic_Telemetry*);
};
struct Admission {
    struct { meshtastic_Config_DeviceConfig device{}; } config;
    struct { meshtastic_ModuleConfig_TelemetryConfig telemetry{}; } moduleConfig;
#include "role.inc"
#include "admission.inc"
};
// Only the synthetic environmental sensor is fitted in the donor harness.
#define LOG_WARN(...) ((void)0)
#define T1000X_SENSOR_EN 1
#include "reports.inc"
#undef T1000X_SENSOR_EN
#undef LOG_WARN
} // namespace reference

struct Counts {
    unsigned intervals = 0, decisions = 0, schedules = 0, wire = 0;
};

static constexpr unsigned roles[] = {0, 1, 2, 5, 6, 7, 8, 9, 10, 11, 12};
static void configure(Fixture &f, unsigned role, unsigned region) {
    f.s.stop();
    auto cfg = f.s.config;
    cfg.setTxEnabled(true);
    auto request = cfg.radioRequest();
    request.regionCode = region;
    request.usePreset = true;
    request.preset = region == 29 ? 10 : 0;
    request.channel = region == 1 ? 20 : 0;
    assert(cfg.setRadio(request) && cfg.setDeviceRole(role, 0) && f.s.apply(cfg));
}

static void intervals(Counts &counts) {
    auto f = std::make_unique<Fixture>();
    for (unsigned region : {1u, 29u}) {
        for (auto role : roles) {
            configure(*f, role, region);
            mt::ModuleController modules;
            const auto *policy = mt::radio::RegionPolicy::lookup(region);
            mt::LoRaPresets::Profile radio{};
            mt::USChannelSelection channel{};
            assert(policy && policy->profile && f->s.config.resolveRadio(radio, channel));
            unsigned seen = 1;
            for (unsigned online : {1u, 40u, 41u, 60u, 65u}) {
                while (seen < online) {
                    modules.observe(1000 + seen, OURS, 0, 1000, true, 1);
                    ++seen;
                }
                for (uint32_t seconds : {0u, 60u, 3600u}) {
                    uint32_t actual = 0;
                    assert(modules.telemetryInterval(f->s, seconds, 1000, actual));
                    assert(actual == stockInterval(role, seconds, online, radio.spreadingFactor,
                                                   radio.bandwidthHz / 1000.f, policy->profile->telemetryThrottle));
                    ++counts.intervals;
                }
            }
        }
    }
}

static void schedules(Counts &counts) {
    // Exercise the actual recurring device and environment branches. A late
    // router with a default interval must not queue again at one hour. A
    // TAK tracker must remain eligible at its configured interval under load.
    for (unsigned role : {0u, 2u, 10u, 11u}) {
        for (bool environment : {false, true}) {
            auto f = std::make_unique<Fixture>();
            configure(*f, role, 1);
            assert(f->s.start(false, 0) && f->s.armEndpoint(true));
            mt::ModuleController modules;
            mt::ModuleSettings settings;
            meshtastic_ModuleConfig config{};
            config.which_payload_variant = meshtastic_ModuleConfig_telemetry_tag;
            auto &t = config.payload_variant.telemetry;
            const unsigned seconds = role == 10 || role == 0 ? 120 : 0;
            t.device_telemetry_enabled = !environment;
            t.environment_measurement_enabled = environment;
            t.device_update_interval = t.environment_update_interval = seconds;
            assert(settings.set(config));
            zt::telemetry::EnvironmentReadings sample;
            sample.ambientCelsius = 0.f;
            f->s.measuredEnvironment(sample);
            auto poll = [&](uint32_t now, bool expected) {
                f->channelNow = now;
                // Refresh observations exactly as if peers remained online over a long interval.
                for (unsigned id = 1000; id < 1059; ++id)
                    modules.observe(id, OURS, 0, now, true, 1);
                unsigned queued = 0;
                modules.pollTimed(f->s, settings, true, now, now + 7, 1770000000,
                    [&](const uint8_t *p, size_t n, uint32_t id, bool idle) {
                        meshtastic_Data d{};
                        auto stream = pb_istream_from_buffer(p, n);
                        assert(id && pb_decode(&stream, meshtastic_Data_fields, &d));
                        if (idle || d.portnum != meshtastic_PortNum_TELEMETRY_APP) return;
                        meshtastic_Telemetry report{};
                        stream = pb_istream_from_buffer(d.payload.bytes, d.payload.size);
                        assert(pb_decode(&stream, meshtastic_Telemetry_fields, &report));
                        queued += report.which_variant == (environment ? meshtastic_Telemetry_environment_metrics_tag
                                                                       : meshtastic_Telemetry_device_metrics_tag);
                    });
                assert(queued == unsigned(expected) && f->s.endpointControls() == queued && !f->radio.sends);
                if (queued) {
                    f->s.stop();
                    assert(f->s.start(false, now) && f->s.armEndpoint(true));
                }
                ++counts.schedules;
            };
            mt::LoRaPresets::Profile radio{};
            mt::USChannelSelection selection{};
            assert(f->s.config.resolveRadio(radio, selection));
            const auto interval = stockInterval(role, seconds, 60, radio.spreadingFactor, radio.bandwidthHz / 1000.f, 1);
            const uint32_t first = environment ? 55000 : (role==meshtastic_Config_DeviceConfig_Role_TRACKER||role==meshtastic_Config_DeviceConfig_Role_TAK_TRACKER?75000:90000);
            poll(0, false);
            poll(first, true);
            if (interval > 3600000) poll(first + 3600000, false);
            // Coarse polling deliberately lands on the exact stock deadline after the last poll.
            poll(first + interval, true);
        }
    }
}

static void admission(Counts &counts) {
    reference::Admission source;
    source.moduleConfig.telemetry.device_telemetry_enabled = true;
    for (auto role : roles) {
        source.config.device.role = static_cast<meshtastic_Config_DeviceConfig_Role>(role);
        for (float load : {0.f, 24.99f, 25.f, 25.01f, 39.99f, 40.f, 40.01f, 100.f}) {
            reference::meter.channel = load;
            for (bool allowed : {false, true}) {
                reference::meter.allowed = allowed;
                assert(mt::stock::telemetry::deviceMeshAllowed(role, load, allowed) == source.device());
                assert(mt::stock::telemetry::environmentMeshAllowed(role, load, allowed) == source.environment());
                counts.decisions += 2;
            }
        }
    }
}

static void checkWire(const uint8_t *bytes, size_t size, uint32_t request, const meshtastic_Telemetry &expected) {
    meshtastic_Data data{};
    auto in = pb_istream_from_buffer(bytes, size);
    assert(pb_decode(&in, meshtastic_Data_fields, &data) && !in.bytes_left);
    assert(data.portnum == meshtastic_PortNum_TELEMETRY_APP && !data.want_response && data.request_id == request);
    uint8_t encoded[233]{};
    auto out = pb_ostream_from_buffer(encoded, sizeof(encoded));
    assert(pb_encode(&out, meshtastic_Telemetry_fields, &expected));
    assert(data.payload.size == out.bytes_written && !memcmp(data.payload.bytes, encoded, data.payload.size));
}

static void reports(Counts &counts) {
    reference::DeviceTelemetryModule device;
    for (auto clock : {0u, 1770000000u}) {
        reference::utc = clock;
        for (auto age : {0u, 123456u}) {
            reference::uptime = age;
            for (bool onUsb : {false, true}) {
                for (float load : {0.f, 39.5f, 100.f}) {
                    reference::meter.channel = load;
                    reference::meter.tx = load / 2;
                    reference::power = {true, onUsb, 63, 3842};
                    mt::HardwareSnapshot sample;
                    sample.cellMilliVolts = 3842;
                    sample.cellPercent = 63;
                    sample.externalPower = onUsb;
                    const auto expected = device.getDeviceTelemetry();
                    for (auto request : {0u, 0x3456u}) {
                        uint8_t buf[239]{};
                        size_t n = 0;
                        assert(request ? mt::TelemetryQuery::uptimeResponse(age, request, load / 2, load, buf,
                                                                           sizeof(buf), n, sample, clock)
                                       : mt::TelemetryAnnouncement::encode(age, load / 2, load, buf, sizeof(buf),
                                                                          n, sample, clock));
                        checkWire(buf, n, request, expected);
                        ++counts.wire;
                    }
                }
            }
        }
        for (unsigned mask = 0; mask < 8; ++mask) {
            reference::TelemetrySensor sensor;
            zt::telemetry::EnvironmentReadings sample;
            if (mask & 1) {
                sensor.values.has_temperature = true;
                sensor.values.temperature = 0;
                sample.ambientCelsius = 0.f;
            }
            if (mask & 2) {
                sensor.values.has_relative_humidity = true;
                sensor.values.relative_humidity = 56.25f;
                sample.humidityPercent = 56.25f;
            }
            if (mask & 4) {
                sensor.values.has_barometric_pressure = true;
                sensor.values.barometric_pressure = 1001.5f;
                sample.pressureHpa = 1001.5f;
            }
            sensor.valid = mask != 0;
            reference::EnvironmentTelemetryModule env{{&sensor}};
            meshtastic_Telemetry expected{};
            const bool available = env.getEnvironmentTelemetry(&expected);
            for (auto request : {0u, 0x3456u}) {
                uint8_t buf[239]{};
                size_t n = 0;
                const bool emitted = request ? mt::EnvironmentQuery::responseMetrics(sensor.values, request, buf,
                                                                                     sizeof(buf), n, clock)
                                             : mt::EnvironmentAnnouncement::encode(sample, buf, sizeof(buf), n, clock);
                assert(emitted == available);
                if (emitted) checkWire(buf, n, request, expected);
                else assert(!n);
                ++counts.wire;
            }
        }
    }
}

int main() {
    Counts result;
    intervals(result);
    schedules(result);
    admission(result);
    reports(result);
    std::printf("{\"intervals\":%u,\"admissions\":%u,\"scheduled_packets\":%u,\"wire_reports\":%u}\n",
                result.intervals, result.decisions, result.schedules, result.wire);
}
