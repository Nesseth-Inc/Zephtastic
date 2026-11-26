// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Arduino.h"
struct TwoWire {
    void beginTransmission(uint8_t);
    size_t write(const uint8_t *,size_t);
    size_t write(uint8_t);
    uint8_t endTransmission();
    uint8_t requestFrom(uint8_t,uint8_t,bool stop=true);
    int read();
    size_t available();
    void setTimeout(unsigned) {}
};
extern TwoWire Wire;
