#pragma once
#include <cassert>
#include <cmath>
#include <cstdint>
// Explicit host seams: callers supply already-resolved, valid SF/BW. These
// stubs do not implement stock modem tables, node discovery or region policy.
enum Role {meshtastic_Config_DeviceConfig_Role_CLIENT=0,meshtastic_Config_DeviceConfig_Role_ROUTER=2,
 meshtastic_Config_DeviceConfig_Role_ROUTER_LATE=11,meshtastic_Config_DeviceConfig_Role_SENSOR=6,
 meshtastic_Config_DeviceConfig_Role_TRACKER=5,meshtastic_Config_DeviceConfig_Role_TAK_TRACKER=10};
struct FixtureConfig {struct {Role role=meshtastic_Config_DeviceConfig_Role_CLIENT;} device;
 struct {bool use_preset=true;unsigned modem_preset=0,spread_factor=11,bandwidth=250;uint8_t hop_limit=3;} lora;};
inline FixtureConfig config;
inline uint8_t resolvedSF=11;inline float resolvedBW=250;
inline unsigned presetCalls=0,manualCalls=0;
struct FixtureProfile {int8_t positionThrottle=1,telemetryThrottle=1;};
struct FixtureRegion {FixtureProfile *profile;};
inline FixtureProfile profile;inline FixtureRegion region{&profile};inline FixtureRegion *myRegion=&region;
inline void modemPresetToParams(unsigned,bool zero,float &bw,uint8_t &sf,uint8_t &cr){assert(!zero);++presetCalls;bw=resolvedBW;sf=resolvedSF;cr=1;}
inline float bwCodeToKHz(unsigned){++manualCalls;return resolvedBW;}
inline uint8_t clampSpreadFactor(uint8_t sf){assert(sf>=5&&sf<=12);return sf;}
inline float clampBandwidthKHz(float bw){assert(std::isfinite(bw)&&bw>0);return bw;}
inline uint32_t pow_of_2(uint8_t sf){return uint32_t(1)<<sf;}
#define IS_ONE_OF(v,a,b,c) ((v)==(a)||(v)==(b)||(v)==(c))
#define HOP_RELIABLE 3
#define HOP_MAX 7
