// SPDX-License-Identifier: GPL-3.0-only
#include "mock_control.h"
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/regulator.h>
#include <hal/nrf_power.h>
#include <errno.h>
#include <string.h>
struct device mock_adc_device={1},mock_divider_device={2};
NRF_POWER_Type mock_nrf_power={3};
const mock_ficr_t mock_nrf_ficr={{1,2},{3,4}};
struct mock_state mock;
static void event(enum mock_event e){if(mock.event_count<512)mock.events[mock.event_count++]=e;else ++mock.violations;}
void mock_reset(void){memset(&mock,0,sizeof(mock));mock.adc_ready=true;mock.divider_ready=true;for(unsigned i=0;i<8;++i)mock.raw[i]=900;}
static void spec_check(const struct adc_dt_spec *spec){if(!spec||spec->dev!=&mock_adc_device||spec->channel_id!=1)++mock.violations;}
bool device_is_ready(const struct device *dev){event(MOCK_DIVIDER_READY);if(dev!=&mock_divider_device)++mock.violations;return mock.divider_ready;}
bool adc_is_ready_dt(const struct adc_dt_spec *spec){spec_check(spec);event(MOCK_ADC_READY);return mock.adc_ready;}
int adc_raw_to_millivolts_dt(const struct adc_dt_spec *spec,int32_t *value){
 spec_check(spec);if(!value||*value!=4096){++mock.violations;return -EINVAL;}
 if(mock.conversion_status)return mock.conversion_status;
 // nRF SAADC on T096 uses 600 mV internal reference, 1/6 gain and 12 bits.
 *value=3600;return 0;
}
int adc_channel_setup_dt(const struct adc_dt_spec *spec){spec_check(spec);event(MOCK_SETUP);++mock.setup_calls;return mock.setup_status;}
int regulator_enable(const struct device *dev){if(dev!=&mock_divider_device)++mock.violations;event(MOCK_ENABLE);++mock.enable_calls;if(!mock.enable_status)++mock.refs;return mock.enable_status;}
int regulator_disable(const struct device *dev){if(dev!=&mock_divider_device)++mock.violations;event(MOCK_DISABLE);++mock.disable_calls;if(!mock.disable_status){--mock.refs;if(mock.refs<0)++mock.violations;}return mock.disable_status;}
int adc_sequence_init_dt(const struct adc_dt_spec *spec,struct adc_sequence *seq){spec_check(spec);if(!seq||!seq->buffer||seq->buffer_size!=sizeof(int16_t))++mock.violations;event(MOCK_SEQUENCE);mock.sample_index=mock.sequence_calls%8;++mock.sequence_calls;return mock.sequence_status[mock.sample_index];}
int adc_read_dt(const struct adc_dt_spec *spec,const struct adc_sequence *seq){spec_check(spec);event(MOCK_READ);
#ifndef MT_MOCK_NO_DIVIDER
 if(mock.refs!=1)++mock.violations;
#endif
 if(mock.read_calls<64)mock.read_times[mock.read_calls]=mock.now;else ++mock.violations;
 ++mock.read_calls;if(mock.read_status[mock.sample_index])return mock.read_status[mock.sample_index];
 if(seq&&seq->buffer&&seq->buffer_size==sizeof(int16_t))memcpy(seq->buffer,&mock.raw[mock.sample_index],sizeof(int16_t));else ++mock.violations;
 return 0;}
bool nrf_power_usbregstatus_vbusdet_get(const NRF_POWER_Type *power){if(power!=NRF_POWER)++mock.violations;event(MOCK_VBUS);++mock.vbus_calls;return mock.usb;}
