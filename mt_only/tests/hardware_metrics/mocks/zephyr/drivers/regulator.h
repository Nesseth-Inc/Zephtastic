#ifndef MOCK_ZEPHYR_REGULATOR_H
#define MOCK_ZEPHYR_REGULATOR_H
#include <zephyr/device.h>
#ifdef __cplusplus
extern "C" {
#endif
int regulator_enable(const struct device *dev);
int regulator_disable(const struct device *dev);
#ifdef __cplusplus
}
#endif
#endif
