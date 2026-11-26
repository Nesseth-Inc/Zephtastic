// SPDX-License-Identifier: MIT
#pragma once
#include "GnssPromotionEvent.h"
#include "../stock/modules/PositionModule.h"
#include "GnssMeasurementUnits.h"
#include "meshtastic/mesh.pb.h"
namespace zt::gnss {
// After validated geographic/freshness projection, match the pinned stock
// PositionModule flag selection. No RTC update or synthetic optional values.
inline void projectMeasurements(const PromotionEvent &fix,uint32_t flags,meshtastic_Position &out) {
  struct Readings {
    const PromotionEvent &f;
    bool measured,hasUtc,hasGroundTrack;
    uint32_t satellites,utcSeconds,groundTrackE5;
    int32_t msl()const{return millimetersToMeters(f.altitudeMm);}
    int32_t geoid()const{return millimetersToMeters(f.geoidMm);}
    int32_t hae()const{return int32_t((int64_t(f.altitudeMm)+f.geoidMm)/1000);}
    uint32_t hdop()const{return f.hdopMilli/10u;}
    uint32_t speed()const{return millimetersPerSecondToKmh(f.speedMmPerSecond);}
  };
  mt::stock::position::selectPositionFields(Readings{fix,fix.measured,fix.hasUtc,fix.hasGroundTrack,
      fix.satellites,fix.utcSeconds,fix.groundTrackE5},flags,out);
}
}
