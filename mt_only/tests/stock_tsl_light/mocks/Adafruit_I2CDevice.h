// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Wire.h>
struct Adafruit_I2CDevice {
    Adafruit_I2CDevice(uint8_t,TwoWire*){}
    bool begin(){return true;}
    bool write_then_read(const uint8_t *,size_t,uint8_t *,size_t);
    bool write(const uint8_t *,size_t);
};
