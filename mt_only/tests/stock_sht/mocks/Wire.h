#pragma once
#include <stddef.h>
#include <stdint.h>
struct TwoWire {
    uint8_t addr=0,tx[4]{},rx[8]{};
    unsigned n=0,received=0,offset=0;
    void beginTransmission(uint8_t a);
    size_t write(uint8_t v);
    int endTransmission();
    uint8_t requestFrom(uint8_t a,uint8_t len);
    int available();
    int read();
};
extern TwoWire Wire;
