// Exact pinned stock Default bodies, explicit host seams, real pure ZT headers.
#ifdef NDEBUG
#error Interval qualification requires active assertions
#endif
#include "ModuleIntervals.h"
#include "NeighborIntervals.h"
#include "NeighborReplyWindow.h"
#include "Default.h"
#include <cassert>
#include <climits>
#include <iostream>
#include <limits>
static unsigned groups=0,comparisons=0;
static void pass(const char *name){++groups;std::cout<<"PASS "<<name<<'\n';}
static void resolve(bool router,unsigned sf,float bw,bool preset=true){
 config.device.role=router?meshtastic_Config_DeviceConfig_Role_ROUTER:meshtastic_Config_DeviceConfig_Role_CLIENT;
 config.lora.use_preset=preset;resolvedSF=uint8_t(sf);resolvedBW=bw;config.lora.spread_factor=sf;
 profile.positionThrottle=1;profile.telemetryThrottle=1;myRegion=&region;region.profile=&profile;
}
int main(){
 const uint32_t maximum=uint32_t(INT32_MAX),poison=0xdeadbeef;
 for(bool router:{false,true})for(uint32_t seconds:{0u,1u,60u,3600u,43200u,2147483u,2147484u,UINT32_MAX}){
  resolve(router,11,250);assert(mt::ModuleIntervals::base(seconds,router)==Default::getConfiguredOrDefaultMs(seconds));++comparisons;
 }
 assert(mt::ModuleIntervals::base(2147483,false)==2147483000u&&mt::ModuleIntervals::base(2147484,false)==maximum);
 pass("seconds conversion and role defaults match literal stock with INT32 saturation");
 for(bool router:{false,true})for(bool preset:{false,true})for(unsigned sf:{5u,7u,9u,11u,12u})
 for(float bw:{7.8125f,125.f,250.f,500.f})for(uint32_t online:{0u,1u,40u,41u,60u,65535u})
 for(uint32_t seconds:{0u,1u,3600u,2147484u})for(int multiplier:{-128,-1,0,1,2,127}){
  resolve(router,sf,bw,preset);profile.telemetryThrottle=int8_t(multiplier);uint32_t value=poison;
  assert(mt::ModuleIntervals::device(seconds,router,true,online,sf,bw,USERPREFS_EVENT_MODE,int8_t(multiplier),value));
  const auto expected=Default::getConfiguredOrDefaultMsScaled(seconds,default_telemetry_broadcast_interval_secs,online,TrafficType::TELEMETRY);
  assert(value==expected&&value<=maximum);++comparisons;
 }
 pass("resolved SF/BW, online0/40/41/65535, role bypass, event mode and regional throttle match literal stock");
 for(bool preset:{false,true}){
  resolve(false,11,250,preset);presetCalls=manualCalls=0;uint32_t value=0;
  assert(mt::ModuleIntervals::device(3600,false,true,41,11,250,USERPREFS_EVENT_MODE,1,value));
  assert(value==Default::getConfiguredOrDefaultMsScaled(3600,3600,41));
  ++comparisons;
  assert(preset?presetCalls==1&&manualCalls==0:manualCalls==1&&presetCalls==0);
 }
 for(Role role:{meshtastic_Config_DeviceConfig_Role_SENSOR,meshtastic_Config_DeviceConfig_Role_TRACKER,meshtastic_Config_DeviceConfig_Role_TAK_TRACKER}){
  resolve(false,11,250);config.device.role=role;uint32_t value=0;
  assert(mt::ModuleIntervals::scale(3600000,65535,0,std::numeric_limits<float>::quiet_NaN(),false,true,value));
  assert(value==Default::getConfiguredOrDefaultMsScaled(3600,3600,65535));
  ++comparisons;
 }
 resolve(true,11,250);config.device.role=meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;uint32_t late=0;
 assert(mt::ModuleIntervals::device(0,true,false,UINT32_MAX,0,NAN,false,1,late)&&late==Default::getConfiguredOrDefaultMsScaled(0,default_telemetry_broadcast_interval_secs,65535));
 ++comparisons;
 for(bool absentRegion:{false,true}){resolve(false,11,250);if(absentRegion)myRegion=nullptr;else region.profile=nullptr;uint32_t unthrottled=0;
  assert(mt::ModuleIntervals::device(3600,false,true,41,11,250,USERPREFS_EVENT_MODE,1,unthrottled)&&unthrottled==Default::getConfiguredOrDefaultMsScaled(3600,3600,41,TrafficType::TELEMETRY));++comparisons;}
 resolve(false,11,250);profile.positionThrottle=2;profile.telemetryThrottle=3;
 assert(Default::getConfiguredOrDefaultMsScaled(1,3600,40,TrafficType::POSITION)==mt::ModuleIntervals::throttle(1000,2));
 assert(Default::getConfiguredOrDefaultMsScaled(1,3600,40,TrafficType::TELEMETRY)==mt::ModuleIntervals::throttle(1000,3));
 comparisons+=2;
 pass("stock preset/manual resolver seams run, and explicit priority-role bypass needs no modem values");
 for(bool router:{false,true})for(bool known:{false,true}){
  uint32_t value=poison;const bool ok=mt::ModuleIntervals::device(0,router,known,UINT32_MAX,0,std::numeric_limits<float>::quiet_NaN(),false,2,value);
  assert(ok==router);assert(value==(router?86400000u:poison));
 }
 for(uint32_t online:{0u,40u,41u}){uint32_t unchanged=poison;
  assert(!mt::ModuleIntervals::device(0,false,false,online,11,250,false,1,unchanged)&&unchanged==poison);}
 for(uint32_t online:{0u,40u}){uint32_t value=poison;assert(mt::ModuleIntervals::scale(1234,online,0,NAN,false,false,value)&&value==1234);}
 pass("unknown online count refuses nonrouters without output writes; routers and small known networks bypass modem dependencies");
 for(float bw:{-1.f,0.f,std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::denorm_min()}){
  uint32_t value=poison;assert(!mt::ModuleIntervals::scale(1234,41,11,bw,false,false,value)&&value==poison);
 }
 for(unsigned sf:{0u,4u,13u,UINT_MAX}){uint32_t value=poison;assert(!mt::ModuleIntervals::scale(1234,41,sf,250,false,false,value)&&value==poison);}
 for(bool bypass:{false,true}){uint32_t value=poison;assert(!mt::ModuleIntervals::scale(UINT32_MAX,1,11,250,false,bypass,value)&&value==poison);
  assert(!mt::ModuleIntervals::scale(1,UINT32_MAX,11,250,false,bypass,value)&&value==poison);}
 uint32_t value=poison;assert(!mt::ModuleIntervals::device(3600,false,true,41,11,-1,false,1,value)&&value==poison);
 assert(mt::ModuleIntervals::scale(maximum,65535,12,7.8125f,true,false,value)&&value==maximum);
 assert(mt::ModuleIntervals::throttle(UINT32_MAX,-1)==maximum&&mt::ModuleIntervals::throttle(maximum,127)==maximum);
 pass("negative/nonfinite/overflow inputs refuse atomically or saturate deliberately; no timer wraps");
 // Both modes produce coef1.0819200277: double product2147483588.486<INT32_MAX,
 // but float multiplication rounds to2^31. Stock returns that uint32 value;
 // ZT deliberately clamps it to INT32_MAX for its signed timer consumers.
 resolve(false,5,USERPREFS_EVENT_MODE?31.25f:7.8125f);
 const auto stockEdge=Default::getConfiguredOrDefaultMsScaled(1984882,3600,42);
 assert(stockEdge==uint32_t(INT32_MAX)+1u);
 ++comparisons;
 assert(mt::ModuleIntervals::device(1984882,false,true,42,5,resolvedBW,USERPREFS_EVENT_MODE,1,value)&&value==maximum);
 pass("post-float 2^31 boundary intentionally clamps conservatively rather than claiming universal stock bit identity");
 for(bool router:{false,true})for(uint32_t seconds:{0u,1u,14400u,21600u,43200u,UINT32_MAX}){
  resolve(router,11,250);
  assert(mt::NeighborIntervals::initialMs(seconds,router)==Default::getConfiguredOrDefaultMs(seconds,default_telemetry_broadcast_interval_secs));
  assert(mt::NeighborIntervals::advertisedSeconds(seconds,router)==Default::getConfiguredOrDefault(seconds,default_telemetry_broadcast_interval_secs));
  assert(mt::NeighborIntervals::steadyMs(seconds)==Default::getConfiguredOrDefaultMs(seconds,default_neighbor_info_broadcast_secs));comparisons+=3;
 }
 assert(mt::NeighborIntervals::initialMs(0,false)==3600000&&mt::NeighborIntervals::initialMs(0,true)==43200000);
 assert(mt::NeighborIntervals::steadyMs(0)==21600000&&mt::NeighborIntervals::advertisedSeconds(0,true)==43200);
 pass("Neighbor constructor, advertised seconds and steady interval retain three distinct stock call-site defaults");
 mt::NeighborReplyWindow window;assert(window.allowed(0)&&window.allowed(1)); // failed allocation consumes nothing
 window.accepted(0);assert(!window.allowed(0)&&!window.allowed(179999)&&window.allowed(180000));
 assert(window.allowed(180001));assert(window.allowed(180002)); // failed enqueue leaves old window
 window.accepted(180002);assert(!window.allowed(180003));window.reset();assert(window.allowed(0));
 const uint32_t start=UINT32_MAX-17;window.accepted(start);assert(!window.allowed(uint32_t(start+179999))&&window.allowed(uint32_t(start+180000)));
 window.reset();assert(window.allowed(start));window.accepted(77);assert(!window.allowed(78));
 pass("global accepted-only reply window handles accepted0, failures, duplicate reads, wrap and reset");
 std::cout<<"COMPARISONS "<<comparisons<<" GROUPS "<<groups<<'\n';
}
