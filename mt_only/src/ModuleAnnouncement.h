// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "TelemetryQuery.h"
#include "RemoteTelemetryQuery.h"
#include "modules/audio/AudioWire.h"
#include "modules/SameChannelBeaconPayloads.h"
#include "environment/EnvironmentMetricsAdapter.h"
#include "power/PowerMetricsAdapter.h"
#include "meshtastic/paxcount.pb.h"
namespace mt {
// Private to the software module composition. No client query, address
// override, signature, ACK, correlation or MQTT consent enters this carrier.
struct ModuleAnnouncement {
  static bool canonical(const uint8_t*data,size_t size,bool&telemetry,bool beacon=false,bool *audio=nullptr,unsigned *port=nullptr,bool *sensorReport=nullptr){
    telemetry=false;if(!data||!size||size>239)return false;
    meshtastic_Data d{};auto input=pb_istream_from_buffer(data,size);
    if(!pb_decode(&input,meshtastic_Data_fields,&d)||input.bytes_left||!d.has_bitfield||d.bitfield||
       d.want_response||d.request_id||d.reply_id||d.emoji||d.source||d.dest||d.xeddsa_signature.size||!d.payload.size||d.payload.size>233)return false;
    if(beacon){
      if(d.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP){if(d.payload.size>100||memchr(d.payload.bytes,0,d.payload.size))return false;}
      else if(d.portnum!=meshtastic_PortNum_MESH_BEACON_APP||!mt::beacon::canonicalPayload(d.payload.bytes,d.payload.size))return false;
    }else if(d.portnum==meshtastic_PortNum_TELEMETRY_APP){
      meshtastic_Telemetry t{};
      if(!RemoteTelemetryQuery::decodeCanonical(d.payload.bytes,d.payload.size,0,t))return false;
      if(sensorReport)*sensorReport=t.which_variant==meshtastic_Telemetry_environment_metrics_tag||
        t.which_variant==meshtastic_Telemetry_air_quality_metrics_tag||t.which_variant==meshtastic_Telemetry_health_metrics_tag||
        t.which_variant==meshtastic_Telemetry_power_metrics_tag;
      if(t.which_variant==meshtastic_Telemetry_device_metrics_tag){
        const auto&m=t.variant.device_metrics;
        if(!m.has_uptime_seconds||(m.has_battery_level&&m.battery_level>101)||
           (m.has_voltage&&!(m.voltage>=0&&m.voltage<=FLT_MAX))||
           (m.has_air_util_tx&&!(m.air_util_tx>=0&&m.air_util_tx<=100))||
           (m.has_channel_utilization&&!(m.channel_utilization>=0&&m.channel_utilization<=100)))return false;
      }else if(t.which_variant==meshtastic_Telemetry_environment_metrics_tag){
        // RemoteTelemetryQuery already verified every populated numeric field
        // and exact canonical protobuf bytes; preserve all weather/sensor data.
      }else if(t.which_variant==meshtastic_Telemetry_air_quality_metrics_tag||
               t.which_variant==meshtastic_Telemetry_health_metrics_tag){
        // Optional local producers use the same finite/canonical checks as
        // admitted peer telemetry, including the newer air/health variants.
      }else if(t.which_variant==meshtastic_Telemetry_power_metrics_tag){
        const auto&m=t.variant.power_metrics;
        if(!zt::telemetry::hasPowerMetrics(m)||!(
          (!m.has_ch1_voltage||(m.ch1_voltage>=0&&m.ch1_voltage<=FLT_MAX))&&
          (!m.has_ch1_current||(m.ch1_current>=-FLT_MAX&&m.ch1_current<=FLT_MAX))&&
          (!m.has_ch2_voltage||(m.ch2_voltage>=0&&m.ch2_voltage<=FLT_MAX))&&
          (!m.has_ch2_current||(m.ch2_current>=-FLT_MAX&&m.ch2_current<=FLT_MAX))&&
          (!m.has_ch3_voltage||(m.ch3_voltage>=0&&m.ch3_voltage<=FLT_MAX))&&
          (!m.has_ch3_current||(m.ch3_current>=-FLT_MAX&&m.ch3_current<=FLT_MAX))&&
          (!m.has_ch4_voltage||(m.ch4_voltage>=0&&m.ch4_voltage<=FLT_MAX))&&
          (!m.has_ch4_current||(m.ch4_current>=-FLT_MAX&&m.ch4_current<=FLT_MAX))&&
          (!m.has_ch5_voltage||(m.ch5_voltage>=0&&m.ch5_voltage<=FLT_MAX))&&
          (!m.has_ch5_current||(m.ch5_current>=-FLT_MAX&&m.ch5_current<=FLT_MAX))&&
          (!m.has_ch6_voltage||(m.ch6_voltage>=0&&m.ch6_voltage<=FLT_MAX))&&
          (!m.has_ch6_current||(m.ch6_current>=-FLT_MAX&&m.ch6_current<=FLT_MAX))&&
          (!m.has_ch7_voltage||(m.ch7_voltage>=0&&m.ch7_voltage<=FLT_MAX))&&
          (!m.has_ch7_current||(m.ch7_current>=-FLT_MAX&&m.ch7_current<=FLT_MAX))&&
          (!m.has_ch8_voltage||(m.ch8_voltage>=0&&m.ch8_voltage<=FLT_MAX))&&
          (!m.has_ch8_current||(m.ch8_current>=-FLT_MAX&&m.ch8_current<=FLT_MAX))))return false;
      }else return false;
      telemetry=true;
    }else if(d.portnum==meshtastic_PortNum_AUDIO_APP){
      if(!mt::audio::AudioWire::validPayload(d.payload.bytes,d.payload.size))return false;
    }else if(d.portnum==meshtastic_PortNum_PAXCOUNTER_APP){
      meshtastic_Paxcount count{};auto payload=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
      if(!pb_decode(&payload,meshtastic_Paxcount_fields,&count)||payload.bytes_left)return false;
      uint8_t bytes[233]{};auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));
      if(!pb_encode(&out,meshtastic_Paxcount_fields,&count)||out.bytes_written!=d.payload.size||memcmp(bytes,d.payload.bytes,d.payload.size))return false;
    }else if(d.portnum!=meshtastic_PortNum_NODE_STATUS_APP&&d.portnum!=meshtastic_PortNum_RANGE_TEST_APP&&d.portnum!=meshtastic_PortNum_NEIGHBORINFO_APP)return false;
#if defined(CONFIG_ZEPHCORE_AUDIO_CODEC2)
    if(audio)*audio=d.portnum==meshtastic_PortNum_AUDIO_APP;
#else
    (void)audio;
#endif
    if(port)*port=d.portnum;
    uint8_t bytes[239]{};auto output=pb_ostream_from_buffer(bytes,sizeof(bytes));
    return pb_encode(&output,meshtastic_Data_fields,&d)&&output.bytes_written==size&&!memcmp(bytes,data,size);
  }
};
}
