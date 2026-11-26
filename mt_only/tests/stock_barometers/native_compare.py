"""Extract the native SDK compensation code at the repository's pinned Zephyr revision."""
import hashlib
from pathlib import Path

sources = {
    'bosch/bmp180/bmp180.c': 'ec69c4372d427a7615e1f0cd9a4d800b4b00fe48a8a3ee3c245f75e93159c315',
    'bosch/bmp388/bmp388.c': '4974826c68d556efd10e4c3a6a275790a85f638dc4be29ebbed3e5f3328bf995',
    'aosong/dht20/dht20.c': '0fcedf72e6f11198c0d772f65a7e60f70c4e024e5b618dc15fc7df1f88acc76f',
    'infineon/dps310/dps310.c': '40196085302b8d0153227e9f1ff82f344df1ea3d7f90d3c9c93be652a6ae2d3e',
}
def extract(sdk, out, function):
    src = {}
    for name, pin in sources.items():
        data=(sdk/'drivers/sensor'/name).read_bytes()
        assert hashlib.sha256(data).hexdigest()==pin, name
        src[Path(name).stem]=data.decode()
    assert 'DHT20_MEAS_FRAME_LENGTH    7' in src['dht20']
    assert 'dps310_measure_tmp_psr(dev)' in src['dps310']
    prelude = '''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#define __ASSERT(ok,...) assert(ok)
#define BIT(n) (1u<<(n))
struct device {void *data;};
struct sensor_value {int32_t val1,val2;};
static float get(const struct sensor_value *v) {return (float)v->val1+(float)v->val2/1000000.0f;}
struct bmp180_cal_data {int16_t ac1,ac2,ac3;uint16_t ac4,ac5,ac6;int16_t b1,b2,mb,mc,md;};
struct bmp180_data {uint8_t osr_pressure;int32_t raw_press,raw_temp,comp_temp;struct bmp180_cal_data cal;};
struct bmp388_cal_data {uint16_t t1,t2;int8_t t3;int16_t p1,p2;int8_t p3,p4;uint16_t p5,p6;int8_t p7,p8;int16_t p9;int8_t p10,p11;} __attribute__((packed));
struct bmp388_sample {uint32_t press,raw_temp;int64_t comp_temp;};
struct bmp388_data {struct bmp388_cal_data cal;struct bmp388_sample sample;};
'''
    code = ''
    for kind in ('bmp180','bmp388'):
        for prefix in ('static void '+kind+'_compensate_temp','static int '+kind+'_temp_channel_get',
                       ('static uint32_t ' if kind=='bmp180' else 'static uint64_t ')+kind+'_compensate_press',
                       'static int '+kind+'_press_channel_get'):
            code+=function(src[kind],prefix)+'\n'
    wrapper = '''
void nativeSample(int part,const uint8_t *cal,const uint8_t *raw,float *temp,float *pressure) {
    struct sensor_value t,p;
    if (part==1) {
        struct bmp180_data d={0};struct device dev={&d};
        uint16_t words[11];
        for (int i=0;i<11;i++) words[i]=((uint16_t)cal[i*2]<<8)|cal[i*2+1];
        memcpy(&d.cal,words,sizeof(words)); d.osr_pressure=3;
        d.raw_temp=((uint16_t)raw[0]<<8)|raw[1]; d.raw_press=(((uint32_t)raw[2]<<16)|(raw[3]<<8)|raw[4])>>5;
        bmp180_temp_channel_get(&dev,&t); bmp180_press_channel_get(&dev,&p);
    } else {
        struct bmp388_data d={0};struct device dev={&d};
        const uint16_t endian=1; assert(*(const uint8_t*)&endian && sizeof(d.cal)==21);
        memcpy(&d.cal,cal,21);
        d.sample.press=raw[0]|(raw[1]<<8)|(raw[2]<<16);d.sample.raw_temp=raw[3]|(raw[4]<<8)|(raw[5]<<16);
        bmp388_temp_channel_get(&dev,&t);bmp388_press_channel_get(&dev,&p);
    }
    *temp=get(&t); *pressure=get(&p)*10.0f;
}
'''
    path=out/'native.c';path.write_text(prelude+code+wrapper)
    return path
