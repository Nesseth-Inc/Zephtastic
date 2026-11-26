// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
struct device {bool ready;};
static device ina3221{true},ina260{true};
#define DT_NODELABEL(n) n
#define DT_NODE_EXISTS(n) (DT_REG_ADDR(n)!=0)
#define DT_NODE_HAS_STATUS(n,ok) DT_NODE_EXISTS(n)
#define DT_REG_ADDR(n) REG_ADDR(n)
#define REG_ADDR(n) ADDR_##n
#define ADDR_ina219 0
#define ADDR_ina226 0
#define ADDR_ina260 0x45
#define ADDR_ina3221 0x42
#define DEVICE_DT_GET(n) (&(n))
enum sensor_channel {SENSOR_CHAN_ALL,SENSOR_CHAN_CURRENT,SENSOR_CHAN_VOLTAGE};
enum sensor_attribute {SENSOR_ATTR_PRIV_START=100};
struct sensor_value {int val1,val2;};
static int channel=1,attempts,fetches;static bool fail=false;
static bool device_is_ready(const device *d){return d->ready;}
static int device_init(const device*){return -1;}
static int sensor_attr_set(const device *d,sensor_channel c,sensor_attribute attr,const sensor_value *v){
    assert(d==&ina3221&&c==SENSOR_CHAN_ALL&&int(attr)==101&&v->val1==CONFIG_MT_INA3221_BAT_CHANNEL);
    ++attempts;if(fail)return -1;channel=v->val1;return 0;
}
static int sensor_sample_fetch(const device*){++fetches;return 0;}
static int sensor_channel_get(const device *d,sensor_channel c,sensor_value *v){
    v->val1=d==&ina260?7:c==SENSOR_CHAN_VOLTAGE?channel:channel-5;v->val2=0;return 0;
}
static float sensor_value_to_float(const sensor_value *v){return v->val1;}
#include "power/InaBatterySelector.cpp"
int main(){
    using namespace mt::power::ina;
    static_assert(CONFIG_MT_INA3221_BAT_CHANNEL>=1&&CONFIG_MT_INA3221_BAT_CHANNEL<=3);
    assert(!supports(0x46)&&configure(0x42));
    poll(1000);auto sample=snapshot(1000);assert(sample.available()&&sample.busVoltageV==CONFIG_MT_INA3221_BAT_CHANNEL);
    channel=2;fail=true;poll(2000);assert(!snapshot(2000).available()&&attempts==2&&fetches==1);
    fail=false;poll(3000);assert(snapshot(3000).currentA==CONFIG_MT_INA3221_BAT_CHANNEL-5&&attempts==3);
    assert(configure(0x45));poll(4000);assert(snapshot(4000).busVoltageV==7&&attempts==3&&fetches==3);
    puts("PASS selected battery channel reapplied after power scan, acquisition failure and change to INA260");
}
