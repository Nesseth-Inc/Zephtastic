#pragma once
#include <zephyr/device.h>
enum sensor_channel {SENSOR_CHAN_PM_1_0,SENSOR_CHAN_PM_2_5,SENSOR_CHAN_PM_10,SENSOR_CHAN_CO2,
                     SENSOR_CHAN_VOC,SENSOR_CHAN_PM_1_0_CF,SENSOR_CHAN_PM_2_5_CF,SENSOR_CHAN_PM_10_CF,
                     SENSOR_CHAN_PM_0_3_COUNT,SENSOR_CHAN_PM_0_5_COUNT,SENSOR_CHAN_PM_1_0_COUNT,SENSOR_CHAN_PM_2_5_COUNT,SENSOR_CHAN_PM_5_COUNT,SENSOR_CHAN_PM_10_COUNT,
                     SENSOR_CHAN_AMBIENT_TEMP,SENSOR_CHAN_HUMIDITY,SENSOR_CHAN_PRIV_START,SENSOR_CHAN_MAX=64};
struct sensor_value {int value;};
inline int sensor_sample_fetch(const device *){return 0;}
inline int sensor_sample_fetch_chan(const device *,sensor_channel){return 0;}
inline int sensor_channel_get(const device *dev,sensor_channel chan,sensor_value *out){
  if(dev->id==1&&chan==SENSOR_CHAN_CO2){out->value=432;return 0;}
  if(dev->id==2&&chan==SENSOR_CHAN_PRIV_START){out->value=71;return 0;}
  return -1;
}
inline double sensor_value_to_double(const sensor_value *v){return v->value;}
