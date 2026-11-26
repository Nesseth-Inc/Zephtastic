#pragma once
#include <zephyr/device.h>
#include <stdint.h>
enum sensor_channel {SENSOR_CHAN_AMBIENT_TEMP,SENSOR_CHAN_HUMIDITY,SENSOR_CHAN_PRESS,
    SENSOR_CHAN_GAS_RES,SENSOR_CHAN_VOLTAGE,SENSOR_CHAN_CURRENT,SENSOR_CHAN_LIGHT,SENSOR_CHAN_AMBIENT_LIGHT,
    SENSOR_CHAN_IR,SENSOR_CHAN_DISTANCE,SENSOR_CHAN_PM_1_0_CF,SENSOR_CHAN_PM_2_5_CF,SENSOR_CHAN_PM_10_CF,
    SENSOR_CHAN_PM_1_0,SENSOR_CHAN_PM_2_5,SENSOR_CHAN_PM_10,SENSOR_CHAN_PM_0_3_COUNT,SENSOR_CHAN_PM_0_5_COUNT,
    SENSOR_CHAN_PM_1_0_COUNT,SENSOR_CHAN_PM_2_5_COUNT,SENSOR_CHAN_PM_5_COUNT,SENSOR_CHAN_PM_10_COUNT,
    SENSOR_CHAN_RED,SENSOR_CHAN_VOC,SENSOR_CHAN_CO2,SENSOR_CHAN_ALL,SENSOR_CHAN_PRIV_START,SENSOR_CHAN_MAX=100};
enum sensor_attribute {SENSOR_ATTR_RESOLUTION=10,SENSOR_ATTR_PRIV_START=20,SENSOR_ATTR_FAKE_MAX=100};
struct sensor_value {int32_t val1,val2;};
inline double sensor_value_to_double(const sensor_value *v){return v->val1+v->val2/1e6;}
inline float sensor_value_to_float(const sensor_value *v){return sensor_value_to_double(v);}
inline int sensor_value_from_float(sensor_value *v,float n){v->val1=int(n);v->val2=int((n-v->val1)*1000000);return 0;}
int sensor_sample_fetch(const device *);
int sensor_channel_get(const device *,sensor_channel,sensor_value *);
int sensor_attr_get(const device *,sensor_channel,sensor_attribute,sensor_value *);
int sensor_attr_set(const device *,sensor_channel,sensor_attribute,const sensor_value *);
