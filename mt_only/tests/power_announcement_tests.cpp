// SPDX-License-Identifier: GPL-3.0-only
#include "PowerAnnouncement.h"
#include "ModuleAnnouncement.h"
#include "pb_decode.h"
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

// Group 1: known-zero voltage and signed current round-trip with or without UTC.
void test_known_zero_voltage_signed_current(uint32_t utc) {
  zt::telemetry::PowerReadings r{};
  r.voltage[0] = zt::Observed<float>(0.0f);
  r.current[0] = zt::Observed<float>(-0.5f);
  uint8_t buf[512];
  size_t size = 0;
  assert(mt::PowerAnnouncement::encode(r, buf, sizeof(buf), size, utc));
  bool telemetry=false;assert(mt::ModuleAnnouncement::canonical(buf,size,telemetry)&&telemetry);
  meshtastic_Data d{};
  auto dataInput = pb_istream_from_buffer(buf, size);
  assert(pb_decode(&dataInput, meshtastic_Data_fields, &d) && !dataInput.bytes_left);
  assert(d.portnum == meshtastic_PortNum_TELEMETRY_APP);
  assert(d.has_bitfield);
  meshtastic_Telemetry t{};
  auto telemetryInput = pb_istream_from_buffer(d.payload.bytes, d.payload.size);
  assert(pb_decode(&telemetryInput, meshtastic_Telemetry_fields, &t) && !telemetryInput.bytes_left);
  assert(t.which_variant == meshtastic_Telemetry_power_metrics_tag);
  assert(t.time == utc);
  const auto &m = t.variant.power_metrics;
  assert(m.has_ch1_voltage && m.ch1_voltage == 0.0f);
  assert(m.has_ch1_current && m.ch1_current == -0.5f);
  for(unsigned mode=0;mode<3;++mode){
    auto changed=t;
    if(mode==0)changed.variant.power_metrics.ch1_voltage=-1;
    if(mode==1)changed.variant.power_metrics.ch1_current=NAN;
    if(mode==2)changed.variant.power_metrics={};
    auto payload=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));
    assert(pb_encode(&payload,meshtastic_Telemetry_fields,&changed));d.payload.size=payload.bytes_written;
    auto stream=pb_ostream_from_buffer(buf,sizeof(buf));assert(pb_encode(&stream,meshtastic_Data_fields,&d));
    assert(!mt::ModuleAnnouncement::canonical(buf,stream.bytes_written,telemetry));
  }

}

// Group 2: absent metrics reject, size stays 0.
void test_absent_metrics_reject() {
  zt::telemetry::PowerReadings r{};
  uint8_t buf[512];
  size_t size = 0xdeadbeef;
  assert(!mt::PowerAnnouncement::encode(r, buf, sizeof(buf), size));
  assert(size == 0);
}

// Group 3: NaN/Inf/negative voltage omitted, valid sibling preserved.
void test_invalid_omitted_sibling_preserved() {
  zt::telemetry::PowerReadings r{};
  r.voltage[0] = zt::Observed<float>(NAN);
  r.current[0] = zt::Observed<float>(INFINITY);
  r.voltage[1] = zt::Observed<float>(-1.0f);
  r.current[1] = zt::Observed<float>(2.5f);
  uint8_t buf[512];
  size_t size = 0;
  assert(mt::PowerAnnouncement::encode(r, buf, sizeof(buf), size));
  bool telemetry=false;assert(mt::ModuleAnnouncement::canonical(buf,size,telemetry)&&telemetry);
  meshtastic_Data d{};
  auto dataInput = pb_istream_from_buffer(buf, size);
  assert(pb_decode(&dataInput, meshtastic_Data_fields, &d) && !dataInput.bytes_left);
  meshtastic_Telemetry t{};
  auto telemetryInput = pb_istream_from_buffer(d.payload.bytes, d.payload.size);
  assert(pb_decode(&telemetryInput, meshtastic_Telemetry_fields, &t) && !telemetryInput.bytes_left);
  const auto &m = t.variant.power_metrics;
  assert(!m.has_ch1_voltage);
  assert(!m.has_ch1_current);
  assert(!m.has_ch2_voltage);
  assert(m.has_ch2_current && m.ch2_current == 2.5f);
}

// Group 4: null/tiny output rejects, size stays 0.
void test_null_tiny_output_reject() {
  zt::telemetry::PowerReadings r{};
  r.voltage[0] = zt::Observed<float>(0.0f);
  r.current[0] = zt::Observed<float>(-0.5f);
  size_t size = 0xdeadbeef;
  assert(!mt::PowerAnnouncement::encode(r, nullptr, 0, size));
  assert(size == 0);
  uint8_t tiny[1];
  size = 0xdeadbeef;
  assert(!mt::PowerAnnouncement::encode(r, tiny, sizeof(tiny), size));
  assert(size == 0);
}

}  // namespace

int main() {
  test_known_zero_voltage_signed_current(0);
  test_known_zero_voltage_signed_current(1700000001u);
  test_absent_metrics_reject();
  test_invalid_omitted_sibling_preserved();
  test_null_tiny_output_reject();
  return 0;
}
