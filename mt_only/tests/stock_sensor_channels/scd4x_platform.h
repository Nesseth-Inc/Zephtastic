// SPDX-License-Identifier: GPL-3.0-only
// Zephyr API boundary for compiling the locked C driver against a timed I2C fixture.
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>

struct device { const void *config; void *data; };
struct i2c_dt_spec { unsigned addr; const struct device *bus; };
enum sensor_channel { SENSOR_CHAN_ALL, SENSOR_CHAN_CO2, SENSOR_CHAN_AMBIENT_TEMP, SENSOR_CHAN_HUMIDITY };
enum sensor_attribute { SENSOR_ATTR_RESOLUTION = 10, SENSOR_ATTR_PRIV_START = 20, SENSOR_ATTR_MAX = 127 };
struct sensor_value { int32_t val1, val2; };
struct sensor_api {
    int (*sample_fetch)(const struct device *, enum sensor_channel);
    int (*channel_get)(const struct device *, enum sensor_channel, struct sensor_value *);
    int (*attr_set)(const struct device *, enum sensor_channel, enum sensor_attribute, const struct sensor_value *);
    int (*attr_get)(const struct device *, enum sensor_channel, enum sensor_attribute, struct sensor_value *);
};
#define DEVICE_API(kind, name) const struct sensor_api name
#define LOG_MODULE_REGISTER(...)
#define LOG_ERR(...)
#define LOG_ERR_DEVICE_NOT_READY(...)
#define DT_INST_FOREACH_STATUS_OKAY_VARGS(...)

static inline bool i2c_is_ready_dt(const struct i2c_dt_spec *bus) { return bus->addr == 0x62; }
static inline uint16_t sys_get_be16(const uint8_t *p) { return (uint16_t)p[0] << 8 | p[1]; }
static inline void sys_put_be16(uint16_t n, uint8_t *p) { p[0] = n >> 8; p[1] = n; }
static inline uint8_t crc8(const uint8_t *p, size_t size, uint8_t poly, uint8_t init, bool reverse)
{
    assert(!reverse);
    uint8_t value = init;
    while (size--) {
        value ^= *p++;
        for (unsigned bit = 0; bit < 8; bit++)
            value = (value & 0x80) ? ((value << 1) ^ poly) : (value << 1);
    }
    return value;
}
static void k_msleep(unsigned ms);
static int i2c_write_dt(const struct i2c_dt_spec *, const uint8_t *, size_t);
static int i2c_read_dt(const struct i2c_dt_spec *, uint8_t *, size_t);

#define DT_INST_FOREACH_STATUS_OKAY(...)
#define LOG_DBG(...)
#define __ASSERT_NO_MSG(cond) assert(cond)
#define USEC_PER_MSEC 1000
#define K_MSEC(ms) ((ms) * 1000)
#define K_USEC(us) (us)
static inline bool device_is_ready(const struct device *dev) { return dev != NULL; }
static void k_sleep(unsigned us);
#define k_busy_wait k_sleep
static int i2c_write_read_dt(const struct i2c_dt_spec *, const uint8_t *, size_t, uint8_t *, size_t);
enum pm_device_action { PM_DEVICE_ACTION_TURN_ON, PM_DEVICE_ACTION_RESUME, PM_DEVICE_ACTION_SUSPEND, PM_DEVICE_ACTION_TURN_OFF };
static inline int pm_device_driver_init(const struct device *dev, int (*action)(const struct device *, enum pm_device_action))
{
    return action(dev, PM_DEVICE_ACTION_TURN_ON);
}
enum sensor_attribute_sht4x { SENSOR_ATTR_SHT4X_HEATER_POWER = SENSOR_ATTR_PRIV_START, SENSOR_ATTR_SHT4X_HEATER_DURATION };
