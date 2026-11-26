// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Config.h"
#include "meshtastic/module_config.pb.h"

namespace mt {
// Independent versioned module bytes: never reuse identity, channel, key or
// existing Config storage. Default records enable no background transmitter.
struct ModuleSettings {
  static constexpr size_t Size=112,StatusSize=80;
  static constexpr uint32_t MaxIntervalSeconds=0x7fffffffu/1000;
  uint8_t bytes[Size]{};
  ModuleSettings(){bytes[0]='Z';bytes[1]='T';bytes[2]='M';bytes[3]=1;seal();}
  void seal(){const auto c=Config::checksum(bytes,Size-2);bytes[110]=uint8_t(c);bytes[111]=uint8_t(c>>8);}
  static bool interval(uint32_t seconds){return seconds<=MaxIntervalSeconds;}
  static bool stockInterval(uint32_t seconds){return interval(seconds)||seconds==INT32_MAX;}
  bool valid()const {
    if(bytes[0]!='Z'||bytes[1]!='T'||bytes[2]!='M'||(bytes[3]!=1&&bytes[3]!=2&&bytes[3]!=3)||bytes[4]>1||bytes[9]>1||bytes[14]>1||bytes[15]>1||
       !stockInterval(Config::get32(bytes+5))||!stockInterval(Config::get32(bytes+10))||!interval(Config::get32(bytes+16)))return false;
    if(Config::get32(bytes+10)&&Config::get32(bytes+10)<14400)return false;
    const size_t n=strnlen(reinterpret_cast<const char *>(bytes+20),StatusSize);
    if(n==StatusSize)return false;
    for(size_t i=20+n;i<100;++i)if(bytes[i])return false;
    if(bytes[3]==1){for(size_t i=100;i<110;++i)if(bytes[i])return false;}
    else {
      if(bytes[100]>1||!stockInterval(Config::get32(bytes+101)))return false;
      if(bytes[3]==3){if(bytes[105]>1||!interval(Config::get32(bytes+106)))return false;}
      else for(size_t i=105;i<110;++i)if(bytes[i])return false;
    }
    const auto c=Config::checksum(bytes,Size-2);return bytes[110]==uint8_t(c)&&bytes[111]==uint8_t(c>>8);
  }
  bool same(const ModuleSettings &other)const{return !memcmp(bytes,other.bytes,Size);}
  bool environmentEnabled()const{return bytes[3]>=2&&bytes[100]==1;}
  uint32_t environmentInterval()const{return bytes[3]>=2?Config::get32(bytes+101):0;}
  bool powerEnabled()const{return bytes[3]==3&&bytes[105]==1;}
  uint32_t powerInterval()const{return bytes[3]==3?Config::get32(bytes+106):0;}
  static bool supported(unsigned tag){return tag==meshtastic_ModuleConfig_telemetry_tag||tag==meshtastic_ModuleConfig_neighbor_info_tag||tag==meshtastic_ModuleConfig_range_test_tag||tag==meshtastic_ModuleConfig_statusmessage_tag;}
  bool get(meshtastic_ModuleConfig &out,unsigned tag)const {
    if(!valid()||!supported(tag))return false;
    out={};out.which_payload_variant=tag;
    switch(tag){
    case meshtastic_ModuleConfig_telemetry_tag:out.payload_variant.telemetry.device_telemetry_enabled=bytes[4];out.payload_variant.telemetry.device_update_interval=Config::get32(bytes+5);out.payload_variant.telemetry.environment_measurement_enabled=environmentEnabled();out.payload_variant.telemetry.environment_update_interval=environmentInterval();out.payload_variant.telemetry.power_measurement_enabled=powerEnabled();out.payload_variant.telemetry.power_update_interval=powerInterval();break;
    case meshtastic_ModuleConfig_neighbor_info_tag:out.payload_variant.neighbor_info.enabled=bytes[9];out.payload_variant.neighbor_info.update_interval=Config::get32(bytes+10);out.payload_variant.neighbor_info.transmit_over_lora=bytes[14];break;
    case meshtastic_ModuleConfig_range_test_tag:out.payload_variant.range_test.enabled=bytes[15];out.payload_variant.range_test.sender=Config::get32(bytes+16);break;
    case meshtastic_ModuleConfig_statusmessage_tag:memcpy(out.payload_variant.statusmessage.node_status,bytes+20,StatusSize);break;
    default:return false;
    }
    return true;
  }
  bool set(const meshtastic_ModuleConfig &in){
    ModuleSettings candidate=*this;
    switch(in.which_payload_variant){
    case meshtastic_ModuleConfig_telemetry_tag:{
      const auto &t=in.payload_variant.telemetry;
      if(!stockInterval(t.device_update_interval)||!stockInterval(t.environment_update_interval)||!interval(t.power_update_interval)||t.environment_screen_enabled||t.environment_display_fahrenheit||t.air_quality_enabled||t.air_quality_interval||t.power_screen_enabled||t.health_measurement_enabled||t.health_update_interval||t.health_screen_enabled||t.air_quality_screen_enabled)return false;
      candidate.bytes[4]=t.device_telemetry_enabled;Config::put32(candidate.bytes+5,t.device_update_interval);
      // Old v1/v2 no-ops retain every byte; promotion never downgrades an
      // existing record. Only explicit power configuration consumes the final
      // five reserved bytes. Record112/journal128 and Config remain unchanged.
      if(t.power_measurement_enabled||t.power_update_interval)candidate.bytes[3]=3;
      else if((t.environment_measurement_enabled||t.environment_update_interval)&&candidate.bytes[3]<2)candidate.bytes[3]=2;
      candidate.bytes[100]=t.environment_measurement_enabled;Config::put32(candidate.bytes+101,t.environment_update_interval);
      candidate.bytes[105]=t.power_measurement_enabled;Config::put32(candidate.bytes+106,t.power_update_interval);break;
    }
    case meshtastic_ModuleConfig_neighbor_info_tag:{
      const auto &n=in.payload_variant.neighbor_info;
      if(!stockInterval(n.update_interval)||(n.update_interval&&n.update_interval<14400))return false;
      candidate.bytes[9]=n.enabled;Config::put32(candidate.bytes+10,n.update_interval);candidate.bytes[14]=n.transmit_over_lora;break;
    }
    case meshtastic_ModuleConfig_range_test_tag:{
      const auto &r=in.payload_variant.range_test;if(!interval(r.sender)||r.save||r.clear_on_reboot)return false;
      candidate.bytes[15]=r.enabled;Config::put32(candidate.bytes+16,r.sender);break;
    }
    case meshtastic_ModuleConfig_statusmessage_tag:{
      const auto &s=in.payload_variant.statusmessage;const size_t n=strnlen(s.node_status,StatusSize);if(n==StatusSize)return false;
      memset(candidate.bytes+20,0,StatusSize);memcpy(candidate.bytes+20,s.node_status,n);break;
    }
    default:return false;
    }
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
};
static_assert(sizeof(ModuleSettings)==ModuleSettings::Size);
}
