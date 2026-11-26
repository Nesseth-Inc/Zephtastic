// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "power/PowerReadings.h"
#include "power/PowerMetricsAdapter.h"
#include "meshtastic/mesh.pb.h"
#include "pb_encode.h"
#include "stock/telemetry/ReportModules.h"
namespace mt {
struct PowerAnnouncement {
  static bool encode(const zt::telemetry::PowerReadings &readings, uint8_t *out, size_t capacity, size_t &size,uint32_t utc=0) {
    meshtastic_Telemetry report{};
    size=0;
    return stock::telemetry::powerReport(report,utc,readings) &&
           stock::telemetry::encodeReport(report,0,out,capacity,size);
  }
};
}
