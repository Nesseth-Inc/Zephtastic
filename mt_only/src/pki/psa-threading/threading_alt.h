// SPDX-License-Identifier: GPL-3.0-only
#ifndef MT_PKI_THREADING_ALT_H
#define MT_PKI_THREADING_ALT_H
#include <zephyr/kernel.h>
typedef struct k_mutex mbedtls_platform_mutex_t;
typedef struct k_condvar mbedtls_platform_condition_variable_t;
#endif
