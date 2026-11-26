// SPDX-License-Identifier: GPL-3.0-only
// M108 power metrics adapter: maps 8-channel Observed readings into the
// generated meshtastic_PowerMetrics protobuf. Inputs are already in wire
// units (voltage V, current mA); values are copied unchanged, no conversion.
#pragma once
#include "Observed.h"
#include <float.h>
#include <stddef.h>
#include <string.h>
#include "meshtastic/telemetry.pb.h"

namespace zt::telemetry {

namespace power_detail {
// Ordered bounds reject NaN and infinities without a math runtime.
inline bool isFinite(float x) { return x >= -FLT_MAX && x <= FLT_MAX; }
// Nanopb stores each of the 16 optional floats in an identical presence/value
// pair. Check every member offset before using this fixed wire-structure walk.
using Metrics=meshtastic_PowerMetrics;
constexpr size_t FieldStride=offsetof(Metrics,has_ch1_current);
constexpr size_t ValueOffset=offsetof(Metrics,ch1_voltage);
#define CHECK_POWER_LAYOUT(n) \
    static_assert(offsetof(Metrics,has_ch##n##_voltage)==(2*(n)-2)*FieldStride && \
                  offsetof(Metrics,ch##n##_voltage)==(2*(n)-2)*FieldStride+ValueOffset && \
                  offsetof(Metrics,has_ch##n##_current)==(2*(n)-1)*FieldStride && \
                  offsetof(Metrics,ch##n##_current)==(2*(n)-1)*FieldStride+ValueOffset)
CHECK_POWER_LAYOUT(1);CHECK_POWER_LAYOUT(2);CHECK_POWER_LAYOUT(3);CHECK_POWER_LAYOUT(4);
CHECK_POWER_LAYOUT(5);CHECK_POWER_LAYOUT(6);CHECK_POWER_LAYOUT(7);CHECK_POWER_LAYOUT(8);
#undef CHECK_POWER_LAYOUT
inline void setField(uint8_t *field,const Observed<float> &reading,bool voltage){
    if(!reading || !isFinite(*reading) || (voltage && *reading<0))return;
    const bool present=true;
    memcpy(field,&present,sizeof(present));
    const float value=*reading;
    memcpy(field+ValueOffset,&value,sizeof(value));
}
}  // namespace power_detail

inline meshtastic_PowerMetrics buildPowerMetrics(
    const Observed<float>(&voltage)[8],
    const Observed<float>(&current)[8]) {
  meshtastic_PowerMetrics m{};
  auto *field=reinterpret_cast<uint8_t*>(&m);
  for(unsigned i=0;i<8;++i){
    power_detail::setField(field,voltage[i],true);
    field+=power_detail::FieldStride;
    power_detail::setField(field,current[i],false);
    field+=power_detail::FieldStride;
  }
  return m;
}

inline bool hasPowerMetrics(const meshtastic_PowerMetrics &metrics) {
  const auto *bytes=reinterpret_cast<const uint8_t*>(&metrics);
  for(unsigned i=0;i<16;++i){
    bool present=false;memcpy(&present,bytes+i*power_detail::FieldStride,sizeof(present));
    if(present)return true;
  }
  return false;
}
}  // namespace zt::telemetry
