// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
struct TwoWire {
    bool readReg(uint8_t,uint8_t *,size_t);
    bool write(const uint8_t *,size_t);
};
extern TwoWire Wire;
struct Adafruit_I2CDevice {
    uint8_t addr;
    TwoWire *wire;
    Adafruit_I2CDevice(uint8_t address, TwoWire *bus) : addr(address), wire(bus) {}
    bool begin() { return addr == 0x5a; }
    bool write_then_read(const uint8_t *q,size_t n,uint8_t *r,size_t count) {
        return n==1 && wire->readReg(*q,r,count);
    }
    bool write(const uint8_t *p,size_t n) { return wire->write(p,n); }
};
