// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace mt::power::ina {

enum class Status : uint8_t {
  Disabled,
  UnsupportedAddress,
  NotReady,
  NoSample,
  ReadError,
  Stale,
  Fresh,
};

struct Snapshot {
  Status status=Status::Disabled;
  uint32_t address=0;
  float busVoltageV=0.0f;
  float currentA=0.0f; // Positive/negative direction is reported by the driver.
  uint64_t sampledAtMs=0;
  bool available()const{return status==Status::Fresh;}
};

// Address zero disables the optional INA source and restores the caller's
// existing board ADC path. supports() is devicetree-only and performs no I/O.
bool supports(uint32_t address);
// Explicit opt-in. A nonzero selection may initialize only the matching,
// actually declared deferred sensor node. Calls are serialized by the main
// owner; this module has no internal lock.
bool configure(uint32_t address);
// Fetches at most once per second. Must run on the same serialized owner as
// environment sensor sampling. No I2C access occurs while disabled.
void poll(uint64_t nowMs);
// A sample older than five seconds, a backwards clock, or an invalid time is
// reported as absent; voltage/current values are zeroed in that case.
Snapshot snapshot(uint64_t nowMs);

} // namespace mt::power::ina
