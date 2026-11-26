// SPDX-License-Identifier: GPL-3.0-only
// power_query_cases.cpp — proposed M108 PowerQuery fixture (not executed); real nanopb, no stubs.
#include "PowerQuery.h"
#include "meshtastic/telemetry.pb.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include <assert.h>
#include <stdint.h>
#include <stddef.h>

namespace {

size_t enc(meshtastic_Telemetry &t, uint8_t *buf, size_t cap) {
  pb_ostream_t os = pb_ostream_from_buffer(buf, cap);
  return pb_encode(&os, meshtastic_Telemetry_fields, &t) ? os.bytes_written : 0;
}

// Group 1: power tag 5 accepted; device tag 2, environment tag 3, invalid rejected.
void selector_acceptance() {
  uint8_t buf[64];
  meshtastic_Telemetry p{}; p.which_variant = meshtastic_Telemetry_power_metrics_tag;
  p.variant.power_metrics.has_ch1_voltage = true; p.variant.power_metrics.ch1_voltage = 3.3f;
  p.variant.power_metrics.has_ch1_current = true; p.variant.power_metrics.ch1_current = 0.5f;
  assert(mt::PowerQuery::request(buf, enc(p, buf, sizeof buf)));
  meshtastic_Telemetry d{}; d.which_variant = meshtastic_Telemetry_device_metrics_tag;
  assert(!mt::PowerQuery::request(buf, enc(d, buf, sizeof buf)));
  meshtastic_Telemetry e{}; e.which_variant = meshtastic_Telemetry_environment_metrics_tag;
  assert(!mt::PowerQuery::request(buf, enc(e, buf, sizeof buf)));
  const uint8_t bad[2] = {0x52, 0x00};  // field 10 (invalid variant), len 0
  assert(!mt::PowerQuery::request(bad, sizeof bad));
}

// Group 2: measured response decodes to TELEMETRY_APP Data, request_id kept, has_bitfield, nested tag 5 time 0.
void measured_response_decode() {
  zt::telemetry::PowerReadings r{}; r.voltage[0] = 3.3f; r.current[0] = 0.5f;
  uint8_t out[512]; size_t size = 0;
  assert(mt::PowerQuery::response(r, 0x1234, out, sizeof out, size));
  meshtastic_Data d{}; pb_istream_t is = pb_istream_from_buffer(out, size);
  assert(pb_decode(&is, meshtastic_Data_fields, &d));
  assert(d.portnum == meshtastic_PortNum_TELEMETRY_APP);
  assert(d.request_id == 0x1234);
  assert(d.has_bitfield);
  meshtastic_Telemetry t{}; pb_istream_t is2 = pb_istream_from_buffer(d.payload.bytes, d.payload.size);
  assert(pb_decode(&is2, meshtastic_Telemetry_fields, &t));
  assert(t.which_variant == meshtastic_Telemetry_power_metrics_tag);
  assert(t.time == 0);
}

// Group 3: request ID 0 and all-absent metrics rejected, size stays 0.
void rejects_zero_request_and_absent_metrics() {
  zt::telemetry::PowerReadings r{}; r.voltage[0] = 3.3f;
  uint8_t out[512]; size_t size = 99;
  assert(!mt::PowerQuery::response(r, 0, out, sizeof out, size));
  assert(size == 0);
  zt::telemetry::PowerReadings none{}; size = 99;
  assert(!mt::PowerQuery::response(none, 1, out, sizeof out, size));
  assert(size == 0);
}

// Group 4: null output and tiny buffer rejected, size stays 0.
void rejects_null_and_tiny_buffers() {
  zt::telemetry::PowerReadings r{}; r.voltage[0] = 3.3f;
  uint8_t out[512]; uint8_t tiny[1]; size_t size = 99;
  assert(!mt::PowerQuery::response(r, 1, nullptr, sizeof out, size));
  assert(size == 0);
  size = 99;
  assert(!mt::PowerQuery::response(r, 1, tiny, sizeof tiny, size));
  assert(size == 0);
}

}  // namespace

int main() {
  selector_acceptance();
  measured_response_decode();
  rejects_zero_request_and_absent_metrics();
  rejects_null_and_tiny_buffers();
  return 0;
}
