// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrBme680.h"
#include "Bme680Sampling.h"
#include "environment/EnvironmentMetricsAdapter.h"
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <string.h>

namespace mt::stock::telemetry::bme680 {
namespace {
#define BME_NODE DT_ALIAS(zt_bme680)
static_assert(DT_NODE_HAS_COMPAT(BME_NODE,bosch_bme680)&&DT_ON_BUS(BME_NODE,i2c),"zt-bme680 must select a Bosch BME680 on I2C");
static_assert(DT_PROP(BME_NODE,zephyr_deferred_init),"defer BME680 startup until environment telemetry is enabled");
static_assert(IS_ENABLED(CONFIG_BME680_TEMP_OVER_8X)&&IS_ENABLED(CONFIG_BME680_HUMIDITY_OVER_2X)&&
              IS_ENABLED(CONFIG_BME680_PRESS_OVER_4X)&&IS_ENABLED(CONFIG_BME680_FILTER_4)&&
              IS_ENABLED(CONFIG_BME680_HEATR_TEMP_LP),"select the acquisition profile in bme680.conf");
const device *const sensor=DEVICE_DT_GET(BME_NODE);
const i2c_dt_spec bus=I2C_DT_SPEC_GET(BME_NODE);
constexpr const char *stateKey="mtxsensor/bme680";
K_SEM_DEFINE(wake,0,1);
K_MUTEX_DEFINE(owner);
Sampling sampling;
bool enabled=false,retired=false,loaded=false;
unsigned generation=0;
uint64_t next=0;
atomic_t epochUtc=ATOMIC_INIT(0);
int readState(BME680IaqState &s){
    ssize_t len=settings_get_val_len(stateKey);
    if(len<0)return int(len);
    if(!len)return -ENOENT;
    if(len!=sizeof(s))return -EILSEQ;
    len=settings_load_one(stateKey,&s,sizeof(s));return len==sizeof(s)?0:len<0?int(len):-EIO;
}
bool loadState(uint64_t at,uint32_t utc){
    BME680IaqState s{};const int rc=readState(s);
    if(!rc)(void)sampling.restore(s,at,utc);
    return !rc||rc==-ENOENT||rc==-EILSEQ;
}
void checkpoint(uint64_t at,uint32_t utc){
    if(!sampling.checkpointDue(at,utc))return;
    BME680IaqState s{},verify{};sampling.serialize(s,utc);
    if(!settings_save_one(stateKey,&s,sizeof(s))&&!readState(verify)&&!memcmp(&s,&verify,sizeof(s)))sampling.saved(at,utc);
}
bool channel(sensor_channel c,float &out){
    sensor_value v{};if(sensor_channel_get(sensor,c,&v))return false;
    out=sensor_value_to_float(&v);return true;
}
bool measure(Reading &r){
    // The forced conversion uses the driver's stock 320 C heater setting.
    // Bosch encodes 150 ms as 37 x 4 ms in gas_wait_0. Gas data LSB bit 5
    // reports valid gas and bit 4 heater stability for this same conversion.
    uint8_t flags=0;
    if(!device_is_ready(sensor)){
        if(device_init(sensor)||!device_is_ready(sensor))return false;
        // Zephyr starts one conversion during initialisation. Let its default
        // heater cycle finish before requesting the first stock-profile sample.
        k_msleep(300);
    }
    if(i2c_reg_write_byte_dt(&bus,0x64,0x65)||sensor_sample_fetch(sensor)||
       !channel(SENSOR_CHAN_AMBIENT_TEMP,r.temperature)||!channel(SENSOR_CHAN_HUMIDITY,r.humidity)||
       !channel(SENSOR_CHAN_PRESS,r.pressureHpa)||i2c_reg_read_byte_dt(&bus,0x2b,&flags))return false;
    r.pressureHpa*=10;
    r.gasOhms=0;
    return (flags&0x30)!=0x30||channel(SENSOR_CHAN_GAS_RES,r.gasOhms);
}
void work(void*,void*,void*){
    for(;;){
        k_mutex_lock(&owner,K_FOREVER);
        uint64_t now=k_uptime_get();const bool active=enabled&&!retired;
        if(!active||now<next){
            const auto delay=active?K_MSEC(next-now):K_FOREVER;
            k_mutex_unlock(&owner);k_sem_take(&wake,delay);continue;
        }
        const auto token=generation;next=now+Sampling::Interval;
        if(!loaded)loaded=loadState(now,uint32_t(atomic_get(&epochUtc)));
        const bool maySample=loaded;k_mutex_unlock(&owner);
        if(!maySample)continue;
        Reading reading{};
        if(!measure(reading)&&!measure(reading))continue; // One stock one-shot retry.
        k_mutex_lock(&owner,K_FOREVER);
        if(enabled&&!retired&&token==generation){
            now=k_uptime_get();sampling.accept(reading,now);next=now+Sampling::Interval;
            checkpoint(now,uint32_t(atomic_get(&epochUtc)));
        }
        k_mutex_unlock(&owner);
    }
}
K_THREAD_DEFINE(bme680_thread,2304,work,nullptr,nullptr,nullptr,9,0,0);
}
void clock(uint32_t utc){atomic_set(&epochUtc,utc);}
void enable(bool value){
    // The radio loop must not wait for an accessory flash checkpoint.
    if(k_mutex_lock(&owner,K_NO_WAIT))return;
    if(value!=enabled&&!retired){++generation;enabled=value;next=0;sampling.invalidate();k_sem_give(&wake);}
    k_mutex_unlock(&owner);
}
void merge(zt::telemetry::EnvironmentReadings &out,uint64_t now){
    meshtastic_EnvironmentMetrics m{};
    if(k_mutex_lock(&owner,K_NO_WAIT))return;
    const bool ok=enabled&&!retired&&sampling.metrics(now,m);k_mutex_unlock(&owner);
    if(ok){
        const auto source = zt::telemetry::EnvironmentSource::Bme680;
        out.ambientCelsius.record(source,m.temperature);
        out.humidityPercent.record(source,m.relative_humidity);
        out.pressureHpa.record(source,m.barometric_pressure);
        if(m.has_gas_resistance)out.bmeGas=m.gas_resistance;
        if(m.has_iaq)out.bmeIaq=m.iaq;
    }
}
int retire(){
    k_mutex_lock(&owner,K_FOREVER);retired=true;++generation;sampling.invalidate();k_sem_give(&wake);
    const int rc=settings_delete(stateKey);
    const auto len=settings_get_val_len(stateKey);
    k_mutex_unlock(&owner);
    if(rc&&rc!=-ENOENT)return rc;
    return len==0||len==-ENOENT?0:len<0?int(len):-EIO;
}
}
