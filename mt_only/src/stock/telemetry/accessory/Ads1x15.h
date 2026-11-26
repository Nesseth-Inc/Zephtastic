// SPDX-License-Identifier: GPL-3.0-only
// Single-ended acquisition from Meshtastic ADS1X15Sensor (8e6a88d) and
// Adafruit ADS1X15 2.6.2. Only the owning worker touches the declared bus.
#pragma once
#include "SensorBus.h"
#include <meshtastic/telemetry.pb.h>
#include <float.h>

namespace mt::stock::telemetry::accessory {
struct AdcVoltages {
    uint8_t mask = 0;
    float volts[8]{};
    using Metrics = meshtastic_EnvironmentMetrics;
    static constexpr bool Metrics::*present[] = {
        &Metrics::has_adc_voltage_ch0, &Metrics::has_adc_voltage_ch1, &Metrics::has_adc_voltage_ch2,
        &Metrics::has_adc_voltage_ch3, &Metrics::has_adc_voltage_ch4, &Metrics::has_adc_voltage_ch5,
        &Metrics::has_adc_voltage_ch6, &Metrics::has_adc_voltage_ch7};
    static constexpr float Metrics::*value[] = {
        &Metrics::adc_voltage_ch0, &Metrics::adc_voltage_ch1, &Metrics::adc_voltage_ch2,
        &Metrics::adc_voltage_ch3, &Metrics::adc_voltage_ch4, &Metrics::adc_voltage_ch5,
        &Metrics::adc_voltage_ch6, &Metrics::adc_voltage_ch7};

    void apply(Metrics &m) const {
        for (unsigned i = 0; i < 8; ++i) {
            if (mask & (1u << i)) { m.*present[i] = true; m.*value[i] = volts[i]; }
        }
    }
    void from(const Metrics &m) {
        for (unsigned i = 0; i < 8; ++i) {
            const float v = m.*value[i];
            if (m.*present[i] && v >= 0 && v <= FLT_MAX) {
                volts[i] = v;
                mask |= uint8_t(1u << i);
            }
        }
    }
    static bool any(const Metrics &m) {
        for (auto field : present) if (m.*field) return true;
        return false;
    }
};

class Ads1x15 {
    Bus &bus;
    bool twelveBit;
    bool writeWord(uint8_t reg, uint16_t value) {
        const uint8_t bytes[] = {reg, uint8_t(value >> 8), uint8_t(value)};
        return bus.write(bytes, sizeof(bytes)) == 0;
    }
    bool readWord(uint8_t reg, uint16_t &value) {
        uint8_t bytes[2];
        // BusIO sends a STOP after selecting the register.
        if (bus.write(&reg, 1) || bus.read(bytes, sizeof(bytes))) return false;
        value = (uint16_t(bytes[0]) << 8) | bytes[1];
        return true;
    }
    bool conversion(unsigned channel, unsigned gain, uint16_t &raw) {
        // OS start, AINx-to-GND, PGA, single shot, 1600/128 SPS (1015/1115),
        // active-low non-latching conversion-ready comparator.
        uint16_t config = 0xc180 | (channel << 12) | (gain << 9);
        if (!writeWord(1, config) || !writeWord(3, 0x8000) || !writeWord(2, 0)) return false;
        for (unsigned poll = 0; poll < 20; ++poll) {
            uint16_t status;
            if (!readWord(1, status)) return false;
            if (status & 0x8000) {
                if (!readWord(0, raw)) return false;
                if (twelveBit) {
                    raw >>= 4;
                    if (raw & 0x0800) raw |= 0xf000;
                }
                return true;
            }
            bus.wait(1);
        }
        return false;
    }
    bool measure(unsigned channel, float &volts) {
        uint16_t raw;
        if (!conversion(channel, 0, raw)) return false;
        // Stock uses unsigned sign-extended results and a 32768 divisor on
        // both models, including the 12-bit ADS1015.
        double range = 6.144;
        if (raw < 21845) {
            unsigned gain;
            if (raw > 10922) { gain = 1; range = 4.096; }
            else if (raw > 5461) { gain = 2; range = 2.048; }
            else if (raw > 2730) { gain = 3; range = 1.024; }
            else if (raw > 1365) { gain = 4; range = 0.512; }
            else { gain = 5; range = 0.256; }
            if (!conversion(channel, gain, raw)) return false;
        }
        volts = float(raw) / 32768 * range;
        return true;
    }
public:
    Ads1x15(Bus &b, bool is1015) : bus(b), twelveBit(is1015) {}
    bool sample(bool alternate, AdcVoltages &out) {
        float result[4];
        for (unsigned i = 0; i < 4; ++i) if (!measure(i, result[i])) return false;
        const unsigned offset = alternate ? 4 : 0;
        for (unsigned i = 0; i < 4; ++i) out.volts[offset + i] = result[i];
        out.mask |= uint8_t(0x0f << offset);
        return true;
    }
};
}
