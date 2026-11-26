// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
struct i2c_dt_spec {int addr;};
#define I2C_DT_SPEC_GET(node) {0x42}
bool i2c_is_ready_dt(const i2c_dt_spec *);
int i2c_write_dt(const i2c_dt_spec *,const uint8_t *,size_t);
int i2c_read_dt(const i2c_dt_spec *,uint8_t *,size_t);
