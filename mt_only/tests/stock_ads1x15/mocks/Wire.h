// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
struct TwoWire {
    virtual bool write(uint8_t, const uint8_t *, size_t) = 0;
    virtual bool read(uint8_t, uint8_t *, size_t) = 0;
};
extern TwoWire &Wire;
