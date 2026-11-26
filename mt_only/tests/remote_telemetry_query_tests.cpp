// SPDX-License-Identifier: GPL-3.0-only
#include "telemetry_query_fixture.h"
#include "../src/pki/PrivateApplication.h"
#include <limits>
int main(){std::cout.setf(std::ios::unitbuf);
 {Fixture f;beginQuery(f);f.rx(goodFrame(),500);f.clients();assert(telemetries(f.h)==1&&results(f.h,77,0));}
 group("existing DeviceMetrics query and delivery remains compatible with the generalized variant path");
 for(uint8_t tag:{2,3,4,5,6,7,8,9,11}){
  meshtastic_Telemetry t{};t.which_variant=tag;t.time=1792660000;
  if(tag==2){t.variant.device_metrics.has_uptime_seconds=true;t.variant.device_metrics.uptime_seconds=1;}
  const auto response=encodeTelemetry(t);meshtastic_Telemetry decoded{};
  assert(mt::RemoteTelemetryQuery::decodeCanonical(response.data(),response.size(),tag,decoded)&&decoded.time==t.time);
  assert(!mt::RemoteTelemetryQuery::requestVariant(response.data(),response.size()));
  t.time=0;auto selector=encodeTelemetry(t);assert(mt::RemoteTelemetryQuery::requestVariant(selector.data(),selector.size())==tag);
  for(auto model:{81,127})for(uint8_t owner:{0,1}){
   Fixture f(true,model);auto q=query();q.packet.decoded.payload.size=selector.size();memcpy(q.packet.decoded.payload.bytes,selector.data(),selector.size());
   input(owner?f.bc:f.c,q,100);f.s.poll(300,0);assert(f.radio.sends==1);f.done(301);
   bench::RouteMetadata outgoing{};assert(f.s.codec.decode(f.radio.frames[0].data(),f.radio.frames[0].size(),outgoing));
   assert(outgoing.wantResponse&&outgoing.payloadLength==2&&outgoing.payload[0]==uint8_t((tag<<3)|2)&&!outgoing.payload[1]);
   meshtastic_Telemetry wrong{};wrong.which_variant=tag==3?4:3;
   f.rx(rawFrame(encodeData(encodeTelemetry(wrong)),PEER,OURS,700),500);assert(!telemetries(f.h)&&!telemetries(f.bh));
   f.rx(rawFrame(encodeData(response),PEER,OURS,701),600);f.clients();assert(telemetries(owner?f.bh:f.h)==1&&!telemetries(owner?f.h:f.bh));
   f.rx(rawFrame(encodeData(response),PEER,OURS,701),700);assert(telemetries(owner?f.bh:f.h)==1);
  }
 }
 group("all nine pinned telemetry variants: canonical empty RF selector, response timestamp, exact variant correlation and owner-only delivery");
 for(uint8_t tag:{0,1,10,12,31,127})assert(!mt::RemoteTelemetryQuery::supportedVariant(tag));
 {uint8_t extra[]={0x1a,0,8,0};meshtastic_Telemetry t{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(extra,sizeof(extra),3,t));}
 group("unset/time/gap/unknown oneofs and duplicate or noncanonical protobuf encodings rejected");
 {for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}){
  {meshtastic_Telemetry t{};t.which_variant=2;t.variant.device_metrics.has_voltage=true;t.variant.device_metrics.voltage=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),2,out));}
  {meshtastic_Telemetry t{};t.which_variant=2;t.variant.device_metrics.has_channel_utilization=true;t.variant.device_metrics.channel_utilization=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),2,out));}
  {meshtastic_Telemetry t{};t.which_variant=2;t.variant.device_metrics.has_air_util_tx=true;t.variant.device_metrics.air_util_tx=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),2,out));}
 }}
 group("DeviceMetrics: every floating field rejects NaN and both infinities");
 {for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}){
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_temperature=true;t.variant.environment_metrics.temperature=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_relative_humidity=true;t.variant.environment_metrics.relative_humidity=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_barometric_pressure=true;t.variant.environment_metrics.barometric_pressure=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_gas_resistance=true;t.variant.environment_metrics.gas_resistance=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_voltage=true;t.variant.environment_metrics.voltage=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_current=true;t.variant.environment_metrics.current=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_distance=true;t.variant.environment_metrics.distance=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_lux=true;t.variant.environment_metrics.lux=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_white_lux=true;t.variant.environment_metrics.white_lux=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_ir_lux=true;t.variant.environment_metrics.ir_lux=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_uv_lux=true;t.variant.environment_metrics.uv_lux=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_wind_speed=true;t.variant.environment_metrics.wind_speed=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_weight=true;t.variant.environment_metrics.weight=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_wind_gust=true;t.variant.environment_metrics.wind_gust=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_wind_lull=true;t.variant.environment_metrics.wind_lull=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_radiation=true;t.variant.environment_metrics.radiation=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_rainfall_1h=true;t.variant.environment_metrics.rainfall_1h=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_rainfall_24h=true;t.variant.environment_metrics.rainfall_24h=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_soil_temperature=true;t.variant.environment_metrics.soil_temperature=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_adc_voltage_ch0=true;t.variant.environment_metrics.adc_voltage_ch0=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_adc_voltage_ch1=true;t.variant.environment_metrics.adc_voltage_ch1=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_adc_voltage_ch2=true;t.variant.environment_metrics.adc_voltage_ch2=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_adc_voltage_ch3=true;t.variant.environment_metrics.adc_voltage_ch3=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_adc_voltage_ch4=true;t.variant.environment_metrics.adc_voltage_ch4=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_adc_voltage_ch5=true;t.variant.environment_metrics.adc_voltage_ch5=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_adc_voltage_ch6=true;t.variant.environment_metrics.adc_voltage_ch6=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_adc_voltage_ch7=true;t.variant.environment_metrics.adc_voltage_ch7=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_one_wire_temperature_ch0=true;t.variant.environment_metrics.one_wire_temperature_ch0=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_one_wire_temperature_ch1=true;t.variant.environment_metrics.one_wire_temperature_ch1=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_one_wire_temperature_ch2=true;t.variant.environment_metrics.one_wire_temperature_ch2=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_one_wire_temperature_ch3=true;t.variant.environment_metrics.one_wire_temperature_ch3=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_one_wire_temperature_ch4=true;t.variant.environment_metrics.one_wire_temperature_ch4=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_one_wire_temperature_ch5=true;t.variant.environment_metrics.one_wire_temperature_ch5=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_one_wire_temperature_ch6=true;t.variant.environment_metrics.one_wire_temperature_ch6=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_one_wire_temperature_ch7=true;t.variant.environment_metrics.one_wire_temperature_ch7=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
  {meshtastic_Telemetry t{};t.which_variant=3;t.variant.environment_metrics.has_lightning_distance_km=true;t.variant.environment_metrics.lightning_distance_km=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),3,out));}
 }}
 group("EnvironmentMetrics: every floating field rejects NaN and both infinities");
 {for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}){
  {meshtastic_Telemetry t{};t.which_variant=4;t.variant.air_quality_metrics.has_co2_temperature=true;t.variant.air_quality_metrics.co2_temperature=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),4,out));}
  {meshtastic_Telemetry t{};t.which_variant=4;t.variant.air_quality_metrics.has_co2_humidity=true;t.variant.air_quality_metrics.co2_humidity=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),4,out));}
  {meshtastic_Telemetry t{};t.which_variant=4;t.variant.air_quality_metrics.has_form_formaldehyde=true;t.variant.air_quality_metrics.form_formaldehyde=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),4,out));}
  {meshtastic_Telemetry t{};t.which_variant=4;t.variant.air_quality_metrics.has_form_humidity=true;t.variant.air_quality_metrics.form_humidity=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),4,out));}
  {meshtastic_Telemetry t{};t.which_variant=4;t.variant.air_quality_metrics.has_form_temperature=true;t.variant.air_quality_metrics.form_temperature=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),4,out));}
  {meshtastic_Telemetry t{};t.which_variant=4;t.variant.air_quality_metrics.has_pm_temperature=true;t.variant.air_quality_metrics.pm_temperature=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),4,out));}
  {meshtastic_Telemetry t{};t.which_variant=4;t.variant.air_quality_metrics.has_pm_humidity=true;t.variant.air_quality_metrics.pm_humidity=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),4,out));}
  {meshtastic_Telemetry t{};t.which_variant=4;t.variant.air_quality_metrics.has_pm_voc_idx=true;t.variant.air_quality_metrics.pm_voc_idx=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),4,out));}
  {meshtastic_Telemetry t{};t.which_variant=4;t.variant.air_quality_metrics.has_pm_nox_idx=true;t.variant.air_quality_metrics.pm_nox_idx=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),4,out));}
  {meshtastic_Telemetry t{};t.which_variant=4;t.variant.air_quality_metrics.has_particles_tps=true;t.variant.air_quality_metrics.particles_tps=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),4,out));}
 }}
 group("AirQualityMetrics: every floating field rejects NaN and both infinities");
 {for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}){
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch1_voltage=true;t.variant.power_metrics.ch1_voltage=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch1_current=true;t.variant.power_metrics.ch1_current=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch2_voltage=true;t.variant.power_metrics.ch2_voltage=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch2_current=true;t.variant.power_metrics.ch2_current=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch3_voltage=true;t.variant.power_metrics.ch3_voltage=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch3_current=true;t.variant.power_metrics.ch3_current=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch4_voltage=true;t.variant.power_metrics.ch4_voltage=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch4_current=true;t.variant.power_metrics.ch4_current=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch5_voltage=true;t.variant.power_metrics.ch5_voltage=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch5_current=true;t.variant.power_metrics.ch5_current=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch6_voltage=true;t.variant.power_metrics.ch6_voltage=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch6_current=true;t.variant.power_metrics.ch6_current=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch7_voltage=true;t.variant.power_metrics.ch7_voltage=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch7_current=true;t.variant.power_metrics.ch7_current=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch8_voltage=true;t.variant.power_metrics.ch8_voltage=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
  {meshtastic_Telemetry t{};t.which_variant=5;t.variant.power_metrics.has_ch8_current=true;t.variant.power_metrics.ch8_current=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),5,out));}
 }}
 group("PowerMetrics: every floating field rejects NaN and both infinities");
 {for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}){
  {meshtastic_Telemetry t{};t.which_variant=6;t.variant.local_stats.channel_utilization=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),6,out));}
  {meshtastic_Telemetry t{};t.which_variant=6;t.variant.local_stats.air_util_tx=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),6,out));}
 }}
 group("LocalStats: every floating field rejects NaN and both infinities");
 {for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}){
  {meshtastic_Telemetry t{};t.which_variant=7;t.variant.health_metrics.has_temperature=true;t.variant.health_metrics.temperature=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),7,out));}
 }}
 group("HealthMetrics: every floating field rejects NaN and both infinities");
 {for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}){
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_soil_ph=true;t.variant.soil_water_metrics.soil_ph=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_ph=true;t.variant.soil_water_metrics.ph=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_electrical_conductivity=true;t.variant.soil_water_metrics.electrical_conductivity=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_salinity=true;t.variant.soil_water_metrics.salinity=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_nitrogen=true;t.variant.soil_water_metrics.nitrogen=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_phosphorus=true;t.variant.soil_water_metrics.phosphorus=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_potassium=true;t.variant.soil_water_metrics.potassium=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_dissolved_oxygen=true;t.variant.soil_water_metrics.dissolved_oxygen=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_orp=true;t.variant.soil_water_metrics.orp=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_chemical_oxygen_demand=true;t.variant.soil_water_metrics.chemical_oxygen_demand=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_turbidity=true;t.variant.soil_water_metrics.turbidity=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_nitrate=true;t.variant.soil_water_metrics.nitrate=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_ammonium=true;t.variant.soil_water_metrics.ammonium=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_biochemical_oxygen_demand=true;t.variant.soil_water_metrics.biochemical_oxygen_demand=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
  {meshtastic_Telemetry t{};t.which_variant=11;t.variant.soil_water_metrics.has_solar_irradiance=true;t.variant.soil_water_metrics.solar_irradiance=bad;auto wire=encodeTelemetry(t);meshtastic_Telemetry out{};assert(!mt::RemoteTelemetryQuery::decodeCanonical(wire.data(),wire.size(),11,out));}
 }}
 group("SoilWaterMetrics: every floating field rejects NaN and both infinities");
 {mt::LocalStatsObservation observed{UINT32_MAX,17};uint8_t wire[128]{};size_t size=0;
  assert(mt::LocalStatsQuery::uptimeResponse(UINT32_MAX,77,100.0f,100.0f,wire,sizeof(wire),size,&observed));
  meshtastic_Data d{};auto in=pb_istream_from_buffer(wire,size);assert(pb_decode(&in,meshtastic_Data_fields,&d));
  meshtastic_Telemetry t{};auto payload=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&payload,meshtastic_Telemetry_fields,&t));
  assert(t.variant.local_stats.num_packets_tx==UINT32_MAX&&t.variant.local_stats.num_total_nodes==17&&!t.variant.local_stats.num_packets_rx_bad&&!t.variant.local_stats.heap_free_bytes);
  assert(!mt::LocalStatsQuery::uptimeResponse(1,77,{}, {},wire,1,size,&observed)&&size==0);}
 group("observed TX/node count maxima serialize without inventing unavailable fields; undersized output fails");
 {for(uint8_t tag:{2,6})for(unsigned mode=0;mode<3;++mode){
  meshtastic_Telemetry t{};t.which_variant=tag;
  if(tag==2){if(mode){t.variant.device_metrics.has_channel_utilization=true;t.variant.device_metrics.channel_utilization=mode==1?-1.0f:100.01f;}}
  else {t.variant.local_stats.channel_utilization=mode==0?-1.0f:100.01f;}
  const auto body=encodeData(encodeTelemetry(t));assert(!mt::pki::PrivateApplication::allowed(body.data(),body.size()));
 }}
 group("automatic variant admission preserves strict DeviceMetrics emptiness and DeviceMetrics/LocalStats percentage bounds");

}
