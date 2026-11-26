// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Arduino.h"
struct TwoWire {
    void begin() {}
    void beginTransmission(uint8_t);
    size_t write(uint8_t);
    uint8_t endTransmission(bool stop=true);
    uint8_t requestFrom(uint8_t,size_t);
    int available();
    int read();
};
extern TwoWire Wire;
