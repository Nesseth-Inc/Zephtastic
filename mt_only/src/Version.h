// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "pki/LegacyTailTools.h"
namespace mt {
#if defined(CONFIG_MT_M119_TLS_TEST)
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-tls";
#elif defined(CONFIG_MT_STORE_FORWARD_PSRAM)
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-sf";
#else
#if defined(CONFIG_MT_PKI_STOCK_TRIAL)
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188d1";
#elif !MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188s";
#else
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188";
#endif
#elif defined(CONFIG_MT_RUNTIME_RESOURCES)
#if defined(CONFIG_MT_ALLOW_TX)
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-txr";
#else
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-rxr";
#endif
#elif defined(CONFIG_MT_PKI_TARGET_RUNTIME)
#if defined(CONFIG_MT_BLE_DIAGNOSTICS) && defined(CONFIG_MT_ALLOW_TX) && !defined(CONFIG_MT_NORMAL_ENDPOINT)
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-p1d";
#elif defined(CONFIG_MT_BLE_DIAGNOSTICS) && defined(CONFIG_MT_ALLOW_TX)
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-ptd";
#elif defined(CONFIG_MT_BLE_DIAGNOSTICS)
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-prd";
#elif defined(CONFIG_MT_ALLOW_TX) && !defined(CONFIG_MT_NORMAL_ENDPOINT)
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-p1t";
#elif defined(CONFIG_MT_ALLOW_TX)
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-ptx";
#else
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-prx";
#endif
#elif defined(CONFIG_MT_BLE_DIAGNOSTICS) && defined(CONFIG_MT_ALLOW_TX) && !defined(CONFIG_MT_NORMAL_ENDPOINT)
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-1td";
#elif defined(CONFIG_MT_BLE_DIAGNOSTICS) && defined(CONFIG_MT_ALLOW_TX)
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-txd";
#elif defined(CONFIG_MT_BLE_DIAGNOSTICS)
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-rxd";
#elif defined(CONFIG_MT_ALLOW_TX) && !defined(CONFIG_MT_NORMAL_ENDPOINT)
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-1tx";
#elif defined(CONFIG_MT_ALLOW_TX)
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-tx";
#else
inline constexpr char FirmwareVersion[]="2.8.2.ZT-m188-rx";
#endif
#endif

#if defined(CONFIG_MT_SIGNED_WIFI_OTA)
using SignedVersionText = char[18]; // Pinned DeviceMetadata and MapReport wire capacity.
const SignedVersionText &firmwareVersion();
#else
inline constexpr auto &firmwareVersion() { return FirmwareVersion; }
#endif
}
