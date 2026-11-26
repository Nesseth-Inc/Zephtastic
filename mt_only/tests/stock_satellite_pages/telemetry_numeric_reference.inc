// SPDX-License-Identifier: GPL-3.0-only
// Verbatim numeric policy from ZT 89f40099331382867d198507db3559d34b31c76e.
  static bool finite(float value){return value>=-FLT_MAX&&value<=FLT_MAX;}
  static bool numeric(const meshtastic_Telemetry &t){switch(t.which_variant){
    case 2: {const auto &v=t.variant.device_metrics;return
      (!v.has_voltage||finite(v.voltage))&&
      (!v.has_channel_utilization||finite(v.channel_utilization))&&
      (!v.has_air_util_tx||finite(v.air_util_tx));}
    case 3: {const auto &v=t.variant.environment_metrics;return
      (!v.has_temperature||finite(v.temperature))&&
      (!v.has_relative_humidity||finite(v.relative_humidity))&&
      (!v.has_barometric_pressure||finite(v.barometric_pressure))&&
      (!v.has_gas_resistance||finite(v.gas_resistance))&&
      (!v.has_voltage||finite(v.voltage))&&
      (!v.has_current||finite(v.current))&&
      (!v.has_distance||finite(v.distance))&&
      (!v.has_lux||finite(v.lux))&&
      (!v.has_white_lux||finite(v.white_lux))&&
      (!v.has_ir_lux||finite(v.ir_lux))&&
      (!v.has_uv_lux||finite(v.uv_lux))&&
      (!v.has_wind_speed||finite(v.wind_speed))&&
      (!v.has_weight||finite(v.weight))&&
      (!v.has_wind_gust||finite(v.wind_gust))&&
      (!v.has_wind_lull||finite(v.wind_lull))&&
      (!v.has_radiation||finite(v.radiation))&&
      (!v.has_rainfall_1h||finite(v.rainfall_1h))&&
      (!v.has_rainfall_24h||finite(v.rainfall_24h))&&
      (!v.has_soil_temperature||finite(v.soil_temperature))&&
      (!v.has_adc_voltage_ch0||finite(v.adc_voltage_ch0))&&
      (!v.has_adc_voltage_ch1||finite(v.adc_voltage_ch1))&&
      (!v.has_adc_voltage_ch2||finite(v.adc_voltage_ch2))&&
      (!v.has_adc_voltage_ch3||finite(v.adc_voltage_ch3))&&
      (!v.has_adc_voltage_ch4||finite(v.adc_voltage_ch4))&&
      (!v.has_adc_voltage_ch5||finite(v.adc_voltage_ch5))&&
      (!v.has_adc_voltage_ch6||finite(v.adc_voltage_ch6))&&
      (!v.has_adc_voltage_ch7||finite(v.adc_voltage_ch7))&&
      (!v.has_one_wire_temperature_ch0||finite(v.one_wire_temperature_ch0))&&
      (!v.has_one_wire_temperature_ch1||finite(v.one_wire_temperature_ch1))&&
      (!v.has_one_wire_temperature_ch2||finite(v.one_wire_temperature_ch2))&&
      (!v.has_one_wire_temperature_ch3||finite(v.one_wire_temperature_ch3))&&
      (!v.has_one_wire_temperature_ch4||finite(v.one_wire_temperature_ch4))&&
      (!v.has_one_wire_temperature_ch5||finite(v.one_wire_temperature_ch5))&&
      (!v.has_one_wire_temperature_ch6||finite(v.one_wire_temperature_ch6))&&
      (!v.has_one_wire_temperature_ch7||finite(v.one_wire_temperature_ch7))&&
      (!v.has_lightning_distance_km||finite(v.lightning_distance_km));}
    case 4: {const auto &v=t.variant.air_quality_metrics;return
      (!v.has_co2_temperature||finite(v.co2_temperature))&&
      (!v.has_co2_humidity||finite(v.co2_humidity))&&
      (!v.has_form_formaldehyde||finite(v.form_formaldehyde))&&
      (!v.has_form_humidity||finite(v.form_humidity))&&
      (!v.has_form_temperature||finite(v.form_temperature))&&
      (!v.has_pm_temperature||finite(v.pm_temperature))&&
      (!v.has_pm_humidity||finite(v.pm_humidity))&&
      (!v.has_pm_voc_idx||finite(v.pm_voc_idx))&&
      (!v.has_pm_nox_idx||finite(v.pm_nox_idx))&&
      (!v.has_particles_tps||finite(v.particles_tps));}
    case 5: {const auto &v=t.variant.power_metrics;return
      (!v.has_ch1_voltage||finite(v.ch1_voltage))&&
      (!v.has_ch1_current||finite(v.ch1_current))&&
      (!v.has_ch2_voltage||finite(v.ch2_voltage))&&
      (!v.has_ch2_current||finite(v.ch2_current))&&
      (!v.has_ch3_voltage||finite(v.ch3_voltage))&&
      (!v.has_ch3_current||finite(v.ch3_current))&&
      (!v.has_ch4_voltage||finite(v.ch4_voltage))&&
      (!v.has_ch4_current||finite(v.ch4_current))&&
      (!v.has_ch5_voltage||finite(v.ch5_voltage))&&
      (!v.has_ch5_current||finite(v.ch5_current))&&
      (!v.has_ch6_voltage||finite(v.ch6_voltage))&&
      (!v.has_ch6_current||finite(v.ch6_current))&&
      (!v.has_ch7_voltage||finite(v.ch7_voltage))&&
      (!v.has_ch7_current||finite(v.ch7_current))&&
      (!v.has_ch8_voltage||finite(v.ch8_voltage))&&
      (!v.has_ch8_current||finite(v.ch8_current));}
    case 6: {const auto &v=t.variant.local_stats;return
      finite(v.channel_utilization)&&
      finite(v.air_util_tx);}
    case 7: {const auto &v=t.variant.health_metrics;return
      (!v.has_temperature||finite(v.temperature));}
    case 8: {return true;}
    case 9: {return true;}
    case 11: {const auto &v=t.variant.soil_water_metrics;return
      (!v.has_soil_ph||finite(v.soil_ph))&&
      (!v.has_ph||finite(v.ph))&&
      (!v.has_electrical_conductivity||finite(v.electrical_conductivity))&&
      (!v.has_salinity||finite(v.salinity))&&
      (!v.has_nitrogen||finite(v.nitrogen))&&
      (!v.has_phosphorus||finite(v.phosphorus))&&
      (!v.has_potassium||finite(v.potassium))&&
      (!v.has_dissolved_oxygen||finite(v.dissolved_oxygen))&&
      (!v.has_orp||finite(v.orp))&&
      (!v.has_chemical_oxygen_demand||finite(v.chemical_oxygen_demand))&&
      (!v.has_turbidity||finite(v.turbidity))&&
      (!v.has_nitrate||finite(v.nitrate))&&
      (!v.has_ammonium||finite(v.ammonium))&&
      (!v.has_biochemical_oxygen_demand||finite(v.biochemical_oxygen_demand))&&
      (!v.has_solar_irradiance||finite(v.solar_irradiance));}
    default:return false;
  }}
