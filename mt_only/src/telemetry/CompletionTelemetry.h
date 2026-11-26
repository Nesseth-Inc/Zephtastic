// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <cstddef>
#include "meshtastic/module_config.pb.h"
#include "meshtastic/telemetry.pb.h"

namespace mt::telemetry {
// Producer for the stock TelemetryConfig air-quality and health variants.
// Acquisition is strictly from optional Zephyr DT aliases; absence is unavailable.
class CompletionTelemetry {
public:
  struct Snapshot {
    meshtastic_AirQualityMetrics air{};
    meshtastic_HealthMetrics health{};
    bool hasAir=false,hasHealth=false;
    uint64_t sampledAtMs=0;
  };
  struct PeerSnapshot {
    uint32_t nodeId=0; meshtastic_AirQualityMetrics air{}; meshtastic_HealthMetrics health{};
    uint64_t airAtMs=0,healthAtMs=0;
    uint32_t airUtc=0,healthUtc=0; bool hasAir=false,hasHealth=false;
  };
  struct Capabilities { bool hasAir=false,hasHealth=false; };
  enum class Result : uint8_t { Ok, InvalidConfig, Disabled, Unavailable, Stale, InvalidRequest };
private:
  meshtastic_ModuleConfig_TelemetryConfig config_{};
  Snapshot latest_{};
  PeerSnapshot peers_[8]{};
  uint8_t nextPeer_=0;
  bool configured_=false;
public:
  bool configure(const meshtastic_ModuleConfig_TelemetryConfig &config);
  bool configured() const { return configured_; }
  Capabilities capabilities() const;
  bool configSupported(const meshtastic_ModuleConfig_TelemetryConfig &config) const;
  const meshtastic_ModuleConfig_TelemetryConfig &config() const { return config_; }
  // Samples only configured and present devices. No synthesized zero/default readings.
  Result poll(uint64_t nowMs);
  Result snapshot(uint64_t nowMs, Snapshot &out) const;
  // Cache only canonical, finite peer-reported air/health metrics. This cache
  // is bounded, unauthenticated observation and never becomes a local reading.
  bool observePeer(uint32_t nodeId,const uint8_t *payload,size_t size,uint64_t nowMs);
  Result peerSnapshot(uint32_t nodeId,uint8_t variant,uint64_t nowMs,meshtastic_Telemetry &out) const;
  Result response(uint8_t variant, uint32_t requestId, uint64_t nowMs, uint32_t utc,
                  uint8_t *out, size_t capacity, size_t &size) const;
};
} // namespace mt::telemetry
