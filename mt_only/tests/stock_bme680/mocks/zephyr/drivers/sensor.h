#pragma once
#include <zephyr/device.h>
enum sensor_channel {SENSOR_CHAN_AMBIENT_TEMP,SENSOR_CHAN_HUMIDITY,SENSOR_CHAN_PRESS,SENSOR_CHAN_GAS_RES};
struct sensor_value {float value;};
extern float readings[4];extern unsigned fetches;
inline int sensor_sample_fetch(const device*){++fetches;return 0;}
inline int sensor_channel_get(const device*,sensor_channel c,sensor_value *v){v->value=readings[c];return 0;}
inline float sensor_value_to_float(const sensor_value *v){return v->value;}
