// SPDX-License-Identifier: GPL-3.0-only
#include "EnvironmentQuery.h"
#include "environment/EnvironmentDonorReading.h"
#include "environment/EnvironmentSnapshot.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace zt::telemetry;
int main(){
  const uint8_t selector[]={0x1a,0},device[]={0x12,0},stats[]={0x32,0},unknown[]={0x1a,0,0x78,1};
  assert(mt::EnvironmentQuery::request(selector,sizeof(selector)));
  assert(!mt::EnvironmentQuery::request(nullptr,0)&&!mt::EnvironmentQuery::request(device,2));
  assert(!mt::EnvironmentQuery::request(stats,2)&&!mt::EnvironmentQuery::request(unknown,4));
  env_sensor_reading donor{};donor.temperature_c=24;donor.humidity_pct=50;donor.pressure_hpa=1013;
  auto readings=fromDonorReading(donor);assert(!readings.ambientCelsius&&!readings.humidityPercent&&!readings.pressureHpa);
  donor.fields=ENV_F_TEMPERATURE|ENV_F_HUMIDITY|ENV_F_PRESSURE;donor.temperature_c=donor.humidity_pct=0;
  readings=fromDonorReading(donor);assert(readings.ambientCelsius&&*readings.ambientCelsius==0&&readings.humidityPercent&&*readings.humidityPercent==0&&*readings.pressureHpa==1013);
  auto metrics=toMeshtasticEnvironmentMetrics(readings);uint8_t bytes[128]{};size_t size=77;
  assert(mt::EnvironmentQuery::responseMetrics(metrics,123,bytes,sizeof(bytes),size));
  meshtastic_Data data{};auto wire=pb_istream_from_buffer(bytes,size);assert(pb_decode(&wire,meshtastic_Data_fields,&data)&&!wire.bytes_left);
  assert(data.portnum==meshtastic_PortNum_TELEMETRY_APP&&data.request_id==123&&data.has_bitfield&&!data.bitfield&&!data.want_response);
  meshtastic_Telemetry telemetry{};auto payload=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
  assert(pb_decode(&payload,meshtastic_Telemetry_fields,&telemetry)&&telemetry.which_variant==3);
  assert(telemetry.variant.environment_metrics.has_temperature&&telemetry.variant.environment_metrics.temperature==0);
  assert(telemetry.variant.environment_metrics.has_relative_humidity&&telemetry.variant.environment_metrics.relative_humidity==0);
  assert(telemetry.variant.environment_metrics.has_barometric_pressure&&telemetry.variant.environment_metrics.barometric_pressure==1013);
  assert(mt::EnvironmentQuery::request(data.payload.bytes,data.payload.size)); // Canonical warm-cache snapshot selects, never publishes.
  assert(!mt::EnvironmentQuery::responseMetrics(metrics,0,bytes,sizeof(bytes),size)&&!size);
  assert(!mt::EnvironmentQuery::responseMetrics(metrics,123,nullptr,sizeof(bytes),size)&&!size);
  assert(!mt::EnvironmentQuery::responseMetrics(metrics,123,bytes,1,size)&&!size);
  metrics={};assert(!mt::EnvironmentQuery::responseMetrics(metrics,123,bytes,sizeof(bytes),size)&&!size);
  metrics.has_temperature=true;metrics.temperature=std::numeric_limits<float>::quiet_NaN();
  assert(!mt::EnvironmentQuery::responseMetrics(metrics,123,bytes,sizeof(bytes),size)&&!size);
  EnvironmentSnapshot snapshot;EnvironmentReadings value;value.ambientCelsius=9;
  assert(!snapshot.snapshot(0,30,value)&&!value.ambientCelsius);
  snapshot.publish(readings,0);assert(snapshot.snapshot(30,30,value)&&*value.pressureHpa==1013);
  assert(!snapshot.snapshot(31,30,value)&&!value.ambientCelsius&&!value.pressureHpa);
  snapshot.publish(readings,100);assert(!snapshot.snapshot(99,30,value));
  snapshot.publish(readings,UINT64_MAX);assert(snapshot.snapshot(UINT64_MAX,0,value));
  snapshot.invalidate();assert(!snapshot.snapshot(UINT64_MAX,UINT64_MAX,value)&&!value.pressureHpa);
  puts("PASS environment adapters: canonical cold/warm selectors, measured flags and zeros, hPa, nested stock Data/Telemetry, failure size, invalid omission and fresh/future/stale/overflow invalidation");
}
