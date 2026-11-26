// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include "../telemetry/CompletionTelemetry.h"

namespace mt::ui {

struct SelectedPeerForTelemetry {
  uint32_t nodeId = 0;
  char shortName[5]{};
};

struct PeerTelemetryView {
  enum class Freshness : uint8_t { Unavailable, Stale, Fresh };

  uint32_t sourceNodeId = 0;
  char sourceShortName[5]{};
  Freshness airFreshness = Freshness::Unavailable;
  Freshness healthFreshness = Freshness::Unavailable;
  bool hasAir = false;
  bool hasHealth = false;
  // The detail pane paints four air fields; the telemetry owner retains the
  // complete protobuf and remains responsible for observation freshness.
  struct VisibleAir {
    uint32_t pm25_standard=0,pm100_standard=0,co2=0;
    float pm_voc_idx=0;
    bool has_pm25_standard=false,has_pm100_standard=false,has_co2=false,has_pm_voc_idx=false;
    void assign(const meshtastic_AirQualityMetrics &m) {
      pm25_standard=m.pm25_standard;pm100_standard=m.pm100_standard;co2=m.co2;pm_voc_idx=m.pm_voc_idx;
      has_pm25_standard=m.has_pm25_standard;has_pm100_standard=m.has_pm100_standard;
      has_co2=m.has_co2;has_pm_voc_idx=m.has_pm_voc_idx;
    }
  } air{};
  meshtastic_HealthMetrics health{};
};

// Copies only one selected peer from the existing bounded cache. Core
// validation and freshness remain authoritative; no local sensor fallback.
// The current core API returns freshness but not its observation timestamp,
// so this view never turns owner/render time into a sample time.
inline bool projectPeerTelemetry(const telemetry::CompletionTelemetry &cache,
                                 const SelectedPeerForTelemetry &selected,
                                 uint64_t nowMs,
                                 PeerTelemetryView &out) {
  out = {};
  if (!selected.nodeId || nowMs > 0x7fffffffffffffffULL) return false;
  out.sourceNodeId = selected.nodeId;
  for (unsigned i = 0; i < sizeof(out.sourceShortName) - 1 &&
                       selected.shortName[i]; ++i) {
    const unsigned char c = static_cast<unsigned char>(selected.shortName[i]);
    out.sourceShortName[i] = c >= 32 && c < 127 ? static_cast<char>(c) : ' ';
  }

  meshtastic_Telemetry value{};
  const auto air = cache.peerSnapshot(
      selected.nodeId, meshtastic_Telemetry_air_quality_metrics_tag, nowMs,
      value);
  if (air == telemetry::CompletionTelemetry::Result::Ok &&
      value.which_variant == meshtastic_Telemetry_air_quality_metrics_tag) {
    out.air.assign(value.variant.air_quality_metrics);
    out.hasAir = true;
    out.airFreshness = PeerTelemetryView::Freshness::Fresh;
  } else if (air == telemetry::CompletionTelemetry::Result::Stale) {
    out.airFreshness = PeerTelemetryView::Freshness::Stale;
  }

  value = {};
  const auto health = cache.peerSnapshot(
      selected.nodeId, meshtastic_Telemetry_health_metrics_tag, nowMs, value);
  if (health == telemetry::CompletionTelemetry::Result::Ok &&
      value.which_variant == meshtastic_Telemetry_health_metrics_tag) {
    out.health = value.variant.health_metrics;
    out.hasHealth = true;
    out.healthFreshness = PeerTelemetryView::Freshness::Fresh;
  } else if (health == telemetry::CompletionTelemetry::Result::Stale) {
    out.healthFreshness = PeerTelemetryView::Freshness::Stale;
  }
  return true;
}

}  // namespace mt::ui
