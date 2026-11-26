// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct device {const void *config; void *data;};
struct i2c_dt_spec {int tag;};
enum sensor_channel {SENSOR_CHAN_ALL,SENSOR_CHAN_CURRENT,SENSOR_CHAN_VOLTAGE,SENSOR_CHAN_LIGHT};
struct sensor_value {int32_t val1,val2;};
struct api {
    int (*sample_fetch)(const struct device*,enum sensor_channel);
    int (*channel_get)(const struct device*,enum sensor_channel,struct sensor_value*);
};
#define DEVICE_API(kind,name) const struct api name
#define BUILD_ASSERT(...) _Static_assert(1,"fixture")
#define I2C_DT_SPEC_INST_GET(inst) {1}
#define DT_INST_FOREACH_STATUS_OKAY(emit) emit(0)
#define SENSOR_DEVICE_DT_INST_DEFINE(inst,init,pm,data,bus,lvl,prio,methods) \
    const struct device testDevice={bus,data}; \
    const struct api *testApi=methods; \
    int testInit(void){return init(&testDevice);}
#ifdef __cplusplus
extern "C" {
#endif
extern const struct device testDevice;
extern const struct api *testApi;
int testInit(void);
bool i2c_is_ready_dt(const struct i2c_dt_spec*);
int i2c_write_read_dt(const struct i2c_dt_spec*,const void*,size_t,void*,size_t);
int i2c_write_dt(const struct i2c_dt_spec*,const void*,size_t);
void k_msleep(int);
static inline uint16_t sys_get_be16(const uint8_t *data){return ((uint16_t)data[0]<<8)|data[1];}
static inline void sys_put_be16(uint16_t n,uint8_t *data){data[0]=n>>8;data[1]=n;}
#ifdef __cplusplus
}
#endif
