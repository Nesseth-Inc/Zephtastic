// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Zephyr follows the explicit Kconfig value. Native fixtures preserve the
// historical diagnostics by default and opt out only with the test-profile
// define, so existing native contexts do not silently change behavior.
#if defined(__ZEPHYR__)
#if defined(CONFIG_MT_PKI_EXTENDED_DIAGNOSTICS)
#define MT_PKI_EXTENDED_DIAGNOSTICS_ENABLED 1
#else
#define MT_PKI_EXTENDED_DIAGNOSTICS_ENABLED 0
#endif
#elif defined(MT_PKI_EXTENDED_DIAGNOSTICS_NATIVE_DISABLED)
#define MT_PKI_EXTENDED_DIAGNOSTICS_ENABLED 0
#else
#define MT_PKI_EXTENDED_DIAGNOSTICS_ENABLED 1
#endif
