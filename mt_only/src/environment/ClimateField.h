// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "EnvironmentSource.h"
#include "Observed.h"

namespace zt::telemetry {
class ClimateField {
 public:
  ClimateField() = default;
  ClimateField(float value) : value_(value), present_(true) {}
  ClimateField(Observed<float> sample) {
    if (sample) *this = ClimateField(*sample);
  }

  bool has_value() const { return present_; }
  explicit operator bool() const { return present_; }
  const float &operator*() const { return value_; }

  void record(EnvironmentSource source, float value) {
    if (!accepts(source)) return;
    value_ = value;
    source_ = source;
    present_ = true;
  }
  void clear(EnvironmentSource source) {
    if (!accepts(source)) return;
    source_ = source;
    present_ = false;
  }

 private:
  bool accepts(EnvironmentSource source) const {
    // The stock AHT handler fills only an absent field.
    if (source == EnvironmentSource::Aht && present_) return false;
    return source > source_ || (source == source_ && !present_);
  }
  float value_ = 0;
  EnvironmentSource source_ = EnvironmentSource::Unspecified;
  bool present_ = false;
};
static_assert(sizeof(ClimateField) == sizeof(Observed<float>));
}
