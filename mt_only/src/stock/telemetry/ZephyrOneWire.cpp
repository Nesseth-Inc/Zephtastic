// SPDX-License-Identifier: GPL-3.0-only
#include "OneWireThermometers.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
namespace mt::stock::telemetry::oneWire {
namespace {
const device *const sensors[]={
    DEVICE_DT_GET_OR_NULL(DT_ALIAS(zt_one_wire_0)),DEVICE_DT_GET_OR_NULL(DT_ALIAS(zt_one_wire_1)),
    DEVICE_DT_GET_OR_NULL(DT_ALIAS(zt_one_wire_2)),DEVICE_DT_GET_OR_NULL(DT_ALIAS(zt_one_wire_3)),
    DEVICE_DT_GET_OR_NULL(DT_ALIAS(zt_one_wire_4)),DEVICE_DT_GET_OR_NULL(DT_ALIAS(zt_one_wire_5)),
    DEVICE_DT_GET_OR_NULL(DT_ALIAS(zt_one_wire_6)),DEVICE_DT_GET_OR_NULL(DT_ALIAS(zt_one_wire_7))};
K_MUTEX_DEFINE(stateLock);
K_SEM_DEFINE(requested,0,1);
bool enabled=false,running=false;
uint32_t epoch=0,requestEpoch=0;
uint64_t lastRequest=0,requestTime=0,sampledAt=0;
unsigned primary=0;
OneWireMeasurements latest{};
void run(void *,void *,void *){
    for(;;){
        k_sem_take(&requested,K_FOREVER);
        OneWireMeasurements sample;
        for(unsigned slot=0;slot<8;++slot){
            auto dev=sensors[slot];sensor_value v{};
            if(dev&&device_is_ready(dev)&&!sensor_sample_fetch(dev)&&!sensor_channel_get(dev,SENSOR_CHAN_AMBIENT_TEMP,&v))
                sample.record(slot,sensor_value_to_float(&v));
        }
        k_mutex_lock(&stateLock,K_FOREVER);
        if(enabled&&requestEpoch==epoch){latest=sample;sampledAt=requestTime;}
        running=false;
        k_mutex_unlock(&stateLock);
    }
}
// A conversion on each of eight buses can take seconds. Keep the radio
// owner free and publish only readings from one completed acquisition.
K_THREAD_DEFINE(oneWireThread,1536,run,nullptr,nullptr,nullptr,10,0,0);
}
void poll(bool active,uint64_t now){
    k_mutex_lock(&stateLock,K_FOREVER);
    if(active!=enabled||now<lastRequest){++epoch;latest={};lastRequest=0;enabled=active;}
    if(enabled&&!running&&(lastRequest==0||now-lastRequest>=1000)){
        running=true;lastRequest=requestTime=now;requestEpoch=epoch;k_sem_give(&requested);
    }
    k_mutex_unlock(&stateLock);
}
bool snapshot(uint64_t now,OneWireMeasurements &out,unsigned &selected){
    k_mutex_lock(&stateLock,K_FOREVER);
    const bool fresh=enabled&&latest.mask&&now>=sampledAt&&now-sampledAt<=90000;
    if(fresh){out=latest;selected=primary;}
    k_mutex_unlock(&stateLock);
    return fresh;
}
int select(unsigned selected){
    if(selected>=8)return -EINVAL;
    k_mutex_lock(&stateLock,K_FOREVER);
    primary=selected;k_mutex_unlock(&stateLock);return 0;
}
}
