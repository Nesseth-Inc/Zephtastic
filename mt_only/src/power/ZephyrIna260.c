// SPDX-License-Identifier: GPL-3.0-only
// Adafruit_INA260 1.5.3, commit 1d14a8e. Device identity, reset and ADC scaling.
#define DT_DRV_COMPAT ti_ina260
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <errno.h>

struct ina260_sample {
    int32_t microamps;
    int32_t microvolts;
    bool current;
    bool voltage;
};

static int word(const struct i2c_dt_spec *bus,uint8_t reg,uint16_t *value)
{
    uint8_t data[2];
    int rc=i2c_write_read_dt(bus,&reg,1,data,sizeof(data));
    if(!rc)*value=sys_get_be16(data);
    return rc;
}

static int ina260_init(const struct device *dev)
{
    const struct i2c_dt_spec *bus=dev->config;
    uint16_t manufacturer,die,config;
    if(!i2c_is_ready_dt(bus))return -ENODEV;
    int rc=word(bus,0xfe,&manufacturer);
    if(rc)return rc;
    if(manufacturer!=0x5449)return -ENODEV;
    rc=word(bus,0xff,&die);if(rc)return rc;
    if((die>>4)!=0x227)return -ENODEV;
    rc=word(bus,0,&config);if(rc)return rc;
    uint8_t reset[3]={0};sys_put_be16(config|0x8000,reset+1);
    rc=i2c_write_dt(bus,reset,sizeof(reset));
    if(!rc)k_msleep(2);
    return rc;
}

static int ina260_fetch(const struct device *dev,enum sensor_channel chan)
{
    const struct i2c_dt_spec *bus=dev->config;
    struct ina260_sample *s=dev->data;
    if(chan!=SENSOR_CHAN_ALL&&chan!=SENSOR_CHAN_CURRENT&&chan!=SENSOR_CHAN_VOLTAGE)return -ENOTSUP;
    uint16_t value;
    int current=0,voltage=0;
    if(chan!=SENSOR_CHAN_VOLTAGE){
        current=word(bus,1,&value);s->current=!current;
        if(!current)s->microamps=(int16_t)value*1250;
    }
    if(chan!=SENSOR_CHAN_CURRENT){
        voltage=word(bus,2,&value);s->voltage=!voltage;
        if(!voltage)s->microvolts=(int32_t)value*1250;
    }
    if(chan==SENSOR_CHAN_CURRENT)return current;
    if(chan==SENSOR_CHAN_VOLTAGE)return voltage;
    return s->current||s->voltage?0:current;
}

static int ina260_get(const struct device *dev,enum sensor_channel chan,struct sensor_value *out)
{
    const struct ina260_sample *s=dev->data;
    if(chan!=SENSOR_CHAN_CURRENT&&chan!=SENSOR_CHAN_VOLTAGE)return -ENOTSUP;
    if(chan==SENSOR_CHAN_CURRENT?!s->current:!s->voltage)return -ENODATA;
    int32_t micro=chan==SENSOR_CHAN_CURRENT?s->microamps:s->microvolts;
    out->val1=micro/1000000;out->val2=micro%1000000;
    return 0;
}

static DEVICE_API(sensor, ina260_api)={.sample_fetch=ina260_fetch,.channel_get=ina260_get};
#define DEFINE_INA260(inst) \
    BUILD_ASSERT(DT_INST_REG_ADDR(inst)>=0x40 && DT_INST_REG_ADDR(inst)<=0x4f,"invalid INA260 address"); \
    BUILD_ASSERT(DT_INST_PROP(inst,zephyr_deferred_init), "INA260 must wait for an admitted measurement owner"); \
    static const struct i2c_dt_spec ina260_bus_##inst=I2C_DT_SPEC_INST_GET(inst); \
    static struct ina260_sample ina260_data_##inst; \
    SENSOR_DEVICE_DT_INST_DEFINE(inst,ina260_init,NULL,&ina260_data_##inst,&ina260_bus_##inst, \
                                 POST_KERNEL,CONFIG_SENSOR_INIT_PRIORITY,&ina260_api);
DT_INST_FOREACH_STATUS_OKAY(DEFINE_INA260)
