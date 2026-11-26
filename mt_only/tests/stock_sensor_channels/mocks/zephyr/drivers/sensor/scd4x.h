#pragma once
#include <zephyr/drivers/sensor.h>
enum sensor_attribute_scd4x {SENSOR_ATTR_SCD4X_TEMPERATURE_OFFSET=SENSOR_ATTR_PRIV_START,
    SENSOR_ATTR_SCD4X_SENSOR_ALTITUDE,SENSOR_ATTR_SCD4X_AMBIENT_PRESSURE,SENSOR_ATTR_SCD4X_AUTOMATIC_CALIB_ENABLE};
int scd4x_persist_settings(const device *);
int scd4x_factory_reset(const device *);
int scd4x_forced_recalibration(const device *,uint16_t,uint16_t *);

int scd4x_set_periodic_mode(const device *,bool);
int scd4x_set_asc_target(const device *,uint16_t);
int scd4x_fetch_calibration_sample(const device *);
