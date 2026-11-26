// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Arduino.h"
struct TwoWire {
    void begin(){}
    void beginTransmission(uint8_t);
    void write(uint8_t);
    void endTransmission();
    void requestFrom(uint8_t,unsigned);
    uint8_t read();
};
extern TwoWire Wire;
