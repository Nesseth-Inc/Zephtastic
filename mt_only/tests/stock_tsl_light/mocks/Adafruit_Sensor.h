// SPDX-License-Identifier: GPL-3.0-only
#pragma once
struct Adafruit_Sensor {};
struct sensor_t {char name[24];int version,sensor_id,type,min_delay;float max_value,min_value,resolution;};
struct sensors_event_t {int version,sensor_id,type;unsigned timestamp;float light;};
enum { SENSOR_TYPE_LIGHT=5 };
