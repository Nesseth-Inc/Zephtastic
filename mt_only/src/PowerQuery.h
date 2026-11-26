// SPDX-License-Identifier: GPL-3.0-only
// Canonical power-metrics selector/encoder using the exact adapter contract.
// Caller owns the measured producer and reply admission.
#pragma once
#include "power/PowerReadings.h"
#include "power/PowerMetricsAdapter.h"
#include "RemoteTelemetryQuery.h"
namespace mt {
class PowerQuery {
public:
  static bool request(const uint8_t *payload,size_t size){
    return RemoteTelemetryQuery::requestVariant(payload,size)==meshtastic_Telemetry_power_metrics_tag;
  }
  static bool response(const zt::telemetry::PowerReadings &readings,uint32_t request,
                       uint8_t *out,size_t capacity,size_t &size,uint32_t utc=0){
    size=0;if(!request||!out)return false;
    meshtastic_Telemetry report{};
    return stock::telemetry::powerReport(report,utc,readings) &&
           stock::telemetry::encodeReport(report,request,out,capacity,size);
  }
};
}
