// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Wire.h"
struct Adafruit_I2CDevice {
    uint8_t addr;
    TwoWire *wire;
    Adafruit_I2CDevice(uint8_t a, TwoWire *w) : addr(a), wire(w) {}
    bool begin() { return true; }
    bool write(const uint8_t *p, size_t n) { return wire->write(addr, p, n); }
    bool read(uint8_t *p, size_t n) { return wire->read(addr, p, n); }
};
