// SPDX-License-Identifier: GPL-3.0-only
// Proposed M108 fixture for the exact assignPowerChannel contract.
#include "power/PowerDonorReading.h"
#include <assert.h>
#include <limits.h>
#include <math.h>

namespace {
using zt::telemetry::PowerReadings;
using zt::telemetry::assignPowerChannel;

env_sensor_reading donor(unsigned fields, float v, float a) {
  env_sensor_reading r{};
  r.fields = (uint16_t)fields;
  r.voltage_v = v;
  r.current_a = a;
  return r;
}

void expectSame(const PowerReadings &l, const PowerReadings &r) {
  for (unsigned i = 0; i < 8u; ++i) {
    const bool lv = l.voltage[i].has_value(), lc = l.current[i].has_value();
    assert(lv == r.voltage[i].has_value() && lc == r.current[i].has_value());
    if (lv) assert(*l.voltage[i] == *r.voltage[i]);
    if (lc) assert(*l.current[i] == *r.current[i]);
  }
}

void case_exact_milliampere_conversion() {
  PowerReadings p{};
  assert(assignPowerChannel(p, 0u, donor(ENV_F_VOLTAGE | ENV_F_CURRENT, 3.3f, 0.125f)));
  assert(p.voltage[0].has_value() && *p.voltage[0] == 3.3f);
  assert(p.current[0].has_value() && *p.current[0] == 125.0f);
  assert(assignPowerChannel(p, 7u, donor(ENV_F_CURRENT, 0.0f, -0.125f)));
  assert(!p.voltage[7].has_value());
  assert(p.current[7].has_value() && *p.current[7] == -125.0f);
}

void case_missing_flags_clear_only_selected_pair() {
  PowerReadings p{};
  for (unsigned i = 0; i < 8u; ++i) {
    p.voltage[i] = 1.0f + i;
    p.current[i] = 10.0f + i;
  }
  const PowerReadings before = p;
  assert(assignPowerChannel(p, 3u, donor(0u, 5.0f, 0.5f)));
  assert(!p.voltage[3].has_value() && !p.current[3].has_value());
  PowerReadings after = p;
  after.voltage[3] = before.voltage[3];
  after.current[3] = before.current[3];
  expectSame(after, before);
}

void case_out_of_range_index_is_no_op() {
  PowerReadings p{};
  for (unsigned i = 0; i < 8u; ++i) {
    p.voltage[i] = 2.0f;
    p.current[i] = 200.0f;
  }
  const PowerReadings before = p;
  assert(!assignPowerChannel(p, 8u, donor(ENV_F_VOLTAGE | ENV_F_CURRENT, 9.0f, 0.9f)));
  expectSame(p, before);
  assert(!assignPowerChannel(p, UINT_MAX, donor(ENV_F_VOLTAGE | ENV_F_CURRENT, 9.0f, 0.9f)));
  expectSame(p, before);
}

void case_invalid_values_are_omitted() {
  PowerReadings p{};
  assert(assignPowerChannel(p, 0u, donor(ENV_F_VOLTAGE, -1.0f, 0.0f)));
  assert(!p.voltage[0].has_value() && !p.current[0].has_value());
  assert(assignPowerChannel(p, 1u, donor(ENV_F_CURRENT, 0.0f, NAN)));
  assert(!p.current[1].has_value());
  assert(assignPowerChannel(p, 2u, donor(ENV_F_CURRENT, 0.0f, INFINITY)));
  assert(!p.current[2].has_value());
  assert(assignPowerChannel(p, 3u, donor(ENV_F_CURRENT, 0.0f, 1e38f)));
  assert(!p.current[3].has_value());
  assert(assignPowerChannel(p, 4u, donor(ENV_F_VOLTAGE | ENV_F_CURRENT, 3.0f, NAN)));
  assert(p.voltage[4].has_value() && *p.voltage[4] == 3.0f);
  assert(!p.current[4].has_value());
}
}  // namespace

int main() {
  case_exact_milliampere_conversion();
  case_missing_flags_clear_only_selected_pair();
  case_out_of_range_index_is_no_op();
  case_invalid_values_are_omitted();
}
