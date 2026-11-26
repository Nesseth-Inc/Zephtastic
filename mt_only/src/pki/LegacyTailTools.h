// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Native regression fixtures retain the existing migration API by default.
// A separate disabled-profile fixture can exercise the production boot paths
// without those optional tools; Zephyr builds always use their Kconfig choice.
#if defined(__ZEPHYR__)
#if defined(CONFIG_MT_PKI_LEGACY_TAIL_TOOLS)
#define MT_PKI_LEGACY_TAIL_TOOLS_ENABLED 1
#else
#define MT_PKI_LEGACY_TAIL_TOOLS_ENABLED 0
#endif
#elif defined(MT_PKI_LEGACY_TAIL_TOOLS_NATIVE_DISABLED)
#define MT_PKI_LEGACY_TAIL_TOOLS_ENABLED 0
#else
#define MT_PKI_LEGACY_TAIL_TOOLS_ENABLED 1
#endif
