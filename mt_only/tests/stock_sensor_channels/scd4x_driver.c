// SPDX-License-Identifier: GPL-3.0-only
#include "scd4x.c"
#include <stdio.h>

struct event {
    uint16_t command;
    uint16_t data;
    uint32_t at;
};
static struct event writes[64];
static unsigned count, now;
static uint16_t next_failure, read_words[3];
static bool bad_crc;

static int i2c_write_dt(const struct i2c_dt_spec *bus, const uint8_t *p, size_t n)
{
    assert(bus->addr == 0x62 && (n == 2 || n == 5) && count < 64);
    struct event *e = &writes[count++];
    e->at = now;
    e->command = sys_get_be16(p);
    if (n == 5) {
        assert(crc8(p + 2, 2, 0x31, 0xff, false) == p[4]);
        e->data = sys_get_be16(p + 2);
        // Manufacturer example word 0xbeef and its published checksum.
        if (e->data == 0xbeef) assert(p[4] == 0x92);
    }
    if (next_failure == e->command) {
        next_failure = 0;
        return -EIO;
    }
    return 0;
}
static int i2c_read_dt(const struct i2c_dt_spec *bus, uint8_t *p, size_t n)
{
    assert(bus->addr == 0x62 && count > 0 && (n == 3 || n == 9));
    for (size_t i = 0; i < n / 3; i++) {
        sys_put_be16(read_words[i], p + i * 3);
        p[i * 3 + 2] = crc8(p + i * 3, 2, 0x31, 0xff, false) ^ (bad_crc ? 1 : 0);
    }
    return 0;
}
static void k_msleep(unsigned ms) { now += ms; }
static void clear_trace(void) { count = now = 0; }
static void event(unsigned index, uint16_t command, uint32_t when)
{
    assert(index < count && writes[index].command == command && writes[index].at == when);
}
static struct scd4x_config cfg = { .bus = { 0x62 }, .model = SCD4X_MODEL_SCD41, .mode = SCD4X_MODE_NORMAL };
static struct scd4x_data samples;
static struct device dev = { &cfg, &samples };

static int period(enum sensor_attribute_scd4x which, int value)
{
    const struct sensor_value hours = { value, 0 };
    return scd4x_api_funcs.attr_set(&dev, SENSOR_CHAN_ALL, (enum sensor_attribute)which, &hours);
}
int main(void)
{
    assert(scd4x_init(&dev) == 0);
    clear_trace();
    assert(scd4x_set_periodic_mode(&dev, true) == 0);
    assert(count == 2); event(0, 0x3f86, 0); event(1, 0x21ac, 500);

    clear_trace();
    assert(scd4x_set_asc_target(&dev, 0xbeef) == 0 && count == 4);
    event(0, 0x3f86, 0); event(1, 0x243a, 500); event(2, 0x3615, 501); event(3, 0x21ac, 1301);
    assert(writes[1].data == 0xbeef);

    clear_trace();
    assert(period(SENSOR_ATTR_SCD4X_SELF_CALIB_INITIAL_PERIOD, 48) == 0);
    assert(period(SENSOR_ATTR_SCD4X_SELF_CALIB_STANDARD_PERIOD, 160) == 0);
    assert(count == 6); event(1, 0x2445, 500); event(2, 0x21ac, 501);
    event(4, 0x244e, 1001); event(5, 0x21ac, 1002);
    assert(writes[1].data == 48 && writes[4].data == 160);
    clear_trace();
    assert(period(SENSOR_ATTR_SCD4X_SELF_CALIB_STANDARD_PERIOD, 46) == -EINVAL);
    assert(period(SENSOR_ATTR_SCD4X_SELF_CALIB_INITIAL_PERIOD, 65536) == -EINVAL);
    cfg.model = SCD4X_MODEL_SCD40;
    assert(period(SENSOR_ATTR_SCD4X_SELF_CALIB_INITIAL_PERIOD, 48) == -ENOTSUP);
    assert(count == 0); cfg.model = SCD4X_MODEL_SCD41;

    next_failure = 0x243a;
    assert(scd4x_set_asc_target(&dev, 420) == -EIO && count == 3);
    event(2, 0x21ac, 500);
    clear_trace();
    next_failure = 0x3615;
    assert(scd4x_set_asc_target(&dev, 420) == -EIO && count == 4);
    event(3, 0x21ac, 1301);
    clear_trace();
    next_failure = 0x21b1;
    assert(scd4x_set_periodic_mode(&dev, false) == -EIO && count == 3);
    event(2, 0x21ac, 500);
    clear_trace();
    next_failure = 0x3f86;
    assert(scd4x_set_periodic_mode(&dev, false) == -EIO && count == 1);
    clear_trace();
    assert(scd4x_set_periodic_mode(&dev, false) == 0);
    event(1, 0x21b1, 500);

    clear_trace();
    read_words[0] = 0xf800;
    assert(scd4x_fetch_calibration_sample(&dev) == -EAGAIN && count == 1);
    assert(scd4x_fetch_calibration_sample(&dev) == -EAGAIN);
    read_words[0] = 450; read_words[1] = 26321; read_words[2] = 28180;
    bad_crc = true; assert(scd4x_fetch_calibration_sample(&dev) == -EIO); bad_crc = false;
    assert(scd4x_fetch_calibration_sample(&dev) == 0 && samples.co2_sample == 450);
    struct sensor_value value;
    assert(scd4x_api_funcs.channel_get(&dev, SENSOR_CHAN_AMBIENT_TEMP, &value) == 0 && value.val1 == 25);
    assert(scd4x_api_funcs.attr_get(&dev, SENSOR_CHAN_ALL,
                                    (enum sensor_attribute)SENSOR_ATTR_SCD4X_TEMPERATURE_OFFSET, &value) == 0);
    assert(value.val1 == 1 && value.val2 > 200000);
    // The forced calibration sentinel is an error; the old mode starts again.
    clear_trace();
    read_words[0] = UINT16_MAX;
    uint16_t correction;
    assert(scd4x_forced_recalibration(&dev, 435, &correction) == -EIO && count == 3);
    event(0, 0x3f86, 0); event(1, 0x362f, 500); event(2, 0x21b1, 900);

    cfg.mode = SCD4X_MODE_SINGLE_SHOT;
    assert(scd4x_init(&dev) == 0);
    clear_trace(); next_failure = 0x36f6; // Sleeping SCD41 NACKs the first wake.
    assert(scd4x_fetch_calibration_sample(&dev) == 0 && count == 5);
    event(0, 0x36f6, 0); event(1, 0x36f6, 30); event(2, 0x2196, 60);
    event(3, 0xec05, 110); event(4, 0x36e0, 111);
    clear_trace();
    assert(scd4x_set_periodic_mode(&dev, true) == 0);
    event(2, 0x21ac, 60);
    clear_trace(); read_words[0] = 1;
    assert(scd4x_fetch_calibration_sample(&dev) == 0 && count == 2);
    event(0, 0xe4b8, 0); event(1, 0xec05, 1);
    puts("PASS real SCD4x driver I2C frames, CRC, command delays, ASC target and periods, mode transitions including DT single-shot, errors and fresh samples");
    return 0;
}
