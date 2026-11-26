// SPDX-License-Identifier: GPL-3.0-only
#include "ModuleJournal.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using namespace mt;
struct MemoryPowerSettings:SettingsBackend {
  std::map<std::string,std::vector<uint8_t>> rows;
  int read(const char*key,uint8_t*out,size_t n)override{auto i=rows.find(key);if(i==rows.end())return -ENOENT;if(i->second.size()>n)return -E2BIG;memcpy(out,i->second.data(),i->second.size());return int(i->second.size());}
  int write(const char*key,const uint8_t*in,size_t n)override{assert(std::string(key).rfind("mtmod/",0)==0);rows[key]={in,in+n};return 0;}
};
static meshtastic_ModuleConfig get(const ModuleSettings&s){meshtastic_ModuleConfig c{};assert(s.get(c,meshtastic_ModuleConfig_telemetry_tag));return c;}
int main(){
  static_assert(sizeof(ModuleSettings)==112);
  ModuleSettings legacy;auto c=get(legacy);auto before=legacy;assert(legacy.set(c)&&legacy.same(before)&&legacy.bytes[3]==1);
  c.payload_variant.telemetry.environment_measurement_enabled=true;c.payload_variant.telemetry.environment_update_interval=120;assert(legacy.set(c)&&legacy.bytes[3]==2);
  before=legacy;c=get(legacy);assert(legacy.set(c)&&legacy.same(before)&&!legacy.powerEnabled()&&!legacy.powerInterval());
  puts("PASS old v1 and v2 telemetry no-ops remain byte-identical without power promotion");
  auto power=legacy;c=get(power);c.payload_variant.telemetry.power_measurement_enabled=true;c.payload_variant.telemetry.power_update_interval=60;assert(power.set(c)&&power.valid()&&power.bytes[3]==3&&power.powerEnabled()&&power.powerInterval()==60&&power.environmentEnabled()&&power.environmentInterval()==120);
  c=get(power);assert(c.payload_variant.telemetry.power_measurement_enabled&&c.payload_variant.telemetry.power_update_interval==60);before=power;assert(power.set(c)&&power.same(before));
  c.payload_variant.telemetry.environment_measurement_enabled=false;c.payload_variant.telemetry.environment_update_interval=0;assert(power.set(c)&&power.bytes[3]==3&&power.powerEnabled());
  c.payload_variant.telemetry.power_measurement_enabled=false;c.payload_variant.telemetry.power_update_interval=0;assert(power.set(c)&&power.bytes[3]==3&&!power.powerEnabled()&&!power.environmentEnabled());
  puts("PASS explicit v3 power get/set and disable never downgrade or erase sibling settings");
  before=power;c=get(power);c.payload_variant.telemetry.power_update_interval=ModuleSettings::MaxIntervalSeconds+1;assert(!power.set(c)&&power.same(before));
  c=get(power);c.payload_variant.telemetry.power_screen_enabled=true;assert(!power.set(c)&&power.same(before));
  auto bad=legacy;bad.bytes[105]=1;bad.seal();assert(!bad.valid());bad=power;bad.bytes[105]=2;bad.seal();assert(!bad.valid());bad=power;Config::put32(bad.bytes+106,UINT32_MAX);bad.seal();assert(!bad.valid());bad=power;bad.bytes[3]=4;bad.seal();assert(!bad.valid());bad=power;bad.bytes[109]^=1;assert(!bad.valid());
  puts("PASS unsupported flags, invalid v3 values and legacy reserved bytes fail without partial mutation");
  MemoryPowerSettings store;ModuleJournal journal;assert(!journal.load(store)&&!journal.save(store,legacy));
  power=legacy;c=get(power);c.payload_variant.telemetry.power_measurement_enabled=true;c.payload_variant.telemetry.power_update_interval=ModuleSettings::MaxIntervalSeconds;assert(power.set(c)&&!journal.save(store,power));
  ModuleJournal reboot;assert(!reboot.load(store)&&reboot.config().same(power));assert(store.rows.at("mtmod/state1").size()==128);
  for(size_t cut=0;cut<128;++cut){auto torn=store;torn.rows["mtmod/state1"].resize(cut);ModuleJournal recovered;assert(!recovered.load(torn)&&recovered.config().same(legacy));}
  puts("PASS unchanged128-byte journal reloads v3 and recovers legacy v2 at all128 truncated-new-record boundaries");
}
