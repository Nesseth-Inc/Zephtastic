// SPDX-License-Identifier: GPL-3.0-only
// Root integrates the Qwen coverage plan with actual Observed presence semantics.
// Directly includes the production body; only the donor C API is fake.
#include "environment/ZephyrEnvironment.cpp"
#include <cassert>
#include <errno.h>
#include <cstdio>
#include <limits>
namespace {
env_sensor_reading values[32]{};bool powerKind[32]{},fail[32]{};int count=0,initResult=0,inits=0,counts=0,reads=0,last=-1;
void reset(){for(auto&v:values)v={};for(auto&v:fail)v=false;for(auto&v:powerKind)v=false;count=initResult=inits=counts=reads=0;last=-1;
 mt::environment::latest.invalidate();mt::environment::latestPower.invalidate();mt::environment::initialized=false;mt::environment::initializationReady=false;mt::environment::sampled=false;mt::environment::sampledAt=0;}
void set(int i,uint16_t flags,float t=0,float h=0,float p=0){values[i].fields=flags;values[i].temperature_c=t;values[i].humidity_pct=h;values[i].pressure_hpa=p;}
void empty(uint64_t at){auto s=mt::environment::snapshot(at);assert(!s.ambientCelsius&&!s.humidityPercent&&!s.pressureHpa);}
}
extern "C" int env_sensors_init(){++inits;return initResult;}
extern "C" int env_sensor_count(){++counts;return count;}
extern "C" bool env_sensor_is_power(int i){assert(i>=0&&i<count&&i<16);return powerKind[i];}
extern "C" int env_sensor_read_metrics(int i,env_sensor_reading*out){++reads;last=i;assert(i>=0&&i<32);if(fail[i])return -EIO;*out=values[i];return 0;}
int main(){using mt::environment::poll;using mt::environment::snapshot;
 reset();poll(false,0);empty(0);assert(!inits&&!reads&&!counts);count=1;set(0,ENV_F_TEMPERATURE,0);poll(true,0);assert(inits==1&&reads==1&&snapshot(0).ambientCelsius&&*snapshot(0).ambientCelsius==0);
 poll(false,1);empty(1);poll(true,2);assert(inits==1&&reads==2);
 puts("PASS disabled acquisition has no donor calls; explicit enable and reenable preserve measured zero and init once");
 reset();count=1;set(0,ENV_F_TEMPERATURE,10);poll(true,1000);set(0,ENV_F_TEMPERATURE,20);poll(true,1999);assert(reads==1&&*snapshot(1999).ambientCelsius==10);poll(true,2000);assert(reads==2&&*snapshot(2000).ambientCelsius==20);
 poll(true,1500);empty(1500);assert(reads==2);poll(true,1500);assert(reads==3&&*snapshot(1500).ambientCelsius==20);
 puts("PASS exact1000ms sampling boundary, no early reread, backwards time invalidation and subsequent new sample");
 reset();count=4;set(0,ENV_F_TEMPERATURE,0,999,999);set(1,ENV_F_TEMPERATURE|ENV_F_HUMIDITY,30,0);set(2,ENV_F_HUMIDITY|ENV_F_PRESSURE,999,55,1013.25f);set(3,ENV_F_PRESSURE,999,999,1000);poll(true,1000);auto s=snapshot(1000);assert(reads==4&&s.ambientCelsius&&*s.ambientCelsius==0&&s.humidityPercent&&*s.humidityPercent==0&&s.pressureHpa&&*s.pressureHpa==1013.25f);
 puts("PASS first valid per-field merge retains present zero and donor hPa without rescaling or flag-cleared values");
 reset();count=4;
 using Source=zt::telemetry::EnvironmentSource;
 auto producer=[&](int i,Source source){values[i].environment_source=static_cast<uint8_t>(source);};
 set(0,ENV_F_TEMPERATURE|ENV_F_HUMIDITY,20,48);producer(0,Source::Sht);
 set(1,ENV_F_TEMPERATURE|ENV_F_PRESSURE,18,0,980);producer(1,Source::Bmp3xx);
 set(2,ENV_F_TEMPERATURE|ENV_F_HUMIDITY,30,99);producer(2,Source::Aht);
 set(3,ENV_F_TEMPERATURE|ENV_F_PRESSURE,19,0,1000);producer(3,Source::Lps22hb);
 poll(true,0);s=snapshot(0);assert(*s.ambientCelsius==20&&*s.humidityPercent==48&&*s.pressureHpa==980);
 fail[0]=true;poll(true,1000);s=snapshot(1000);assert(*s.ambientCelsius==18&&!s.humidityPercent&&*s.pressureHpa==980);
 fail[1]=true;poll(true,2000);s=snapshot(2000);assert(*s.ambientCelsius==19&&*s.humidityPercent==99&&*s.pressureHpa==1000);
 puts("PASS shuffled SHT, BMP3xx, AHT and LPS probes preserve donor field precedence and failed-source fallback");
 reset();count=4;fail[0]=true;set(0,ENV_F_TEMPERATURE,99);set(1,ENV_F_TEMPERATURE|ENV_F_HUMIDITY|ENV_F_PRESSURE,std::numeric_limits<float>::quiet_NaN(),101,-1);set(2,ENV_F_TEMPERATURE|ENV_F_HUMIDITY|ENV_F_PRESSURE,std::numeric_limits<float>::infinity(),-1,0);set(3,ENV_F_TEMPERATURE,21.5f);poll(true,0);s=snapshot(0);assert(reads==4&&s.ambientCelsius&&*s.ambientCelsius==21.5f&&!s.humidityPercent&&!s.pressureHpa);
 for(auto&v:fail)v=true;poll(true,1000);empty(1000);
 puts("PASS failed reads and nonfinite or out-of-range fields skipped; all-failed sample immediately clears prior readings");
 reset();count=32;for(int i=0;i<15;++i)fail[i]=true;set(15,ENV_F_TEMPERATURE,15);set(16,ENV_F_TEMPERATURE,100);poll(true,0);assert(reads==16&&last==15&&*snapshot(0).ambientCelsius==15);
 reset();count=-1;poll(true,0);empty(0);assert(!reads);
 puts("PASS device enumeration bounded to sixteen reads, with no invalid-index call for negative count");
 reset();count=1;set(0,ENV_F_TEMPERATURE,20);initResult=-EIO;poll(true,0);empty(0);poll(true,1000);empty(1000);poll(false,1001);poll(true,1002);empty(1002);assert(inits==1&&!reads&&!counts);
 puts("PASS failed initialization latches unavailable and never reads a partial donor table or silently retries");
 reset();empty(0);count=1;set(0,ENV_F_TEMPERATURE,20);poll(true,1000);empty(999);assert(snapshot(1000).ambientCelsius&&snapshot(90999).ambientCelsius&&snapshot(91000).ambientCelsius);empty(91001);
 reset();poll(true,UINT64_MAX);empty(UINT64_MAX);assert(!inits&&!reads);poll(true,uint64_t(INT64_MAX));assert(inits==1);poll(false,UINT64_MAX);empty(UINT64_MAX);
 puts("PASS unknown/future/stale snapshots omit data;90-second inclusive limit and int64 clock guard are exact");
 reset();count=5;powerKind[1]=powerKind[2]=powerKind[3]=powerKind[4]=true;set(0,ENV_F_TEMPERATURE,0);
 for(int i=1;i<5;++i){values[i].fields=ENV_F_VOLTAGE|ENV_F_CURRENT;values[i].voltage_v=float(i);values[i].current_a=-0.125f;}
 fail[1]=true;poll(true,true,0);auto power=mt::environment::powerSnapshot(0);
 assert(inits==1&&reads==5&&snapshot(0).ambientCelsius&&!power.voltage[0]&&power.voltage[1]&&*power.voltage[1]==2&&*power.current[1]==-125&&*power.voltage[2]==3&&power.voltage[3]&&*power.voltage[3]==4);
 fail[1]=false;poll(true,true,1000);power=mt::environment::powerSnapshot(1000);assert(inits==1&&reads==10&&*power.voltage[0]==1&&*power.voltage[1]==2);
 puts("PASS one shared initialization/fetch path, eight addressable native power channels stable across failed reads, exact signed A-to-mA");
 poll(false,true,2000);empty(2000);assert(reads==14&&mt::environment::powerSnapshot(2000).voltage[0]);
 poll(true,false,3000);assert(reads==15&&snapshot(3000).ambientCelsius);power=mt::environment::powerSnapshot(3000);for(unsigned i=0;i<8;++i)assert(!power.voltage[i]&&!power.current[i]);
 puts("PASS independent enable/disable clears only corresponding cache and skips disabled native sensor classes");
 poll(true,true,4000);for(auto&v:fail)v=true;poll(true,true,5000);empty(5000);power=mt::environment::powerSnapshot(5000);for(unsigned i=0;i<8;++i)assert(!power.voltage[i]&&!power.current[i]);
 puts("PASS failed sample replaces prior successful power readings with absence");
 for(auto&v:fail)v=false;poll(false,true,6000);assert(mt::environment::powerSnapshot(96000).voltage[0]);power=mt::environment::powerSnapshot(96001);assert(!power.voltage[0]);power=mt::environment::powerSnapshot(5999);assert(!power.voltage[0]);
 puts("PASS power future/stale bounds match environment inclusive90-second snapshot");
}
