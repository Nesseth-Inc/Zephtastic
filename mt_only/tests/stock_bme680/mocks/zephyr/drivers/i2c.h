#pragma once
#include <zephyr/device.h>
struct i2c_dt_spec {};
#define I2C_DT_SPEC_GET(n) {}
extern uint8_t gasFlags,waitRegister,waitValue;
extern int busError;
inline int i2c_reg_write_byte_dt(const i2c_dt_spec*,uint8_t reg,uint8_t val){waitRegister=reg;waitValue=val;return busError;}
inline int i2c_reg_read_byte_dt(const i2c_dt_spec*,uint8_t reg,uint8_t *v){if(reg!=0x2b)return -EINVAL;*v=gasFlags;return busError;}
