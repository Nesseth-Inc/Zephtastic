// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrScd4xAdmin.h"
#include "Scd4xAdmin.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
extern "C" {
#include <zephyr/drivers/sensor/scd4x.h>
}
#include <zephyr/kernel.h>
#include <pb_encode.h>
#include <string.h>

namespace mt::stock::telemetry::scd4x {
namespace {
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_scd4x),sensirion_scd40)||
              DT_NODE_HAS_COMPAT(DT_ALIAS(zt_scd4x),sensirion_scd41),
              "zt-scd4x must select the matching SCD40/SCD41 driver");
const device *const sensor=DEVICE_DT_GET(DT_ALIAS(zt_scd4x));
K_MUTEX_DEFINE(busMutex);
#if !defined(CONFIG_MT_ACCESSORY_SENSORS)
K_SEM_DEFINE(requestReady,0,1);
enum {Idle,Running,Done};
atomic_t state=ATOMIC_INIT(Idle);
struct Ticket {
    uint32_t session=0,id=0;
    meshtastic_SCD4X_config cfg{};
    int result=0;
};
Ticket ticket;
struct WireRequest {
    uint32_t session=0,id=0;
    uint8_t data[meshtastic_SCD4X_config_size]{};
    size_t length=0;
    int result=0;
    bool sameId(const WireRequest &other)const{return id&&id==other.id&&session==other.session;}
    bool sameBody(const WireRequest &other)const{return length==other.length&&!memcmp(data,other.data,length);}
};
// The USB/Companion owner serializes these records. The worker accesses only ticket.
WireRequest active{},previous{};
#endif

struct Driver {
    int set(sensor_attribute_scd4x attr,const sensor_value &v){
        return sensor_attr_set(sensor,SENSOR_CHAN_ALL,static_cast<sensor_attribute>(attr),&v);
    }
    int get(sensor_attribute_scd4x attr,sensor_value &v){
        return sensor_attr_get(sensor,SENSOR_CHAN_ALL,static_cast<sensor_attribute>(attr),&v);
    }
    int factoryReset(){return scd4x_factory_reset(sensor);}
    int setAsc(bool enabled){
        const int error=set(SENSOR_ATTR_SCD4X_AUTOMATIC_CALIB_ENABLE,{enabled?1:0,0});
        return error?error:scd4x_persist_settings(sensor);
    }
    int getAsc(bool &enabled){
        sensor_value v{};const int error=get(SENSOR_ATTR_SCD4X_AUTOMATIC_CALIB_ENABLE,v);
        enabled=v.val1!=0;return error;
    }
    int mode(bool lowPower){return scd4x_set_periodic_mode(sensor,lowPower);}
    int baseline(uint16_t ppm){
        bool enabled=false;
        const int error=getAsc(enabled);
        if(error)return error;
        return enabled?scd4x_set_asc_target(sensor,ppm):-EIO;
    }
    int forcedCalibration(uint16_t reference){uint16_t correction=0;return scd4x_forced_recalibration(sensor,reference,&correction);}
    int altitude(uint32_t meters){return set(SENSOR_ATTR_SCD4X_SENSOR_ALTITUDE,{int32_t(meters),0});}
    int pressure(uint32_t pa){
        const int error=set(SENSOR_ATTR_SCD4X_AMBIENT_PRESSURE,{int32_t(pa/100),0});
        return error?error:scd4x_persist_settings(sensor);
    }
    int temperature(float reference){
        sensor_value measured{},prior{},offset{};
        int error=scd4x_fetch_calibration_sample(sensor);
        if(!error)error=sensor_channel_get(sensor,SENSOR_CHAN_AMBIENT_TEMP,&measured);
        if(!error)error=get(SENSOR_ATTR_SCD4X_TEMPERATURE_OFFSET,prior);
        if(error)return error;
        const float target=sensor_value_to_float(&prior)+sensor_value_to_float(&measured)-reference;
        if(!(target>=0&&target<=20))return -ERANGE;
        error=sensor_value_from_float(&offset,target);
        if(!error)error=set(SENSOR_ATTR_SCD4X_TEMPERATURE_OFFSET,offset);
        return error?error:scd4x_persist_settings(sensor);
    }
};
#if !defined(CONFIG_MT_ACCESSORY_SENSORS)
void run(void*,void*,void*){
    Driver driver;
    for(;;){
        k_sem_take(&requestReady,K_FOREVER);
        k_mutex_lock(&busMutex,K_FOREVER);
        ticket.result=device_is_ready(sensor)?applyScd4x(driver,ticket.cfg):-ENODEV;
        k_mutex_unlock(&busMutex);
        atomic_set(&state,Done);
    }
}
// Calibration can spend seconds in the driver. Keep the system workqueue,
// transport mutex and radio owner available while it completes.
K_THREAD_DEFINE(scd4xAdminThread,2048,run,nullptr,nullptr,nullptr,10,0,0);
#endif
}
bool lockSample(const device *dev){return dev!=sensor || k_mutex_lock(&busMutex,K_NO_WAIT)==0;}
void unlockSample(const device *dev){if(dev==sensor)k_mutex_unlock(&busMutex);}
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
int applySync(const meshtastic_SensorConfig &cfg){
    int e=validateScd4x(cfg);if(e)return e;
    if(!device_is_ready(sensor))return -ENODEV;
    k_mutex_lock(&busMutex,K_FOREVER);
    Driver driver;e=applyScd4x(driver,cfg.scd4x_config);
    k_mutex_unlock(&busMutex);return e;
}
#else
int submit(const meshtastic_SensorConfig &cfg,uint32_t session,uint32_t id){
    if(!id||!device_is_ready(sensor))return -ENODEV;
    const int checked=validateScd4x(cfg);if(checked)return checked;
    WireRequest key{};key.session=session;key.id=id;
    auto stream=pb_ostream_from_buffer(key.data,sizeof(key.data));
    if(!pb_encode(&stream,meshtastic_SCD4X_config_fields,&cfg.scd4x_config))return -EINVAL;
    key.length=stream.bytes_written;
    if(active.sameId(key))return active.sameBody(key)?0:-EINVAL;
    if(!atomic_cas(&state,Idle,Running))return -EBUSY;
    if(previous.sameId(key)){
        if(!previous.sameBody(key)){atomic_set(&state,Idle);return -EINVAL;}
        active=previous;ticket={session,id,{},previous.result};atomic_set(&state,Done);return 0;
    }
    active=key;
    ticket={session,id,cfg.scd4x_config,0};
    k_sem_give(&requestReady);return 0;
}
bool take(uint32_t session,uint32_t &id,int &result){
    if(atomic_get(&state)!=Done)return false;
    const bool same=ticket.session==session;
    id=ticket.id;result=ticket.result;previous=active;previous.result=result;active={};ticket={};atomic_set(&state,Idle);
    return same;
}
#endif
}
