// SPDX-License-Identifier: GPL-3.0-only
// M108 power metrics adapter cases: proposed fixture; root integrates and runs.
#include "power/PowerMetricsAdapter.h"

#include <assert.h>
#include <math.h>

using zt::Observed;
using zt::telemetry::buildPowerMetrics;
using zt::telemetry::hasPowerMetrics;

static void casePresentZeroVersusAbsent() {  // group 1: present zero vs absent
  Observed<float> v[8], c[8];  // all channels absent
  meshtastic_PowerMetrics m = buildPowerMetrics(v, c);
  assert(!m.has_ch1_voltage && !m.has_ch1_current && !m.has_ch2_voltage &&
         !m.has_ch2_current && !m.has_ch3_voltage && !m.has_ch3_current &&
         !m.has_ch4_voltage && !m.has_ch4_current && !m.has_ch5_voltage &&
         !m.has_ch5_current && !m.has_ch6_voltage && !m.has_ch6_current &&
         !m.has_ch7_voltage && !m.has_ch7_current && !m.has_ch8_voltage &&
         !m.has_ch8_current);
  assert(!hasPowerMetrics(m));
  v[3] = Observed<float>(0.0f);  // present zero on channel 4
  m = buildPowerMetrics(v, c);
  assert(m.has_ch4_voltage && m.ch4_voltage == 0.0f && !m.has_ch4_current);
  assert(!m.has_ch1_voltage && !m.has_ch3_voltage);
  assert(hasPowerMetrics(m));
}

static void caseAllEightChannelMappings() {  // group 2: index/member mapping
  Observed<float> v[8], c[8];
  for (int i = 0; i < 8; ++i) {
    v[i] = Observed<float>(1.0f + (float)i);
    c[i] = Observed<float>(100.0f + (float)i);
  }
  meshtastic_PowerMetrics m = buildPowerMetrics(v, c);
  assert(m.ch1_voltage == 1.0f && m.has_ch1_voltage && m.ch2_voltage == 2.0f && m.has_ch2_voltage);
  assert(m.ch3_voltage == 3.0f && m.has_ch3_voltage && m.ch4_voltage == 4.0f && m.has_ch4_voltage);
  assert(m.ch5_voltage == 5.0f && m.has_ch5_voltage && m.ch6_voltage == 6.0f && m.has_ch6_voltage);
  assert(m.ch7_voltage == 7.0f && m.has_ch7_voltage && m.ch8_voltage == 8.0f && m.has_ch8_voltage);
  assert(m.ch1_current == 100.0f && m.has_ch1_current && m.ch2_current == 101.0f && m.has_ch2_current);
  assert(m.ch3_current == 102.0f && m.has_ch3_current && m.ch4_current == 103.0f && m.has_ch4_current);
  assert(m.ch5_current == 104.0f && m.has_ch5_current && m.ch6_current == 105.0f && m.has_ch6_current);
  assert(m.ch7_current == 106.0f && m.has_ch7_current && m.ch8_current == 107.0f && m.has_ch8_current);
  assert(hasPowerMetrics(m));
}

static void caseNonFiniteAndNegativeVoltageOmitted() {  // group 3: omit bad values
  Observed<float> v[8], c[8];
  v[0] = Observed<float>(NAN);
  v[1] = Observed<float>(INFINITY);
  v[2] = Observed<float>(-INFINITY);
  v[3] = Observed<float>(-5.0f);
  c[4] = Observed<float>(NAN);
  c[5] = Observed<float>(INFINITY);
  c[6] = Observed<float>(-INFINITY);
  meshtastic_PowerMetrics m = buildPowerMetrics(v, c);
  assert(!m.has_ch1_voltage && !m.has_ch2_voltage && !m.has_ch3_voltage &&
         !m.has_ch4_voltage && !m.has_ch5_voltage && !m.has_ch6_voltage &&
         !m.has_ch7_voltage && !m.has_ch8_voltage);
  assert(!m.has_ch1_current && !m.has_ch2_current && !m.has_ch3_current &&
         !m.has_ch4_current && !m.has_ch5_current && !m.has_ch6_current &&
         !m.has_ch7_current && !m.has_ch8_current);
  assert(!hasPowerMetrics(m));
}

static void caseNegativeCurrentRetainedExactly() {  // group 4: signed current
  Observed<float> v[8], c[8];
  c[0] = Observed<float>(-12.5f);
  c[7] = Observed<float>(-0.5f);
  meshtastic_PowerMetrics m = buildPowerMetrics(v, c);
  assert(m.has_ch1_current && m.ch1_current == -12.5f && m.ch1_current < 0.0f);
  assert(m.has_ch8_current && m.ch8_current == -0.5f && m.ch8_current < 0.0f);
  assert(!m.has_ch1_voltage && !m.has_ch8_voltage);
  assert(hasPowerMetrics(m));
}

int main() {
  casePresentZeroVersusAbsent();
  caseAllEightChannelMappings();
  caseNonFiniteAndNegativeVoltageOmitted();
  caseNegativeCurrentRetainedExactly();
  return 0;
}
