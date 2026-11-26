#ifndef MOCK_ZEPHYR_DEVICE_H
#define MOCK_ZEPHYR_DEVICE_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
struct device { int identity; };
extern struct device mock_adc_device, mock_divider_device;
bool device_is_ready(const struct device *dev);
#ifdef __cplusplus
}
#endif
#ifdef MT_MOCK_NO_DIVIDER
#define DEVICE_DT_GET_OR_NULL(node) ((const struct device *)0)
#else
#define DEVICE_DT_GET_OR_NULL(node) (&mock_divider_device)
#endif
#endif
