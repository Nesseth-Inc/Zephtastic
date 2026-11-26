#ifndef M104_MOCK_CONTROL_H
#define M104_MOCK_CONTROL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum mock_event {MOCK_ADC_READY=1,MOCK_DIVIDER_READY,MOCK_SETUP,MOCK_ENABLE,MOCK_SEQUENCE,MOCK_READ,MOCK_DISABLE,MOCK_VBUS};
struct mock_state {
 int conversion_status;
 bool adc_ready,divider_ready,usb;
 int setup_status,enable_status,disable_status;
 int sequence_status[8],read_status[8];int16_t raw[8];
 unsigned setup_calls,enable_calls,disable_calls,sequence_calls,read_calls,vbus_calls;
 unsigned sample_index,violations;int refs;
 uint64_t now,read_times[64];
 enum mock_event events[512];size_t event_count;
};
extern struct mock_state mock;
void mock_reset(void);
#ifdef __cplusplus
}
#endif
#endif
