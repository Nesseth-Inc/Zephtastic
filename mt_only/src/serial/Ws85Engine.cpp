// SPDX-License-Identifier: GPL-3.0-only
#include "Ws85Engine.h"
#include "../stock/peripherals/SerialWeather.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace mt_serial {
namespace {
void trim(char *&begin, char *&end) {
  while(begin<end && (*begin==' ' || *begin=='\t')) ++begin;
  while(end>begin && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r')) --end;
}
bool parseNumber(char *begin, char *end, float &value) {
  trim(begin,end);
  if(begin==end) return false;
  const char saved=*end;
  *end='\0';
  char *after=nullptr;
  const float parsed=strtof(begin,&after);
  const bool valid=after==end && isfinite(parsed);
  *end=saved;
  if(!valid) return false;
  value=parsed;
  return true;
}
}

bool Ws85Engine::feed(const uint8_t *bytes,size_t size) {
  if(size&&!bytes) return false;
  for(size_t i=0;i<size;++i) {
    const char c=static_cast<char>(bytes[i]);
    if(c=='\n') {
      if(discardLine_) {
        if(malformedLines_!=UINT32_MAX) ++malformedLines_;
      } else if(lineUsed_) {
        line_[lineUsed_]='\0';
        lineComplete();
      }
      lineUsed_=0;
      discardLine_=false;
      continue;
    }
    if(discardLine_) continue;
    if(c=='\r') continue;
    if(static_cast<unsigned char>(c)<0x20 || static_cast<unsigned char>(c)>0x7e) {
      discardLine_=true;
      continue;
    }
    if(lineUsed_+1>=sizeof(line_)) {
      discardLine_=true;
      continue;
    }
    line_[lineUsed_++]=c;
  }
  return true;
}

void Ws85Engine::lineComplete() {
  char *begin=line_, *end=line_+lineUsed_;
  trim(begin,end);
  if(begin==end) return;
  char *equals=begin;
  while(equals<end && *equals!='=') ++equals;
  if(equals==end) { if(malformedLines_!=UINT32_MAX)++malformedLines_; return; }
  char *nameEnd=equals;
  trim(begin,nameEnd);
  char *valueBegin=equals+1, *valueEnd=end;
  trim(valueBegin,valueEnd);
  if(begin==nameEnd || valueBegin==valueEnd) {
    if(malformedLines_!=UINT32_MAX)++malformedLines_;
    return;
  }
  float value=0;
  if(!parseNumber(valueBegin,valueEnd,value)) {
    if(malformedLines_!=UINT32_MAX)++malformedLines_;
    return;
  }
  const size_t nameLength=static_cast<size_t>(nameEnd-begin);
  auto nameIs=[&](const char *expected) {
    return strlen(expected)==nameLength && memcmp(begin,expected,nameLength)==0;
  };
  if(nameIs("WindDir")) {
    if(value<0 || value>=360) return;
    mt::stock::serial::addDirection(value,directionSin_,directionCos_);
    if(directionCount_!=UINT32_MAX)++directionCount_;
    gotWind_=true;
  } else if(nameIs("WindSpeed")) {
    if(value<0) return;
    velocitySum_+=value;
    if(velocityCount_!=UINT32_MAX)++velocityCount_;
    if(lull_<0 || value<lull_) lull_=value;
    gotWind_=true;
  } else if(nameIs("WindGust")) {
    if(value<0) return;
    if(value>gust_) gust_=value;
    gustSeen_=true;
    gotWind_=true;
  } else if(nameIs("BatVoltage")) {
    if(value>=0) { batteryVoltage_=value; haveBatteryVoltage_=true; }
  } else if(nameIs("CapVoltage")) {
    if(value>=0) { capacitorVoltage_=value; haveCapacitorVoltage_=true; }
  } else if(nameIs("GXTS04Temp") || nameIs("Temperature")) {
    temperature_=value; haveTemperature_=true;
  } else if(nameIs("RainIntSum")) {
    if(value>=0) { rainSum_=truncf(value); haveRainSum_=true; }
  } else if(nameIs("Rain")) {
    if(value>=0) { rain_=value; haveRain_=true; }
  }
}

bool Ws85Engine::poll(uint32_t nowMs,WeatherEmit emit,void *context) {
  if(!gotWind_ || !velocityCount_ || !directionCount_ || !emit ||
     static_cast<uint32_t>(nowMs-lastAveragedMs_)<ReportIntervalMs) return false;
  WeatherReport report{};
  report.has_wind_speed=true;
  report.has_wind_direction=true;
  report.has_wind_gust=gustSeen_;
  report.has_wind_lull=true;
  report.has_temperature=haveTemperature_;
  report.has_voltage=haveBatteryVoltage_||haveCapacitorVoltage_;
  report.has_rainfall_24h=haveRainSum_;
  report.has_rainfall_1h=haveRain_;
  mt::stock::serial::averageWind(report,velocitySum_,velocityCount_,directionSin_,directionCos_,directionCount_);
  report.temperature=temperature_;
  report.voltage=capacitorVoltage_>batteryVoltage_?capacitorVoltage_:batteryVoltage_;
  report.wind_gust=gust_;
  report.rainfall_24h=rainSum_;
  report.rainfall_1h=rain_;
  report.wind_lull=lull_<0?0:lull_;
  if(!emit(context,report)) return false;
  lastAveragedMs_=nowMs;
  resetWindow();
  return true;
}

void Ws85Engine::resetWindow() {
  directionSin_=directionCos_=0;
  velocitySum_=gust_=0;
  lull_=-1;
  velocityCount_=directionCount_=0;
  gotWind_=gustSeen_=false;
}
} // namespace mt_serial
