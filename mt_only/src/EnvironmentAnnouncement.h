// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "TelemetryQuery.h"
#include "environment/EnvironmentMetricsAdapter.h"
#include "stock/telemetry/ReportModules.h"
namespace mt {
struct EnvironmentAnnouncement {
  static bool encode(const zt::telemetry::EnvironmentReadings &readings,uint8_t *out,size_t capacity,size_t &size,uint32_t utc=0){
    meshtastic_Telemetry report{};
    size=0;
    return stock::telemetry::environmentReport(report,utc,readings) &&
           stock::telemetry::encodeReport(report,0,out,capacity,size);
  }
};
}
