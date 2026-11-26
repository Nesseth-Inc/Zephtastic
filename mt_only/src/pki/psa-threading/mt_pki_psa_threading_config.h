// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Applied identically to Zephyr Mbed TLS and the shared TF-PSA library.
#if !defined(CONFIG_MT_SHARED_MBEDTLS_THREADING)
#error "Shared Mbed TLS threading config is not selected"
#endif
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT
