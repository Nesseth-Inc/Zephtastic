#pragma once
#include <stddef.h>
#include <stdint.h>
struct i2c_dt_spec {uint8_t address; void *bus=nullptr;};
int i2c_recover_bus(void *);
#define address_zt_optical 0x57
#define address_zt_scd30 0x61
#define address_zt_sen5x 0x69
#define address_zt_sen6x 0x6b
#define address_zt_as3935 3
#define address_zt_nau7802 0x2a
#define address_zt_radsens 0x66
#define address_zt_ltr390 0x53
#define address_zt_rainfall 0x1d
#define address_zt_lark 0x42
#define address_zt_rcwl9620 0x57
#define address_of(node) address_##node
#define I2C_DT_SPEC_GET(node) {address_of(node)}
int i2c_write_dt(const i2c_dt_spec *,const uint8_t *,size_t);
int i2c_read_dt(const i2c_dt_spec *,uint8_t *,size_t);
int i2c_write_read_dt(const i2c_dt_spec *,const uint8_t *,size_t,uint8_t *,size_t);
bool i2c_is_ready_dt(const i2c_dt_spec *);
int i2c_burst_read_dt(const i2c_dt_spec *,uint8_t,uint8_t *,size_t);
int i2c_reg_write_byte_dt(const i2c_dt_spec *,uint8_t,uint8_t);
