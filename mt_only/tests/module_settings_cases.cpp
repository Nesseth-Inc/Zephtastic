// SPDX-License-Identifier: GPL-3.0-only
#include "ModuleJournal.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using namespace mt;
struct Backend:SettingsBackend {
 std::map<std::string,std::vector<uint8_t>> rows;bool afterWriteError=false,readbackError=false;unsigned writes=0;
 int read(const char *key,uint8_t*out,size_t cap)override {
  if(readbackError)return -EIO;
  const auto it=rows.find(key);if(it==rows.end())return -ENOENT;
  if(it->second.size()>cap)return -E2BIG;
  memcpy(out,it->second.data(),it->second.size());return int(it->second.size());
 }
 int write(const char *key,const uint8_t*p,size_t n)override {
  assert(std::string(key).rfind("mtmod/",0)==0);++writes;rows[key]={p,p+n};return afterWriteError?-EIO:0;
 }
};
static ModuleSettings status(const char *text){ModuleSettings s;meshtastic_ModuleConfig m{};m.which_payload_variant=meshtastic_ModuleConfig_statusmessage_tag;strcpy(m.payload_variant.statusmessage.node_status,text);assert(s.set(m));return s;}
static uint32_t crc(const uint8_t*p,size_t n){uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;}
static void reseal(std::vector<uint8_t>&v){Config::put32(v.data()+124,crc(v.data(),124));}
int main(){
 {ModuleSettings s;assert(s.valid());for(unsigned tag:{5u,6u,10u,14u}){meshtastic_ModuleConfig m{};assert(s.get(m,tag));assert(s.set(m));}assert(!ModuleSettings::supported(1));puts("PASS disabled module defaults and selected getters");}
 {ModuleSettings s;meshtastic_ModuleConfig m{};m.which_payload_variant=meshtastic_ModuleConfig_telemetry_tag;auto&t=m.payload_variant.telemetry;t.device_telemetry_enabled=true;t.device_update_interval=30;assert(s.set(m));meshtastic_ModuleConfig out{};assert(s.get(out,m.which_payload_variant)&&out.payload_variant.telemetry.device_telemetry_enabled&&out.payload_variant.telemetry.device_update_interval==30);auto before=s;t.power_screen_enabled=true;assert(!s.set(m)&&s.same(before));t.power_screen_enabled=false;t.device_update_interval=ModuleSettings::MaxIntervalSeconds+1;assert(!s.set(m));puts("PASS measured device telemetry and unsupported screen and interval bounds");}
 {ModuleSettings s;meshtastic_ModuleConfig m{};m.which_payload_variant=meshtastic_ModuleConfig_neighbor_info_tag;auto&n=m.payload_variant.neighbor_info;n.enabled=true;n.update_interval=14399;assert(!s.set(m));n.update_interval=14400;n.transmit_over_lora=true;assert(s.set(m));meshtastic_ModuleConfig out{};assert(s.get(out,m.which_payload_variant)&&out.payload_variant.neighbor_info.transmit_over_lora);puts("PASS neighbor interval and retained LoRa preference");}
 {ModuleSettings s;meshtastic_ModuleConfig m{};m.which_payload_variant=meshtastic_ModuleConfig_range_test_tag;auto&r=m.payload_variant.range_test;r.enabled=true;r.sender=1;assert(s.set(m));r.save=true;assert(!s.set(m));r.save=false;r.clear_on_reboot=true;assert(!s.set(m));puts("PASS range sender seconds and unsupported filesystem flags");}
 {auto s=status("test");meshtastic_ModuleConfig m{};m.which_payload_variant=meshtastic_ModuleConfig_statusmessage_tag;memset(m.payload_variant.statusmessage.node_status,'x',80);auto before=s;assert(!s.set(m)&&s.same(before));m.payload_variant.statusmessage.node_status[79]=0;assert(s.set(m));m.payload_variant.statusmessage.node_status[0]=0;assert(s.set(m));for(unsigned i=20;i<100;++i)assert(!s.bytes[i]);s.bytes[100]=1;s.seal();assert(!s.valid());puts("PASS bounded status text, clearing and reserved bytes");}
 {Backend b;ModuleJournal j;assert(j.load(b)==0&&j.ready()&&j.config().valid());auto s=status("first");assert(j.save(b,s)==0&&b.writes==1&&j.generation()==1);assert(j.save(b,s)==0&&b.writes==1);s=status("second");assert(j.save(b,s)==0&&j.generation()==2);ModuleJournal reloaded;assert(reloaded.load(b)==0&&reloaded.config().same(s));puts("PASS independent dual-slot durable save and restart");}
 {Backend b;ModuleJournal j;assert(j.load(b)==0);auto first=status("first");assert(j.save(b,first)==0);assert(j.save(b,status("second"))==0);b.rows["mtmod/state1"][20]^=1;ModuleJournal recovered;assert(recovered.load(b)==0&&recovered.state()==ModuleJournal::State::Recovered&&recovered.config().same(first));puts("PASS torn/corrupt newest record recovers previous verified state");}
 {Backend b;ModuleJournal j;assert(j.load(b)==0);auto before=j.config();b.afterWriteError=true;auto committed=status("uncertain");assert(j.save(b,committed)==-EIO&&!j.ready()&&j.config().same(before));assert(j.save(b,before)==-EACCES);b.afterWriteError=false;ModuleJournal reloaded;assert(reloaded.load(b)==0&&reloaded.config().same(committed));puts("PASS uncertain committed write poisons further saves until reload");}
 {Backend b;ModuleJournal j;assert(j.load(b)==0);assert(j.save(b,status("a"))==0);assert(j.save(b,status("b"))==0);Config::put32(b.rows["mtmod/state1"].data()+4,1);reseal(b.rows["mtmod/state1"]);ModuleJournal conflict;assert(conflict.load(b)==-EINVAL&&!conflict.ready());Config::put32(b.rows["mtmod/state1"].data()+4,0x80000001u);reseal(b.rows["mtmod/state1"]);assert(conflict.load(b)==-EINVAL);puts("PASS same-generation conflict and half-range ambiguity refusal");}
 {Backend b;b.rows["mtmod/state0"]={1,2,3};b.rows["mtbase/state0"]={4,5,6};ModuleJournal j;assert(j.load(b)==-EINVAL&&!j.ready()&&b.writes==0);assert(b.rows["mtbase/state0"]==std::vector<uint8_t>({4,5,6}));puts("PASS invalid module storage never resets existing core storage");}
 puts("MODULE_SETTINGS_GROUPS 10");
}
