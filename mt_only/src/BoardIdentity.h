// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include "meshtastic/mesh.pb.h"

namespace mt::board {
#if defined(CONFIG_BOARD_ADAFRUIT_FEATHER_RFM95_RP2040)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_RP2040_FEATHER_RFM95;
#elif defined(CONFIG_BOARD_HELTEC_T096)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_HELTEC_MESH_NODE_T096;
#elif defined(CONFIG_BOARD_HELTEC_T114_V2) || defined(CONFIG_BOARD_HELTEC_T114)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_HELTEC_MESH_NODE_T114;
#elif defined(CONFIG_BOARD_HELTEC_T1)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_HELTEC_MESH_NODE_T1;
#elif defined(CONFIG_BOARD_HELTEC_WIFI_LORA32_V3)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_HELTEC_V3;
#elif defined(CONFIG_BOARD_HELTEC_WIFI_LORA32_V4_R8)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_HELTEC_V4_R8;
#elif defined(CONFIG_BOARD_HELTEC_WIFI_LORA32_V4) || defined(CONFIG_BOARD_HELTEC_WIFI_LORA32_V43)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_HELTEC_V4;
#elif defined(CONFIG_BOARD_HELTEC_WIRELESS_STICK_LITE_V3)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_HELTEC_WSL_V3;
#elif defined(CONFIG_BOARD_HELTEC_WIRELESS_TRACKER)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_HELTEC_WIRELESS_TRACKER;
#elif defined(CONFIG_BOARD_HELTEC_WIRELESS_TRACKER_V2)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_HELTEC_WIRELESS_TRACKER_V2;
#elif defined(CONFIG_BOARD_LILYGO_T3S3)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_TLORA_T3_S3;
#elif defined(CONFIG_BOARD_LILYGO_TECHO)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_T_ECHO;
#elif defined(CONFIG_BOARD_LILYGO_TIMPULSE_PLUS)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_T_IMPULSE_PLUS;
#elif defined(CONFIG_BOARD_LILYGO_TLORA_C6)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_TLORA_C6;
#elif defined(CONFIG_BOARD_LORA_E5_MINI)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_WIO_E5;
#elif defined(CONFIG_BOARD_M5STACK_CORES3)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_M5STACK_CORES3;
#elif defined(CONFIG_BOARD_M5STACK_UNITC6L)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_M5STACK_C6L;
#elif defined(CONFIG_BOARD_MESHNOLOGY_W12)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_MESHNOLOGY_W12;
#elif defined(CONFIG_BOARD_MESHTRACKER_X1)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_MESH_TRACKER_X1;
#elif defined(CONFIG_BOARD_MUZIWORKS_R1NEO)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_MUZI_R1_NEO;
#elif defined(CONFIG_BOARD_NRF52840DONGLE)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_NRF52840_PCA10059;
#elif defined(CONFIG_BOARD_PROMICRO_SX1262) || defined(CONFIG_BOARD_PROMICRO_LR2021)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_NRF52_PROMICRO_DIY;
#elif defined(CONFIG_BOARD_RAK3172)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_RAK3172;
#elif defined(CONFIG_BOARD_RAK3401_1WATT)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_RAK3401;
#elif defined(CONFIG_BOARD_RAK4631)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_RAK4631;
#elif defined(CONFIG_BOARD_RAK_WISMESH_TAG)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_WISMESH_TAG;
#elif defined(CONFIG_BOARD_RPI_PICO)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_RPI_PICO;
#elif defined(CONFIG_BOARD_RPI_PICO2)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_RPI_PICO2;
#elif defined(CONFIG_BOARD_SENSECAP_SOLAR)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_SEEED_SOLAR_NODE;
#elif defined(CONFIG_BOARD_STATION_G2)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_STATION_G2;
#elif defined(CONFIG_BOARD_T1000_E)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_TRACKER_T1000_E;
#elif defined(CONFIG_BOARD_T_DECK)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_T_DECK;
#elif defined(CONFIG_BOARD_THINKNODE_M1)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_THINKNODE_M1;
#elif defined(CONFIG_BOARD_THINKNODE_M3)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_THINKNODE_M3;
#elif defined(CONFIG_BOARD_THINKNODE_M6)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_THINKNODE_M6;
#elif defined(CONFIG_BOARD_THINKNODE_M9)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_THINKNODE_M9;
#elif defined(CONFIG_BOARD_TTGO_LORA32)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_TLORA_V2_1_1P6;
#elif defined(CONFIG_BOARD_TTGO_TBEAM)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_TBEAM;
#elif defined(CONFIG_BOARD_TWATCH_S3)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_T_WATCH_S3;
#elif defined(CONFIG_BOARD_WIO_TRACKER_L1)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_SEEED_WIO_TRACKER_L1;
#elif defined(CONFIG_BOARD_WIO_TRACKER_L1_1W)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_SEEED_WIO_TRACKER_L1_PRO_1W;
#elif defined(CONFIG_BOARD_WIO_WM1110_DEV_KIT)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_WIO_WM1110;
#elif defined(CONFIG_BOARD_XIAO_ESP32S3)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_SEEED_XIAO_S3;
#elif defined(CONFIG_BOARD_XIAO_NRF52840)
inline constexpr auto DetectedModel = meshtastic_HardwareModel_XIAO_NRF52_KIT;
#else
inline constexpr auto DetectedModel = meshtastic_HardwareModel_UNSET;
#endif

// The 2.8.2 wire enum is contiguous through NODARA. Vendor placeholders
// remain valid for decoding peers but cannot identify a local port.
constexpr bool supportedModel(int model) {
  return (model > meshtastic_HardwareModel_UNSET && model <= meshtastic_HardwareModel_NODARA &&
          model != meshtastic_HardwareModel_MAKERFABS_RESERVED &&
          model != meshtastic_HardwareModel_M5STACK_RESERVED) || model == meshtastic_HardwareModel_PRIVATE_HW;
}

#if defined(CONFIG_MT_HARDWARE_MODEL) && CONFIG_MT_HARDWARE_MODEL != 0
static_assert(supportedModel(CONFIG_MT_HARDWARE_MODEL), "Select an assigned Meshtastic hardware model");
inline constexpr auto Model = static_cast<meshtastic_HardwareModel>(CONFIG_MT_HARDWARE_MODEL);
#else
inline constexpr auto Model = DetectedModel;
#endif

// A custom port may identify its carrier separately from its underlying board.
constexpr uint8_t capabilityModel(uint8_t reported) {
#if defined(CONFIG_MT_HARDWARE_MODEL) && CONFIG_MT_HARDWARE_MODEL != 0
  (void)reported;
  return DetectedModel;
#else
  return reported;
#endif
}

// MyNodeInfo.pio_env selects updater files. Advertise only established targets.
constexpr const char *firmwareTarget(uint8_t model) {
  if (model != capabilityModel(model)) return "";
  if (model == meshtastic_HardwareModel_HELTEC_MESH_NODE_T096) return "mt-zephyr-t096";
  if (model != meshtastic_HardwareModel_SEEED_XIAO_S3) return "";
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  return "mt-zephyr-xiao-198";
#else
  return "mt-zephyr-xiao";
#endif
}

inline constexpr bool HasUpdateTarget = firmwareTarget(Model)[0] != 0;
}
