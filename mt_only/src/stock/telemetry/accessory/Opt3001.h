// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "SensorBus.h"

namespace mt::stock::telemetry::accessory {
// Meshtastic OPT3001Sensor and ClosedCube 1.1.2 use continuous, 100 ms,
// automatic range with a latched interrupt. Initialize the other bits to zero.
class Opt3001 {
    static constexpr uint16_t continuous = 0xc610;
    static constexpr uint16_t shutdown = 0xc010;
    Bus &bus;
    uint64_t readyAt = 0;
    bool running = false;
    bool needsShutdown = false;

    bool readWord(uint8_t reg, uint16_t &value) {
        uint8_t bytes[2];
        if (bus.write(&reg, 1) || bus.read(bytes, sizeof(bytes))) return false;
        value = (uint16_t(bytes[0]) << 8) | bytes[1];
        return true;
    }
    bool configure(uint16_t value) {
        const uint8_t bytes[] = {1, uint8_t(value >> 8), uint8_t(value)};
        return bus.write(bytes, sizeof(bytes)) == 0;
    }
    void start() {
        uint16_t word;
        if (!readWord(0x7e, word) || word != 0x5449 ||
            !readWord(0x7f, word) || word != 0x3001) return;
        needsShutdown = true;
        if (!configure(continuous)) { stop(); return; }
        readyAt = bus.now() + 100;
        running = true;
    }
public:
    explicit Opt3001(Bus &b) : bus(b) {}
    void stop() {
        running = false;
        if (needsShutdown && configure(shutdown)) needsShutdown = false;
    }
    bool sample(float &lux) {
        if (!running) {
            if (needsShutdown) { stop(); if (needsShutdown) return false; }
            start();
            return false;
        }
        if (bus.now() < readyAt) return false;
        uint16_t config, raw;
        if (!readWord(1, config) || (config & 0xfe1f) != continuous) { stop(); return false; }
        // Reading CONFIG clears CRF. Require a completed, unsaturated conversion.
        if (!(config & 0x0080) || (config & 0x0100)) return false;
        if (!readWord(0, raw)) { stop(); return false; }
        lux = float(((1u << (raw >> 12)) * (raw & 0x0fff)) * 0.01);
        return true;
    }
};
}
