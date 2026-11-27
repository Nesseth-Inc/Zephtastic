// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Model.h"
#include "stock/optional/UnitConversions.h"
#include <math.h>
#include <stdarg.h>

namespace mt::ui {
struct TelemetryView {
private:
  static void append(char *out,size_t capacity,size_t &used,const char *format,...){
    if(used>=capacity)return;
    va_list args;va_start(args,format);const int n=vsnprintf(out+used,capacity-used,format,args);va_end(args);
    if(n>0){const size_t remaining=capacity-used;used+=(size_t(n)<remaining?size_t(n):remaining-1);}
  }
public:
  // The T096 release links picolibc's integer-only printf. Bound the value
  // before conversion and use the same whole-degree rounding as %.0f.
  static bool displayInteger(float value, int &rounded) {
    if (!(value >= -1e9f && value <= 1e9f)) return false;
    rounded = int(lrintf(value));
    return true;
  }

  static bool healthTemperature(float celsius, bool fahrenheit, int &degrees) {
    return displayInteger(fahrenheit ? stock::UnitConversions::CelsiusToFahrenheit(celsius) : celsius, degrees);
  }

  static unsigned lines(const TelemetrySnapshot &s,uint64_t nowMs,bool fahrenheit,
                        char out[4][36]) {
    unsigned count=0;const bool fresh=s.fresh(nowMs);
    if(s.showEnvironment){
      char value[28]{};
      if(!fresh||(!s.hasTemperature&&!s.hasHumidity&&!s.hasPressure))snprintf(value,sizeof(value),"UNAVAILABLE");
      else {
        size_t used=0;
        if(s.hasTemperature){
          const int32_t t10=fahrenheit?(int32_t(s.temperatureTenthsC)*9)/5+320:s.temperatureTenthsC;
          const uint32_t magnitude=t10<0?uint32_t(-t10):uint32_t(t10);
          append(value,sizeof(value),used,"%s%u.%u%c",t10<0?"-":"",unsigned(magnitude/10),unsigned(magnitude%10),fahrenheit?'F':'C');
        }
        if(s.hasHumidity)append(value,sizeof(value),used," %u.%u%%",unsigned(s.humidityTenths/10),unsigned(s.humidityTenths%10));
        if(s.hasPressure)append(value,sizeof(value),used," %luhPa",static_cast<unsigned long>(s.pressureHpa));
      }
      snprintf(out[count++],36,"ENV %s",value);
    }
    if(s.showPower){
      char value[28]{};size_t used=0;
      if(fresh)for(unsigned i=0;i<3;++i)if(s.hasPowerVoltage[i]||s.hasPowerCurrent[i]){
        if(used)append(value,sizeof(value),used," ");
        append(value,sizeof(value),used,"%u:",i+1);
        if(s.hasPowerVoltage[i])append(value,sizeof(value),used,"%u.%02uV",unsigned(s.powerMilliVolts[i]/1000),unsigned((s.powerMilliVolts[i]%1000)/10));
        if(s.hasPowerCurrent[i]){const int32_t current=s.powerMilliAmps[i];const uint32_t magnitude=current<0?uint32_t(-current):uint32_t(current);
          append(value,sizeof(value),used,"%s%s%u.%02uA",s.hasPowerVoltage[i]?"/":"",current<0?"-":"",unsigned(magnitude/1000),unsigned(magnitude%1000/10));}
      }
      if(!value[0])snprintf(value,sizeof(value),"UNAVAILABLE");
      snprintf(out[count++],36,"PWR %s",value);
    }
    if(s.showAirQuality){
      if(!fresh||(!s.hasPm25&&!s.hasPm10&&!s.hasCo2))snprintf(out[count++],36,"AIR UNAVAILABLE");
      else {
        char value[28]{};size_t used=0;
        if(s.hasPm25)append(value,sizeof(value),used,"2.5:%u",unsigned(s.pm25UgM3));
        if(s.hasPm10)append(value,sizeof(value),used," 10:%u",unsigned(s.pm10UgM3));
        if(s.hasCo2)append(value,sizeof(value),used," C:%u",unsigned(s.co2Ppm));
        snprintf(out[count++],36,"AIR %s",value);
      }
    }
    if(s.showHealth){
      if(!fresh||(!s.hasHeartRate&&!s.hasSpO2&&!s.hasBodyTemperature))snprintf(out[count++],36,"HEALTH UNAVAILABLE");
      else {
        char value[28]{};size_t used=0;
        if(s.hasHeartRate)append(value,sizeof(value),used,"HR:%u",unsigned(s.heartBpm));
        if(s.hasSpO2)append(value,sizeof(value),used," O2:%u%%",unsigned(s.spO2Percent));
        int degrees;
        if (s.hasBodyTemperature && healthTemperature(s.bodyTemperatureC, fahrenheit, degrees))
          append(value,sizeof(value),used," T:%d%c",degrees,fahrenheit?'F':'C');
        snprintf(out[count++],36,"HLTH %s",value);
      }
    }
    return count;
  }
};
}
