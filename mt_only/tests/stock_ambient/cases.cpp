// SPDX-License-Identifier: GPL-3.0-only
#include "stock/telemetry/ZephyrEnvironmentChannels.h"
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <initializer_list>
using std::isnan;
namespace donor {
uint16_t raw=0;
enum { PCT2075_REGISTER_TEMP=0, MCP9808_REG_AMBIENT_TEMP=5, MSBFIRST=1 };
struct Adafruit_BusIO_Register {
  Adafruit_BusIO_Register(void *,int reg,int size,int order){assert(!reg&&size==2&&order==MSBFIRST);}
  uint16_t read(){return raw;}
};
struct Adafruit_PCT2075{void *i2c_dev=nullptr;float getTemperature();};
struct Adafruit_MCP9808{float readTempC();uint16_t read16(int r){assert(r==5);return raw;}};
struct PCT2075Sensor{Adafruit_PCT2075 pct2075;bool getMetrics(meshtastic_Telemetry *);};
struct MCP9808Sensor{Adafruit_MCP9808 mcp9808;bool getMetrics(meshtastic_Telemetry *);};
struct jc42_data {uint16_t reg_val;};
struct device {jc42_data *data;};
#define JC42_TEMP_SCALE_CEL 16
#define JC42_TEMP_ABS_MASK 4095
#define JC42_TEMP_SIGN_BIT 4096
#define __ASSERT_NO_MSG(x) assert(x)
#define LOG_DEBUG(...)
#include "donors.inc"
}
static constexpr device pressurePart{97};
static int failure=0;static unsigned reads=0;
bool device_is_ready(const device*){return true;}
int sensor_sample_fetch(const device*){return failure;}
int sensor_channel_get(const device *dev,sensor_channel ch,sensor_value *out){
 if(dev==&pressurePart){
   if(ch==SENSOR_CHAN_AMBIENT_TEMP){*out={-5,0};return 0;}
   if(ch==SENSOR_CHAN_PRESS){*out={99,500000};return 0;}
   return -ENOTSUP;
 }
 assert(ch==SENSOR_CHAN_AMBIENT_TEMP);++reads;
 if(dev==&zt_environment){donor::lm75_temp_to_sensor_value(static_cast<int16_t>(donor::raw),out);return 0;}
 donor::jc42_data data{donor::raw};donor::device measured{&data};return donor::jc42_channel_get(&measured,ch,out);
}
int main(){
  donor::PCT2075Sensor pct;donor::MCP9808Sensor mcp;
  for(unsigned i=0;i<2048;++i){
    donor::raw=i<<5;meshtastic_Telemetry t{};assert(pct.getMetrics(&t));
    zt::telemetry::EnvironmentReadings actual;
    mt::stock::telemetry::readEnvironmentChannels<true>(&zt_environment,actual);
    const auto m=zt::telemetry::toMeshtasticEnvironmentMetrics(actual);
    assert(m.has_temperature&&m.temperature==t.variant.environment_metrics.temperature);
  }
  for(unsigned i=0;i<=65535;i+=2){
    donor::raw=i;meshtastic_Telemetry t{};assert(mcp.getMetrics(&t));
    zt::telemetry::EnvironmentReadings actual;
    mt::stock::telemetry::readEnvironmentChannels<true>(&thermometer6,actual);
    const auto m=zt::telemetry::toMeshtasticEnvironmentMetrics(actual);
    assert(m.has_temperature&&m.temperature==t.variant.environment_metrics.temperature);
  }
  using Source=zt::telemetry::EnvironmentSource;
  for(bool reversed : {false,true}) {
    zt::telemetry::EnvironmentReadings actual;
    actual.humidityPercent.record(Source::Lark,50);
    auto baro=[&]{mt::stock::telemetry::readEnvironmentChannels<false,Source::Bmp3xx>(&pressurePart,actual);};
    auto later=[&]{mt::stock::telemetry::readEnvironmentChannels<true,Source::Pct2075>(&zt_environment,actual);};
    donor::raw=uint16_t(20*256);
    if(reversed){later();baro();}else{baro();later();}
    auto m=zt::telemetry::toMeshtasticEnvironmentMetrics(actual);
    assert(m.has_temperature&&m.temperature==20&&!m.has_relative_humidity&&m.barometric_pressure==995);
    actual.ambientCelsius.record(Source::OneWire,23);
    baro();later();
    assert(*actual.ambientCelsius==23);
  }
  failure=-EIO;zt::telemetry::EnvironmentReadings absent;
  mt::stock::telemetry::readEnvironmentChannels<true>(&thermometer6,absent);
  assert(!absent.ambientCelsius&&reads==2048+32768+4);
  puts("PASS all 2048 PCT2075 codes, 32768 MCP9808 resolution-2 readings and alert flags through real Zephyr/donor conversions, failed read and temperature-only query");
}
