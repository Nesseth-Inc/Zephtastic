// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
struct i2c_dt_spec { uint8_t address; };
#define I2C_DT_SPEC_GET(n) {DT_REG_ADDR(n)}
int i2c_write_dt(const i2c_dt_spec *, const uint8_t *, size_t);
int i2c_read_dt(const i2c_dt_spec *, uint8_t *, size_t);
int i2c_write_read_dt(const i2c_dt_spec *, const uint8_t *, size_t, uint8_t *, size_t);
