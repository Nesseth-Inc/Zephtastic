#ifndef MOCK_ZEPHYR_ADC_H
#define MOCK_ZEPHYR_ADC_H
#include <zephyr/device.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct adc_dt_spec { const struct device *dev; uint8_t channel_id; };
struct adc_sequence { void *buffer; size_t buffer_size; };
bool adc_is_ready_dt(const struct adc_dt_spec *spec);
int adc_raw_to_millivolts_dt(const struct adc_dt_spec *spec,int32_t *value);
int adc_channel_setup_dt(const struct adc_dt_spec *spec);
int adc_sequence_init_dt(const struct adc_dt_spec *spec,struct adc_sequence *seq);
int adc_read_dt(const struct adc_dt_spec *spec,const struct adc_sequence *seq);
#ifdef __cplusplus
}
#endif
#define ADC_DT_SPEC_GET(node) {&mock_adc_device,1}
#endif
