// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// IO and storage adapters for the pinned stock AdminModule Device case and
// NodeDB role defaults. Only the local config replacement is under test.
namespace stock_device {
struct Admin {
  struct {
    bool has_device = false;
    meshtastic_Config_DeviceConfig device{};
    meshtastic_Config_PositionConfig position{};
  } config;
  struct {
    meshtastic_ModuleConfig_TelemetryConfig telemetry{};
    meshtastic_ModuleConfig_NeighborInfoConfig neighbor_info{};
    meshtastic_ModuleConfig_StoreForwardConfig store_forward{};
  } moduleConfig;
  meshtastic_User owner{};

  static constexpr uint32_t ONE_DAY = 86400, MAX_INTERVAL = INT32_MAX;
  static constexpr uint32_t default_telemetry_broadcast_interval_secs = 3600;

  void installRoleDefaults(meshtastic_Config_DeviceConfig_Role role) {
#include "donor.inc"
  }

  void apply(const meshtastic_Config &c) {
    const auto existingRole = config.device.role;
    bool requiresReboot = true;
    unsigned changes = 0;
    auto *nodeDB = this;
    auto sendWarning = [](const char *) {};
    enum { SEGMENT_MODULECONFIG = 2, SEGMENT_DEVICESTATE = 4, SEGMENT_NODEDATABASE = 16 };
#define ARCH_PORTDUINO 1
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define IS_ONE_OF(value, a, b) ((value) == (a) || (value) == (b))
#include "stock-nodeinfo-floor.inc"
    switch (c.which_payload_variant) {
#include "stock-device-admin.inc"
    }
#undef min_node_info_broadcast_secs
#undef IS_ONE_OF
#undef LOG_DEBUG
#undef LOG_WARN
#undef LOG_INFO
#undef ARCH_PORTDUINO
    (void)requiresReboot;
    (void)changes;
  }
};
} // namespace stock_device
