// SPDX-License-Identifier: GPL-3.0-only
#ifdef TEST_SHT3XD
#include "sht3xd.c"
#define api sht3xd_driver_api
#else
#include "sht4x.c"
#define api sht4x_api
#endif
#include <stdio.h>

static unsigned time_us, count, read_at;
static uint16_t commands[20], fail_command, fail_recovery;
static unsigned timestamps[20];
static const struct device bus;
#ifdef TEST_SHT3XD
static struct sht3xd_config cfg = { .bus = {0x44, &bus} };
static struct sht3xd_data data;
#else
static struct sht4x_config cfg = { .bus = {0x44, &bus}, .repeatability = 2 };
static struct sht4x_data data;
#endif
static struct device sensor = { &cfg, &data };
static void k_sleep(unsigned us) { time_us += us; }
static int i2c_write_dt(const struct i2c_dt_spec *spec, const uint8_t *p, size_t n)
{
    assert(spec->addr == 0x44 && (n == 1 || n == 2) && count < 20);
    const uint16_t command = n == 1 ? *p : sys_get_be16(p);
    timestamps[count] = time_us; commands[count++] = command;
    if (fail_command && command == fail_command) {
        fail_command = fail_recovery; fail_recovery = 0; return -EIO;
    }
    return 0;
}
static int i2c_read_dt(const struct i2c_dt_spec *spec, uint8_t *p, size_t n)
{
    assert(spec->addr == 0x44 && n == 6);
    for (unsigned i = 0; i < n; i += 3) {
        sys_put_be16(0xbeef,p+i); p[i+2]=0x92;
    }
    read_at = time_us; return 0;
}
static int i2c_write_read_dt(const struct i2c_dt_spec *spec, const uint8_t *tx, size_t len, uint8_t *rx, size_t n)
{
    int ret = i2c_write_dt(spec,tx,len); return ret ? ret : i2c_read_dt(spec,rx,n);
}
static int accuracy(int level)
{
    const struct sensor_value v = {level,0};
    return api.attr_set(&sensor,SENSOR_CHAN_ALL,SENSOR_ATTR_RESOLUTION,&v);
}
int main(void)
{
#ifdef TEST_SHT3XD
    assert(sht3xd_init(&sensor) == 0);
#ifdef CONFIG_SHT3XD_PERIODIC_MODE
    const uint16_t commands_for_level[] = {0x212d,0x2126,0x2130};
#else
    const uint16_t commands_for_level[] = {0x2416,0x240b,0x2400};
    const unsigned delays[] = {4000,6000,15000};
#endif
#else
    assert(sht4x_init(&sensor) == 0);
    const uint16_t commands_for_level[] = {0xe0,0xf6,0xfd};
    const unsigned delays[] = {1600,4500,8300};
#endif
    assert(data.repeatability == 2);
    for(int level=0; level<=2; level++){
        count=time_us=read_at=0;
        assert(accuracy(level)==0); assert(data.repeatability==level);
        assert(api.sample_fetch(&sensor,SENSOR_CHAN_ALL)==0);
#if defined(TEST_SHT3XD) && defined(CONFIG_SHT3XD_PERIODIC_MODE)
        assert(count==3 && commands[0]==0x3093 && commands[1]==commands_for_level[level] && timestamps[1]==1000);
        assert(commands[2]==0xe000);assert(accuracy(level)==0 && count==3);
#else
        assert(count==1 && commands[0]==commands_for_level[level] && read_at==delays[level]);
#endif
    }
    count=0;assert(accuracy(3)==-EINVAL && accuracy(-1)==-EINVAL && count==0 && data.repeatability==2);
#if defined(TEST_SHT3XD) && defined(CONFIG_SHT3XD_PERIODIC_MODE)
    fail_command=0x212d;assert(accuracy(0)==-EIO && commands[2]==0x2130 && data.repeatability==2);
    count=0;fail_command=0x212d;fail_recovery=0x2130;
    assert(accuracy(0)==-EIO && !data.periodic_active);
    count=0;assert(accuracy(2)==0 && data.periodic_active && count==1 && commands[0]==0x2130);
#else
    fail_command=commands_for_level[2];assert(api.sample_fetch(&sensor,SENSOR_CHAN_ALL)==-EIO);
    assert(api.sample_fetch(&sensor,SENSOR_CHAN_ALL)==0);
#endif
    puts("PASS real SHT measurement accuracy, command timing, invalid range and bus recovery");
    return 0;
}
