"""Compile numerical channel conversions from the locked Zephyr SHT drivers."""
import hashlib
pins={
    'shtcx':'78833baaa7765f776ed602770d341823bc6a75e1587bdbceef023e60a12f85a4',
    'sht3xd':'38cf30b9248f5349f493c41d30811730973cdf23ae7e61f15a706b525b24ee66',
    'sht4x':'28daa0b4ced551f7891d21c1f63bb360635621f929d02cd3668713b4147e864a',
}
def extract(sdk,out,func):
    code='''
#include <stdint.h>
#include <errno.h>
struct device {void *data;};
struct sensor_value {int32_t val1,val2;};
enum sensor_channel {SENSOR_CHAN_AMBIENT_TEMP,SENSOR_CHAN_HUMIDITY};
struct sht4x_data {uint16_t t_sample,rh_sample;};
struct sht3xd_data {uint16_t t_sample,rh_sample;};
'''
    for name,sha in pins.items():
        data=(sdk/'drivers/sensor/sensirion'/name/(name+'.c')).read_bytes()
        assert hashlib.sha256(data).hexdigest()==sha,name
        src=data.decode()
        signatures=['static int '+name+'_channel_get'] if name!='shtcx' else [
            'static void shtcx_temperature_from_raw','static void shtcx_humidity_from_raw']
        for sig in signatures:
            code+=func(src,sig)+'\n'
    code+='''
static float channel(const struct sensor_value *v){return (float)v->val1+(float)v->val2/1000000.0f;}
void nativeSample(unsigned model,uint16_t t,uint16_t h,float *temp,float *hum){
    struct sensor_value a,b;
    if(model==5){shtcx_temperature_from_raw(t,&a);shtcx_humidity_from_raw(h,&b);}
    else if(model==1){struct sht4x_data data={t,h};struct device d={&data};
        sht4x_channel_get(&d,SENSOR_CHAN_AMBIENT_TEMP,&a);sht4x_channel_get(&d,SENSOR_CHAN_HUMIDITY,&b);}
    else{struct sht3xd_data data={t,h};struct device d={&data};
        sht3xd_channel_get(&d,SENSOR_CHAN_AMBIENT_TEMP,&a);sht3xd_channel_get(&d,SENSOR_CHAN_HUMIDITY,&b);}
    *temp=channel(&a);*hum=channel(&b);
}
'''
    path=out/'native.c';path.write_text(code)
    return path
