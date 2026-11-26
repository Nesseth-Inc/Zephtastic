// SPDX-License-Identifier: GPL-3.0-only
#include "ui/ReportFrames.h"
#include <cassert>
#include <initializer_list>
#include <cstring>
#include <cstdio>
using Frames=mt::ui::ReportFrames;
static constexpr Frames::Origin self{7,0,"ME"}, a{9,3,"A",true}, b{11,5,"B"};
static meshtastic_Telemetry env(float temp){
  meshtastic_Telemetry t{};t.which_variant=meshtastic_Telemetry_environment_metrics_tag;
  auto &e=t.variant.environment_metrics;e.has_temperature=true;e.temperature=temp;return t;
}
static bool has(const Frames::Page &p,const char *needle){
  if(strstr(p.title,needle)||strstr(p.source,needle))return true;
  for(const auto &r:p.rows)if(strstr(r,needle))return true;
  return false;
}
static void find(const Frames &f,unsigned mask,const char *needle,bool fahrenheit=false){
  for(unsigned i=0;i<f.count(mask);++i)if(has(f.page(i,mask,fahrenheit),needle))return;
  fprintf(stderr,"Missing %s\n",needle);assert(false);
}
int main(){
  Frames f;unsigned ea=3,eb=5;bool fav=true;
  auto lookup=[&](uint32_t id){
    if(id==a.node){auto v=a;v.epoch=ea;v.favorite=fav;return v;}
    if(id==b.node){auto v=b;v.epoch=eb;return v;}
    return Frames::Origin{};
  };
  f.validate(7,21,1000,lookup);
  assert(f.count(15)==6 && f.count(0)==0 && has(f.page(0,1,false),"No measurement"));
  f.localEnvironment(env(-1.5f).variant.environment_metrics);
  assert(has(f.page(0,1,true),"Local sensor")&&has(f.page(0,1,true),"29.3F"));
  f.localEnvironment({});assert(has(f.page(0,1,false),"No measurement"));
  f.observe(self,true,1,1000,env(18));
  assert(has(f.page(0,1,false),"Local 0s")&&has(f.page(0,1,false),"18.0C"));
  assert(has(f.page(2,1,false),"No measurement")); // Favorite-only has no local fallback.
  auto e=env(12.5f);auto &m=e.variant.environment_metrics;
  m.has_relative_humidity=true;m.relative_humidity=60;
  m.has_barometric_pressure=true;m.barometric_pressure=991;
  m.has_iaq=true;m.iaq=201;m.has_voltage=true;m.voltage=3.5f;m.has_current=true;m.current=-12.5f;
  m.has_lux=m.has_white_lux=m.has_weight=m.has_distance=m.has_radiation=true;
  m.lux=45;m.white_lux=36;m.weight=11;m.distance=250;m.radiation=1.25f;
  f.observe(a,true,2,2500,e);f.observe(self,true,3,2800,env(22));f.validate(7,21,3500,lookup);
  f.localEnvironment(env(22).variant.environment_metrics);
  assert(has(f.page(0,1,false),"A 1s")&&has(f.page(0,1,false),"12.5C"));
  for(const char *s:{"Humidity 60%","Pressure 991hPa","IAQ 201 Very Unhealthy","Voltage 3.5V","Current -12mA","Light 45lx","White light 36lx","Weight 11kg","Level 250mm","Radiation 1.25uR/h"})find(f,1,s);
  auto radiation=env(0);radiation.variant.environment_metrics.has_radiation=true;
  radiation.variant.environment_metrics.radiation=0.045f;
  f.localEnvironment(radiation.variant.environment_metrics);
  find(f,1,"Radiation 0.05uR/h");
  f.observe(b,true,4,4000,env(-40));
  assert(has(f.page(0,1,true),"B 0s")&&has(f.page(0,1,true),"-40.0F"));find(f,1,"A 1s");
  f.observe(b,true,4,4100,env(99));assert(has(f.page(0,1,false),"-40.0C"));
  f.observe(b,false,5,4200,env(99));assert(has(f.page(0,1,false),"Local"));
  ea++;f.validate(7,21,6000,lookup);assert(has(f.page(2,1,false),"No measurement"));
  auto air=meshtastic_Telemetry{};air.which_variant=meshtastic_Telemetry_air_quality_metrics_tag;auto &aq=air.variant.air_quality_metrics;
  aq.has_form_formaldehyde=true;aq.form_formaldehyde=1.75;
  f.observe(b,true,6,7000,air);assert(has(f.page(0,4,false),"No measurement"));
  aq.has_pm10_standard=aq.has_pm25_standard=aq.has_pm100_standard=aq.has_co2=true;aq.pm10_standard=1;aq.pm25_standard=2;aq.pm100_standard=3;aq.co2=415;
  f.observe(b,true,7,9000,air);for(const char *s:{"PM1 1ug/m3","PM2.5 2ug/m3","PM10 3ug/m3","CO2 415ppm","HCHO 1.75ppb"})find(f,4,s);
  meshtastic_Telemetry power{};power.which_variant=meshtastic_Telemetry_power_metrics_tag;auto &p=power.variant.power_metrics;
  p.has_ch1_voltage=true;p.ch1_voltage=0;p.has_ch3_current=true;p.ch3_current=-15;
  f.observe(b,true,8,11000,power);find(f,2,"Ch1 volts 0.00V");find(f,2,"Ch3 current -15mA");
  meshtastic_Telemetry health{};health.which_variant=meshtastic_Telemetry_health_metrics_tag;auto &h=health.variant.health_metrics;
  h.has_temperature=true;h.temperature=37.5f;h.has_heart_bpm=true;h.heart_bpm=76;h.has_spO2=true;h.spO2=98;
  f.observe(b,true,9,12000,health);find(f,8,"100F",true);find(f,8,"Heart 76bpm");find(f,8,"SpO2 98%");
  f.validate(7,21,12000+Frames::FreshMs+1,lookup);assert(has(f.page(0,8,false),"STALE")&&!has(f.page(0,8,false),"98%"));
  f.validate(7,22,400000,lookup);assert(has(f.page(0,4,false),"No measurement")); // channel identity changed
  f.observe(self,true,10,400001,env(50));f.validate(7,22,100,lookup);assert(has(f.page(0,1,false),"No measurement")); // monotonic clock reset
  f.observe({},true,11,500,env(90));assert(has(f.page(0,1,false),"No measurement"));
  printf("PASS stock telemetry fields, independently paged sources, sender identity, source order, missing/stale/private and epoch fences; compact owner %zu bytes\n",sizeof(f));
}
