// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Arduino.h>
struct TwoWire {
    void begin(){}
    void beginTransmission(uint8_t);
    void write(uint8_t);
    uint8_t endTransmission(bool=true);
    uint8_t requestFrom(uint8_t,uint8_t);
    uint8_t available();
    uint8_t read();
    unsigned readBytes(uint8_t *, unsigned);
};
extern TwoWire Wire;
