// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <meshtastic/telemetry.pb.h>
#include "stock/optional/UnitConversions.h"

namespace mt::ui {

// Only the fields painted by the stock four telemetry frames are retained.
// Receipt time belongs to this device; a sender's UTC is never used for age.
class ReportFrames {
public:
  struct Origin {uint32_t node=0,epoch=0;char name[5]{};bool favorite=false;};
  static constexpr uint64_t FreshMs=300000;
  struct Page {char title[25]{}, source[32]{}, rows[3][28]{};};

private:
  enum Slot { MeshEnv, Power, Air, Health, LocalEnv, FavoriteEnv, Count };
  struct Field {const char *label,*unit;int decimals;};
  struct Report {
    Origin origin{};
    uint64_t at=0;
    uint32_t id=0;
    uint16_t mask=0;
  };
  struct Writer {
    Report &record;
    uint32_t *words;
    void put(unsigned i,bool present,uint32_t n){if(present){record.mask|=uint16_t(1u<<i);words[i]=n;}}
    void number(unsigned i,bool present,float f){
      uint32_t bits;memcpy(&bits,&f,sizeof(bits));put(i,present&&isfinite(f),bits);
    }
  };
  // Field bank capacities in slot order: Env 11, Power 6, Air 5, Health 3,
  // local Env 11 and favorite Env 11. No slot pays for another group's fields.
  static constexpr unsigned offset_[Count+1]={0,11,17,22,25,36,47};
  uint32_t values_[offset_[Count]]{};
  uint32_t owner_=0,domain_=0;
  uint64_t now_=0;
  Report records_[Count]{};
  static constexpr Field envFields[]={{"Temp","",1},{"Humidity","%",0},{"Pressure","hPa",0},{"IAQ","",-1},
    {"Voltage","V",1},{"Current","mA",0},{"Light","lx",0},{"White light","lx",0},{"Weight","kg",0},{"Level","mm",0},{"Radiation","uR/h",2}};
  static constexpr Field airFields[]={{"PM1","ug/m3",-1},{"PM2.5","ug/m3",-1},{"PM10","ug/m3",-1},{"CO2","ppm",-1},{"HCHO","ppb",2}};
  static constexpr Field healthFields[]={{"Temp","",0},{"Heart","bpm",-1},{"SpO2","%",-1}};
  static int kind(pb_size_t tag) {
    switch(tag){
    case meshtastic_Telemetry_environment_metrics_tag:return MeshEnv;
    case meshtastic_Telemetry_power_metrics_tag:return Power;
    case meshtastic_Telemetry_air_quality_metrics_tag:return Air;
    case meshtastic_Telemetry_health_metrics_tag:return Health;
    default:return -1;
    }
  }
  static unsigned fields(int k){return k==Power?6:k==Air?5:k==Health?3:11;}
  static const char *iaq(unsigned n){
    return n<=25?"Excellent":n<=50?"Good":n<=100?"Moderate":n<=150?"Poor":n<=200?"Unhealthy":n<=300?"Very Unhealthy":"Hazardous";
  }
  static void project(Writer r,int k,const meshtastic_Telemetry &t) {
    if(k==Power){
      const auto &m=t.variant.power_metrics;
#define POWER_FIELDS(num, at) r.number(at,m.has_ch##num##_voltage,m.ch##num##_voltage);r.number(at+1,m.has_ch##num##_current,m.ch##num##_current)
      POWER_FIELDS(1,0);POWER_FIELDS(2,2);POWER_FIELDS(3,4);
#undef POWER_FIELDS
    }else if(k==Air){
      const auto &m=t.variant.air_quality_metrics;
      r.put(0,m.has_pm10_standard,m.pm10_standard);r.put(1,m.has_pm25_standard,m.pm25_standard);
      r.put(2,m.has_pm100_standard,m.pm100_standard);r.put(3,m.has_co2,m.co2);
      // Stock requires a PM or CO2 measurement before displaying HCHO.
      r.number(4,r.record.mask && m.has_form_formaldehyde,m.form_formaldehyde);
    }else if(k==Health){
      const auto &m=t.variant.health_metrics;
      r.number(0,m.has_temperature,m.temperature);r.put(1,m.has_heart_bpm,m.heart_bpm);r.put(2,m.has_spO2,m.spO2);
    }else environment(r,t.variant.environment_metrics);
  }
  static void environment(Writer r,const meshtastic_EnvironmentMetrics &m){
      r.number(0,m.has_temperature,m.temperature);r.number(1,m.has_relative_humidity,m.relative_humidity);
      r.number(2,m.has_barometric_pressure&&m.barometric_pressure!=0,m.barometric_pressure);r.put(3,m.has_iaq,m.iaq);
      const bool electric=(m.has_current&&m.current!=0)||(m.has_voltage&&m.voltage!=0);
      r.number(4,electric&&m.has_voltage,m.voltage);r.number(5,electric&&m.has_current,m.current);
      r.number(6,m.has_lux&&m.lux!=0,m.lux);r.number(7,m.has_white_lux&&m.white_lux!=0,m.white_lux);
      r.number(8,m.has_weight&&m.weight!=0,m.weight);r.number(9,m.has_distance&&m.distance!=0,m.distance);
      r.number(10,m.has_radiation&&m.radiation!=0,m.radiation);
  }
  static unsigned long nearest(double value){
    const auto whole=static_cast<unsigned long>(value);
    const double remainder=value-whole;
    return whole+(remainder>0.5 || (remainder==0.5 && (whole&1)));
  }
  static void numeric(char *out,size_t capacity,float input,int digits){
    // Bound both the display string and integer conversions as in the donor.
    const double n=stock::UnitConversions::displaySafeFloat(input);
    const unsigned factor=digits==2?100:digits==1?10:1;
    const double absValue=n<0?-n:n;
    unsigned long whole=static_cast<unsigned long>(absValue);
    unsigned frac=unsigned(nearest((absValue-whole)*factor));
    if(frac==factor){++whole;frac=0;}
    if(digits)snprintf(out,capacity,"%s%lu.%0*u",n<0?"-":"",whole,digits,frac);
    else snprintf(out,capacity,"%s%lu",n<0?"-":"",nearest(absValue));
  }
  static void line(const uint32_t *values,int k,unsigned i,bool fahrenheit,char (&out)[28]){
    char number[22]{},channel[12]{};Field field{};
    if(k==Power){
      snprintf(channel,sizeof(channel),"Ch%u %s",i/2+1,i%2?"current":"volts");
      field={channel,i%2?"mA":"V",i%2?0:2};
    }else field=k==Air?airFields[i]:k==Health?healthFields[i]:envFields[i];
    if(k==MeshEnv&&i==3){snprintf(out,sizeof(out),"IAQ %u %s",unsigned(values[i]),iaq(values[i]));return;}
    if(field.decimals<0)snprintf(number,sizeof(number),"%lu",static_cast<unsigned long>(values[i]));
    else{
      float value;memcpy(&value,&values[i],sizeof(value));
      if((k==MeshEnv||k==Health)&&i==0){if(fahrenheit)value=stock::UnitConversions::CelsiusToFahrenheit(value);field.unit=fahrenheit?"F":"C";}
      numeric(number,sizeof(number),value,field.decimals);
    }
    snprintf(out,sizeof(out),"%s %s%s",field.label,number,field.unit);
  }
  unsigned selected(unsigned slot)const {
    return slot==MeshEnv&&!records_[slot].origin.node?unsigned(LocalEnv):slot;
  }
  static int group(unsigned slot){return slot>=LocalEnv?MeshEnv:int(slot);}
  bool fresh(const Report &r)const{return r.origin.node && now_>=r.at && now_-r.at<=FreshMs;}
  unsigned pages(const Report &r)const {
    unsigned n=fresh(r)?__builtin_popcount(unsigned(r.mask)):0;
    return n?(n+2)/3:1;
  }
public:
  template<class Lookup>void validate(uint32_t self,uint32_t domain,uint64_t now,Lookup lookup){
    if(self!=owner_||domain!=domain_||now<now_){*this={};owner_=self;domain_=domain;}
    now_=now;
    for(unsigned i=0;i<Count;++i){
      auto &r=records_[i];if(!r.origin.node||r.origin.node==self)continue;
      const auto current=lookup(r.origin.node);
      if(!current.epoch||current.epoch!=r.origin.epoch||(i==FavoriteEnv&&!current.favorite))r={};
      else r.origin=current;
    }
  }
  void observe(Origin sender,bool shared,uint32_t id,uint64_t now,const meshtastic_Telemetry &t){
    const int k=kind(t.which_variant);if(k<0||!id||!owner_||!sender.node||now<now_)return;
    const bool local=sender.node==owner_;if(!local&&!sender.epoch)return;
    now_=now;
    if(!shared){
      for(unsigned i=0;i<Count;++i)if(group(i)==k&&records_[i].origin.node==sender.node)records_[i]={};
      return;
    }
    const unsigned slot=local&&k==MeshEnv?int(LocalEnv):k;
    auto &r=records_[slot];
    if(r.id==id&&r.origin.node==sender.node&&r.origin.epoch==sender.epoch)return;
    r={};r.origin=sender;r.id=id;r.at=now;project({r,values_+offset_[slot]},k,t);
    if(k==MeshEnv&&!local&&sender.favorite){
      records_[FavoriteEnv]=r;
      memcpy(values_+offset_[FavoriteEnv],values_+offset_[slot],11*sizeof(uint32_t));
    }
  }
  // LocalSensor in stock refreshes directly from acquisition, independently of
  // a phone or RF send. The Service snapshot has already checked freshness.
  void localEnvironment(const meshtastic_EnvironmentMetrics &m){
    if(!owner_)return;
    auto &r=records_[LocalEnv];
    r.mask=0;environment({r,values_+offset_[LocalEnv]},m);
    if(!r.mask){r={};return;}
    r.origin.node=owner_;r.id=0;r.at=now_;
  }
  unsigned count(unsigned enabled)const {
    unsigned n=0;for(unsigned i=0;i<Count;++i)if(enabled&(1u<<group(i)))n+=pages(records_[selected(i)]);return n;
  }
  Page page(unsigned index,unsigned enabled,bool fahrenheit)const {
    Page out{};
    static constexpr const char *titles[]={"Mesh environment","Power","Air quality","Health","Local environment","Favorite env"};
    const auto total=count(enabled);if(!total)return out;
    index%=total;
    for(unsigned s=0;s<Count;++s){
      const int k=group(s);if(!(enabled&(1u<<k)))continue;
      const unsigned slot=selected(s);const auto &r=records_[slot];const unsigned n=pages(r);
      if(index>=n){index-=n;continue;}
      snprintf(out.title,sizeof(out.title),"%s %u/%u",titles[s],index+1,n);
      if(!r.origin.node){strcpy(out.source,"No measurement");return out;}
      char who[12]{},age[12]{};
      if(r.origin.node==owner_)strcpy(who,"Local");else if(r.origin.name[0])snprintf(who,sizeof(who),"%.4s",r.origin.name);
      else snprintf(who,sizeof(who),"!%08lx",static_cast<unsigned long>(r.origin.node));
      const uint64_t seconds=(now_-r.at)/1000;
      if(seconds>864000)strcpy(age,"?");else if(seconds>3600)snprintf(age,sizeof(age),"%uh",unsigned(seconds/3600));
      else if(seconds>60)snprintf(age,sizeof(age),"%um",unsigned(seconds/60));else snprintf(age,sizeof(age),"%us",unsigned(seconds));
      if(r.origin.node==owner_&&!r.id)strcpy(out.source,"Local sensor");
      else snprintf(out.source,sizeof(out.source),"%s %s%s",who,age,fresh(r)?"":" STALE");
      unsigned seen=0,rows=0;
      if(fresh(r))for(unsigned i=0;i<fields(k)&&rows<3;++i)if(r.mask&(1u<<i)){
        if(seen++>=index*3)line(values_+offset_[slot],k,i,fahrenheit,out.rows[rows++]);
      }
      if(!rows)strcpy(out.rows[0],fresh(r)?"No measurement":"Waiting for update");
      return out;
    }
    return out;
  }
};
}
