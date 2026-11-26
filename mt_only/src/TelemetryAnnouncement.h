// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "TelemetryQuery.h"
#include "stock/telemetry/ReportModules.h"
namespace mt {
// Qwen proposal, using the pinned minimal-runtime ordered finite bounds.
// Distinct from response encoding: no query correlation is manufactured.
struct TelemetryAnnouncement {
  static bool encode(uint32_t uptime,const zt::Observed<float>&air,const zt::Observed<float>&channel,
                     uint8_t*out,size_t capacity,size_t&size,const HardwareSnapshot &hardware={},uint32_t utc=0){
    meshtastic_Telemetry report{};
    stock::telemetry::deviceReport(report,utc,uptime,air,channel,hardware);
    return stock::telemetry::encodeReport(report,0,out,capacity,size);
  }
};
}
