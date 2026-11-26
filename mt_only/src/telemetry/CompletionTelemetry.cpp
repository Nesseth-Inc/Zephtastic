// SPDX-License-Identifier: GPL-3.0-only
#include "CompletionTelemetry.h"
#include "../ModuleSettings.h"
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
#include "stock/telemetry/accessory/Accessories.h"
#endif
#include "../stock/telemetry/SensorModules.h"
#include "RemoteTelemetryQuery.h"
#include <errno.h>
#include <float.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/devicetree.h>
#include "../stock/telemetry/accessory/ParticleAliases.h"
#if defined(CONFIG_MT_MAX30102_OPTICAL)
#include "../stock/telemetry/max30102/OpticalWindow.h"
#endif
#if GENERIC_AIR(air_quality) || GENERIC_AIR(air_quality_2) || GENERIC_AIR(air_quality_3)
#include "../stock/telemetry/ZephyrAirChannels.h"
#endif

namespace mt::telemetry {
namespace {
constexpr uint64_t MaxAgeMs=90000;
bool finite(float f){return f>=-FLT_MAX&&f<=FLT_MAX;}
bool valid(const meshtastic_ModuleConfig_TelemetryConfig &c){
  return ModuleSettings::stockInterval(c.air_quality_interval)&&ModuleSettings::stockInterval(c.health_update_interval);
}
bool readChannel(const struct device *dev,enum sensor_channel channel,struct sensor_value &value){
  if(!dev||!device_is_ready(dev)||sensor_sample_fetch(dev)<0||sensor_channel_get(dev,channel,&value)<0)return false;
  return true;
}
float asFloat(const struct sensor_value &v){return sensor_value_to_double(&v);}
bool asUnsigned(const struct sensor_value &v,uint32_t &out){
  const double d=sensor_value_to_double(&v);
  if(!(d>=0.0)||d>255)return false;
  out=static_cast<uint32_t>(d+0.5);return true;
}
#if DT_NODE_HAS_STATUS(DT_ALIAS(health_sensor),okay)
// Zephyr's common sensor API has no heart-rate/SpO2 channels. A board may
// bind driver-documented SENSOR_CHAN_PRIV_START offsets on its health_sensor
// node; absent or out-of-range offsets mean that health data is unavailable.
constexpr int HealthHeartRateOffset=DT_PROP_OR(DT_ALIAS(health_sensor),heart_rate_channel_offset,-1);
constexpr int HealthSpO2Offset=DT_PROP_OR(DT_ALIAS(health_sensor),spo2_channel_offset,-1);
constexpr int HealthTemperatureOffset=DT_PROP_OR(DT_ALIAS(health_sensor),body_temperature_channel_offset,-1);
#else
constexpr int HealthHeartRateOffset=-1;
constexpr int HealthSpO2Offset=-1;
constexpr int HealthTemperatureOffset=-1;
#endif
constexpr int HealthHeartRateChannel=SENSOR_CHAN_PRIV_START+HealthHeartRateOffset;
constexpr int HealthSpO2Channel=SENSOR_CHAN_PRIV_START+HealthSpO2Offset;
constexpr bool healthChannelDeclared(int offset){return offset>=0&&SENSOR_CHAN_PRIV_START+offset<SENSOR_CHAN_MAX;}
constexpr bool healthChannelsDeclared(){return healthChannelDeclared(HealthHeartRateOffset)||healthChannelDeclared(HealthSpO2Offset)||healthChannelDeclared(HealthTemperatureOffset);}

using AirSensor=bool (*)(meshtastic_AirQualityMetrics &);
constexpr AirSensor airSensors[]={
#if GENERIC_AIR(air_quality)
  [](meshtastic_AirQualityMetrics &m){return stock::telemetry::readAirChannels(DEVICE_DT_GET(DT_ALIAS(air_quality)),m);},
#endif
#if GENERIC_AIR(air_quality_2)
  [](meshtastic_AirQualityMetrics &m){return stock::telemetry::readAirChannels(DEVICE_DT_GET(DT_ALIAS(air_quality_2)),m);},
#endif
#if GENERIC_AIR(air_quality_3)
  [](meshtastic_AirQualityMetrics &m){return stock::telemetry::readAirChannels(DEVICE_DT_GET(DT_ALIAS(air_quality_3)),m);},
#endif
  nullptr
};
bool readOptical(meshtastic_HealthMetrics &m){
  bool any=false;
#if DT_NODE_HAS_STATUS(DT_ALIAS(health_sensor),okay)
  const struct device *dev=DEVICE_DT_GET(DT_ALIAS(health_sensor));
  struct sensor_value v{};
  uint32_t x=0;
  if(healthChannelDeclared(HealthHeartRateOffset)&&readChannel(dev,static_cast<enum sensor_channel>(HealthHeartRateChannel),v)&&asUnsigned(v,x)&&x>0&&x<=255){
    m.has_heart_bpm=true;m.heart_bpm=static_cast<uint8_t>(x);any=true;
  }
  if(healthChannelDeclared(HealthSpO2Offset)&&readChannel(dev,static_cast<enum sensor_channel>(HealthSpO2Channel),v)&&asUnsigned(v,x)&&x>0&&x<=100){
    m.has_spO2=true;m.spO2=static_cast<uint8_t>(x);any=true;
  }
#endif
  return any;
}
bool readBodyTemperature(meshtastic_HealthMetrics &m){
  bool any=false;
#if DT_NODE_HAS_STATUS(DT_ALIAS(body_temperature),okay)
  const struct device *thermometer=DEVICE_DT_GET(DT_ALIAS(body_temperature));
  struct sensor_value contact{};
  if(device_is_ready(thermometer)&&!sensor_sample_fetch(thermometer)&&
     !sensor_channel_get(thermometer,SENSOR_CHAN_AMBIENT_TEMP,&contact)){
    const float value=asFloat(contact);
    if(finite(value)){m.has_temperature=true;m.temperature=value;return true;}
  }
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(health_sensor),okay)
  const struct device *dev=DEVICE_DT_GET(DT_ALIAS(health_sensor));
  struct sensor_value v{};
  if(healthChannelDeclared(HealthTemperatureOffset)&&readChannel(dev,static_cast<enum sensor_channel>(SENSOR_CHAN_PRIV_START+HealthTemperatureOffset),v)){
    const float value=asFloat(v);if(finite(value)){m.has_temperature=true;m.temperature=value;any=true;}
  }
#endif
  return any;
}
}
bool CompletionTelemetry::configure(const meshtastic_ModuleConfig_TelemetryConfig &config){
  if(!valid(config))return false;
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
  stock::telemetry::accessory::enableAir(config.air_quality_enabled,config.air_quality_interval);
#endif
#if defined(CONFIG_MT_MLX90614)
  stock::telemetry::accessory::enableMlx(config.health_measurement_enabled);
#endif
  config_=config;configured_=true;latest_={};return true;
}
CompletionTelemetry::Capabilities CompletionTelemetry::capabilities()const{
  Capabilities out{};
#if GENERIC_AIR(air_quality)
  out.hasAir=device_is_ready(DEVICE_DT_GET(DT_ALIAS(air_quality)));
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(health_sensor),okay)
  out.hasHealth=healthChannelsDeclared()&&device_is_ready(DEVICE_DT_GET(DT_ALIAS(health_sensor)));
#endif
#if GENERIC_AIR(air_quality_2)
  out.hasAir|=device_is_ready(DEVICE_DT_GET(DT_ALIAS(air_quality_2)));
#endif
#if GENERIC_AIR(air_quality_3)
  out.hasAir|=device_is_ready(DEVICE_DT_GET(DT_ALIAS(air_quality_3)));
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(body_temperature),okay)
  out.hasHealth|=device_is_ready(DEVICE_DT_GET(DT_ALIAS(body_temperature)));
#endif
#if defined(CONFIG_MT_MAX30102_OPTICAL)
  out.hasHealth|=stock::telemetry::max30102::available();
#endif
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
  out.hasAir|=stock::telemetry::accessory::airAvailable();
#endif
#if defined(CONFIG_MT_MLX90614)
  out.hasHealth |= stock::telemetry::accessory::mlxAvailable();
#endif
  return out;
}
bool CompletionTelemetry::configSupported(const meshtastic_ModuleConfig_TelemetryConfig &config)const{
  if(!valid(config))return false;
  const auto caps=capabilities();
  return (!config.air_quality_enabled||caps.hasAir)&&(!config.health_measurement_enabled||caps.hasHealth);
}
CompletionTelemetry::Result CompletionTelemetry::poll(uint64_t now){
  if(!configured_)return Result::InvalidConfig;
  if(now>INT64_MAX)return Result::InvalidConfig;
  Snapshot next{};next.sampledAtMs=now;
  if (config_.air_quality_enabled) {
    next.hasAir = stock::AirQualityTelemetryModule::getAirQualityTelemetry(next.air, airSensors);
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
    next.hasAir|=stock::telemetry::accessory::air(now,next.air);
#endif
  }
  if (config_.health_measurement_enabled){
    next.hasHealth = stock::HealthTelemetryModule::getHealthTelemetry(next.health,
        healthChannelDeclared(HealthHeartRateOffset) || healthChannelDeclared(HealthSpO2Offset),
        readOptical, DT_NODE_HAS_STATUS(DT_ALIAS(body_temperature),okay) || healthChannelDeclared(HealthTemperatureOffset), readBodyTemperature);
#if defined(CONFIG_MT_MAX30102_OPTICAL)
    next.hasHealth|=stock::telemetry::max30102::sample(now,next.health);
#endif
#if defined(CONFIG_MT_MLX90614)
    next.hasHealth |= stock::telemetry::accessory::objectTemperature(now,next.health);
#endif
  }
  latest_=next;
  return next.hasAir||next.hasHealth?Result::Ok:Result::Unavailable;
}
CompletionTelemetry::Result CompletionTelemetry::snapshot(uint64_t now,Snapshot &out)const{
  out={};if(!configured_)return Result::InvalidConfig;
  if((!latest_.hasAir&&!latest_.hasHealth)||now<latest_.sampledAtMs)return Result::Unavailable;
  if(now-latest_.sampledAtMs>MaxAgeMs)return Result::Stale;
  out=latest_;return Result::Ok;
}
bool CompletionTelemetry::observePeer(uint32_t node,const uint8_t *payload,size_t size,uint64_t now){
  if(!node||now>INT64_MAX)return false;
  meshtastic_Telemetry decoded{};
  if(!RemoteTelemetryQuery::decodeCanonical(payload,size,0,decoded)||
     (decoded.which_variant!=meshtastic_Telemetry_air_quality_metrics_tag&&
      decoded.which_variant!=meshtastic_Telemetry_health_metrics_tag))return false;
  PeerSnapshot *slot=nullptr;
  for(auto &peer:peers_)if(peer.nodeId==node){slot=&peer;break;}
  if(!slot){slot=&peers_[nextPeer_++%8];*slot={};slot->nodeId=node;}
  if(decoded.which_variant==meshtastic_Telemetry_air_quality_metrics_tag){
    slot->air=decoded.variant.air_quality_metrics;slot->airAtMs=now;slot->airUtc=decoded.time;slot->hasAir=true;
  }else{
    slot->health=decoded.variant.health_metrics;slot->healthAtMs=now;slot->healthUtc=decoded.time;slot->hasHealth=true;
  }
  return true;
}
CompletionTelemetry::Result CompletionTelemetry::peerSnapshot(uint32_t node,uint8_t variant,uint64_t now,
    meshtastic_Telemetry &out)const{
  out={};if(!node||now>INT64_MAX)return Result::InvalidRequest;
  for(const auto &peer:peers_)if(peer.nodeId==node){
    if(variant==meshtastic_Telemetry_air_quality_metrics_tag&&peer.hasAir){
      if(now<peer.airAtMs||now-peer.airAtMs>MaxAgeMs)return Result::Stale;
      out.time=peer.airUtc;out.which_variant=variant;out.variant.air_quality_metrics=peer.air;return Result::Ok;
    }
    if(variant==meshtastic_Telemetry_health_metrics_tag&&peer.hasHealth){
      if(now<peer.healthAtMs||now-peer.healthAtMs>MaxAgeMs)return Result::Stale;
      out.time=peer.healthUtc;out.which_variant=variant;out.variant.health_metrics=peer.health;return Result::Ok;
    }
    return Result::Unavailable;
  }
  return Result::Unavailable;
}
CompletionTelemetry::Result CompletionTelemetry::response(uint8_t variant,uint32_t request,uint64_t now,uint32_t utc,
    uint8_t *out,size_t capacity,size_t &size)const{
  size=0;if(!out||!request)return Result::InvalidRequest;
  Snapshot s{};const auto state=snapshot(now,s);if(state!=Result::Ok)return state;
  meshtastic_Telemetry t{};
  if (!stock::telemetry::allocReply(variant, s, utc, t)) return Result::Unavailable;
  meshtastic_Data d{};d.portnum=meshtastic_PortNum_TELEMETRY_APP;d.request_id=request;d.has_bitfield=true;
  auto p=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));
  if(!pb_encode(&p,meshtastic_Telemetry_fields,&t))return Result::Unavailable;
  d.payload.size=p.bytes_written;auto stream=pb_ostream_from_buffer(out,capacity);
  if(!pb_encode(&stream,meshtastic_Data_fields,&d))return Result::Unavailable;
  size=stream.bytes_written;return Result::Ok;
}
}
