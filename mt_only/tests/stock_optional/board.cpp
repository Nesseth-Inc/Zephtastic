// SPDX-License-Identifier: GPL-3.0-only
#include "stock/optional/DropzoneBoard.h"
#include <cassert>
#include <cstdio>
#if MT_MOCK_HAS_ADC
device mock_adc_device{},mock_divider_device{};
static bool adcOk=true,busReady=true;
static int16_t sample=0;
static const char *keys[]={"Temp","Speed","Pressure","Dir"};
static const char *values[]={"-7.75","7.25","1003.5","SSE"};
static unsigned key=0,segment=0;
bool adc_is_ready_dt(const adc_dt_spec *){return adcOk;}
int adc_channel_setup_dt(const adc_dt_spec *){return 0;}
int adc_sequence_init_dt(const adc_dt_spec *,adc_sequence *){return 0;}
int adc_read_dt(const adc_dt_spec *,const adc_sequence *seq){
    assert(seq->buffer_size==sizeof(sample));
    memcpy(seq->buffer,&sample,sizeof(sample));return 0;
}
bool i2c_is_ready_dt(const i2c_dt_spec *){return busReady;}
int i2c_write_dt(const i2c_dt_spec *bus,const uint8_t *data,size_t n){
    assert(bus->addr==0x42&&n==3+strlen(keys[key])&&data[0]==0&&data[1]==n-3&&!data[2]&&!memcmp(data+3,keys[key],n-3));
    return 0;
}
int i2c_read_dt(const i2c_dt_spec *,uint8_t *out,size_t n){
    switch(segment++){
    case 0:assert(n==1);out[0]=0x53;break;
    case 1:assert(n==1);out[0]=0;break;
    case 2:assert(n==2);out[0]=strlen(values[key]);out[1]=0;break;
    default:assert(n==strlen(values[key]));memcpy(out,values[key++],n);segment=0;break;
    }
    return 0;
}
#endif
int main(){
    mt::stock::optional::DropzoneBoard board;mt::stock::optional::Conditions conditions;
    board.request(22,4);
    for(unsigned t=1;t<=5;t++)board.hardwarePoll(t);
#if MT_MOCK_HAS_ADC
    assert(board.take(22,4,conditions)&&conditions.open&&conditions.hasWeather);
    assert(conditions.celsius==-7.75f&&conditions.windMps==7.25f&&conditions.pressureHpa==1003.5f&&conditions.bearing==157);
    assert(!board.take(22,4,conditions));
    busReady=false;sample=100;board.request(22,4);board.hardwarePoll(100);assert(board.take(22,4,conditions)&&!conditions.open&&!conditions.hasWeather);
    sample=99;board.request(22,4);board.hardwarePoll(101);assert(board.take(22,4,conditions)&&conditions.open&&!conditions.hasWeather);
    board.request(22,4);board.hardwarePoll(102);assert(!board.take(22,5,conditions));
    adcOk=false;board.request(22,4);board.hardwarePoll(103);assert(!board.take(22,4,conditions));
    puts("PASS mapped Zephyr ADC/I2C driver paths, boundary, absent sensor, epoch change, unreadable ADC");
#else
    assert(!board.take(22,4,conditions));puts("PASS unmapped Zephyr status input suppresses conditions");
#endif
}
