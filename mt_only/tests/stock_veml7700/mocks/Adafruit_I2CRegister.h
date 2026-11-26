// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cassert>
#include <Adafruit_I2CDevice.h>
constexpr unsigned LSBFIRST = 0;
struct Adafruit_I2CRegister {
    Adafruit_I2CDevice *device;
    uint8_t address;
    Adafruit_I2CRegister(Adafruit_I2CDevice *d, uint8_t reg, unsigned n, unsigned order) : device(d), address(reg) {
        assert(n == 2 && order == LSBFIRST);
    }
    uint16_t read() {
        uint8_t bytes[2];
        assert(device->write(&address, 1) && device->read(bytes, 2));
        return uint16_t(bytes[1]) << 8 | bytes[0];
    }
    void write(unsigned v) {
        const uint8_t bytes[] = {address, uint8_t(v), uint8_t(v >> 8)};
        assert(device->write(bytes, sizeof(bytes)));
    }
};
struct Adafruit_I2CRegisterBits {
    Adafruit_I2CRegister *reg;
    unsigned width, shift;
    Adafruit_I2CRegisterBits(Adafruit_I2CRegister *r, unsigned bits, unsigned s) : reg(r), width(bits), shift(s) {}
    unsigned read() { return (reg->read() >> shift) & ((1u << width)-1); }
    void write(unsigned v) { reg->write((reg->read() & ~(((1u << width)-1) << shift)) | (v << shift)); }
};
