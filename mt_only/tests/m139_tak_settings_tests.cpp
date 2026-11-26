// SPDX-License-Identifier: GPL-3.0-only
#include "modules/ExtendedModuleJournal.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using Settings=mt::modules::ExtendedModuleSettings;
using Journal=mt::modules::ExtendedModuleJournal;

struct Backend:mt::SettingsBackend {
  std::map<std::string,std::vector<uint8_t>> records;
  std::vector<std::string> reads,writes;
  int read(const char *key,uint8_t *out,size_t cap)override {
    reads.emplace_back(key);auto it=records.find(key);if(it==records.end())return -ENOENT;
    if(it->second.size()>cap)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());return int(it->second.size());
  }
  int write(const char *key,const uint8_t *p,size_t n)override {
    writes.emplace_back(key);records[key]={p,p+n};return 0;
  }
};

static void put16(uint8_t *p,uint16_t v){p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static uint32_t crc32(const uint8_t *p,size_t n){
  uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;
}
static void sealInner(uint8_t *p){const auto c=mt::Config::checksum(p,318);p[318]=uint8_t(c);p[319]=uint8_t(c>>8);}
static void sealOuter(std::vector<uint8_t> &v){put32(v.data()+v.size()-4,crc32(v.data(),v.size()-4));}
static std::vector<uint8_t> dense(const uint8_t *inner,uint32_t generation=4){
  std::vector<uint8_t> r(Journal::RecordSize);r[0]='Z';r[1]='T';r[2]='X';r[3]=Journal::DenseVersion;
  put32(r.data()+4,generation);put16(r.data()+8,Settings::Size);memcpy(r.data()+Journal::HeaderSize,inner,Settings::Size);sealOuter(r);return r;
}
static std::vector<uint8_t> sparse(const uint8_t *inner,uint32_t generation=5){
  std::vector<uint8_t> r(Journal::V2FixedSize);r[0]='Z';r[1]='T';r[2]='X';r[3]=Journal::SparseVersion;
  put32(r.data()+4,generation);put16(r.data()+8,Settings::Size);size_t count=0;
  for(size_t i=0;i<Settings::Size;++i)if(inner[i])++count;
  put16(r.data()+10,uint16_t(count));
  for(size_t i=0;i<Settings::Size;++i)if(inner[i]){r[Journal::HeaderSize+i/8]|=uint8_t(1u<<(i%8));r.push_back(inner[i]);}
  r.resize(r.size()+4);sealOuter(r);assert(r.size()<Journal::RecordSize);return r;
}
static std::vector<uint8_t> legacyInner(uint8_t version){
  Settings s;auto inner=std::vector<uint8_t>(s.bytes,s.bytes+Settings::Size);inner[3]=version;
  if(version==Settings::LegacyVersion){memset(inner.data()+Settings::CannedEnabled,0,Settings::Size-2-Settings::CannedEnabled);}
  else {inner[Settings::TakTeam]=0;inner[Settings::TakRole]=0;for(size_t i=315;i<318;++i)inner[i]=0;}
  sealInner(inner.data());return inner;
}
static void setBadEnum(void *p,size_t n,uint32_t value){memcpy(p,&value,n);}
static meshtastic_ModuleConfig module(meshtastic_Team team,meshtastic_MemberRole role){
  meshtastic_ModuleConfig m{};m.which_payload_variant=meshtastic_ModuleConfig_tak_tag;
  m.payload_variant.tak.team=team;m.payload_variant.tak.role=role;return m;
}
static void assertDefaultTak(const Settings &s){
  meshtastic_ModuleConfig_TAKConfig out{};assert(s.tak(out));
  assert(out.team==meshtastic_Team_Unspecifed_Color&&out.role==meshtastic_MemberRole_Unspecifed);
}
static void migration(const std::vector<uint8_t> &record,uint32_t generation){
  Backend b;b.records["mtextra/state0"]=record;Journal j;
  assert(j.load(b)==0&&j.state()==Journal::State::Loaded&&j.generation()==generation&&b.writes.empty());
  assert(j.config().valid()&&j.config().bytes[3]==Settings::Version);assertDefaultTak(j.config());
  meshtastic_ModuleConfig out{};assert(j.config().get(out,meshtastic_ModuleConfig_traffic_management_tag));
  assert(out.payload_variant.traffic_management.position_min_interval_secs==21600);
  assert(j.config().get(out,meshtastic_ModuleConfig_canned_message_tag)&&!out.payload_variant.canned_message.enabled);
  assert(j.config().get(out,meshtastic_ModuleConfig_serial_tag)&&!out.payload_variant.serial.enabled);
  assert(j.save(b,j.config())==0&&b.writes.empty());
}

int main(){
  Settings s;assert(s.valid()&&s.bytes[3]==Settings::Version&&sizeof(s.bytes)==320);
  assertDefaultTak(s);
  meshtastic_ModuleConfig canned{};canned.which_payload_variant=meshtastic_ModuleConfig_canned_message_tag;
  canned.payload_variant.canned_message.enabled=true;canned.payload_variant.canned_message.send_bell=true;
  memcpy(canned.payload_variant.canned_message.allow_input_source,"_any",5);assert(s.set(canned));
  meshtastic_ModuleConfig serial{};serial.which_payload_variant=meshtastic_ModuleConfig_serial_tag;
  serial.payload_variant.serial.enabled=true;serial.payload_variant.serial.echo=true;
  serial.payload_variant.serial.rxd=12;serial.payload_variant.serial.txd=13;
  serial.payload_variant.serial.baud=meshtastic_ModuleConfig_SerialConfig_Serial_Baud_BAUD_9600;
  serial.payload_variant.serial.timeout=42;serial.payload_variant.serial.mode=meshtastic_ModuleConfig_SerialConfig_Serial_Mode_SIMPLE;
  assert(s.set(serial));
  meshtastic_ModuleConfig traffic{};traffic.which_payload_variant=meshtastic_ModuleConfig_traffic_management_tag;
  traffic.payload_variant.traffic_management.position_min_interval_secs=321;
  traffic.payload_variant.traffic_management.rate_limit_max_packets=17;assert(s.set(traffic));
  meshtastic_ModuleConfig beacon{};beacon.which_payload_variant=meshtastic_ModuleConfig_mesh_beacon_tag;
  beacon.payload_variant.mesh_beacon.flags=7;beacon.payload_variant.mesh_beacon.broadcast_interval_secs=600;
  assert(s.set(beacon));
  const Settings adjacentBefore=s;
  auto redMedic=module(meshtastic_Team_Red,meshtastic_MemberRole_Medic);
  assert(s.set(redMedic)&&s.valid()&&!s.same(adjacentBefore));
  assert(!memcmp(s.bytes,adjacentBefore.bytes,Settings::TakTeam)&&
         s.bytes[315]==adjacentBefore.bytes[315]&&s.bytes[316]==adjacentBefore.bytes[316]&&
         s.bytes[317]==adjacentBefore.bytes[317]);
  meshtastic_ModuleConfig out{};assert(s.get(out,meshtastic_ModuleConfig_tak_tag));
  assert(out.payload_variant.tak.team==meshtastic_Team_Red&&out.payload_variant.tak.role==meshtastic_MemberRole_Medic);
  Settings roundtrip=s;assert(roundtrip.valid()&&roundtrip.same(s));
  assert(s.get(out,meshtastic_ModuleConfig_canned_message_tag)&&out.payload_variant.canned_message.enabled&&out.payload_variant.canned_message.send_bell&&
         !strcmp(out.payload_variant.canned_message.allow_input_source,"_any"));
  assert(s.get(out,meshtastic_ModuleConfig_serial_tag)&&out.payload_variant.serial.enabled&&out.payload_variant.serial.echo&&
         out.payload_variant.serial.rxd==12&&out.payload_variant.serial.txd==13&&out.payload_variant.serial.timeout==42);
  assert(s.get(out,meshtastic_ModuleConfig_traffic_management_tag)&&out.payload_variant.traffic_management.position_min_interval_secs==321&&
         out.payload_variant.traffic_management.rate_limit_max_packets==17);
  assert(s.get(out,meshtastic_ModuleConfig_mesh_beacon_tag)&&out.payload_variant.mesh_beacon.flags==7&&
         out.payload_variant.mesh_beacon.broadcast_interval_secs==600);

  auto invalid=module(meshtastic_Team_Red,meshtastic_MemberRole_Medic);setBadEnum(&invalid.payload_variant.tak.team,sizeof(invalid.payload_variant.tak.team),UINT32_MAX);
  const auto good=s;assert(!s.set(invalid)&&s.same(good));
  invalid=module(meshtastic_Team_Red,meshtastic_MemberRole_Medic);setBadEnum(&invalid.payload_variant.tak.role,sizeof(invalid.payload_variant.tak.role),UINT32_MAX);
  assert(!s.set(invalid)&&s.same(good));
  auto bad=s;bad.bytes[Settings::TakTeam]=uint8_t(meshtastic_Team_Brown)+1;bad.seal();assert(!bad.valid());
  bad=s;bad.bytes[Settings::TakRole]=uint8_t(meshtastic_MemberRole_K9)+1;bad.seal();assert(!bad.valid());
  bad=s;bad.bytes[315]=1;bad.seal();assert(!bad.valid());
  bad=s;bad.bytes[Settings::TakTeam]=uint8_t(meshtastic_Team_Green);assert(!bad.valid()); // stale CRC rejected

  // Authentic old inner-v1 and inner-v2 records are accepted only after their
  // old CRC/reserved layout is checked. Both wrapper encodings migrate in RAM.
  auto v1=legacyInner(Settings::LegacyVersion);migration(dense(v1.data(),11),11);
  auto v2=legacyInner(Settings::LegacyV2Version);migration(dense(v2.data(),12),12);
  migration(sparse(v2.data(),13),13);
  migration(sparse(v1.data(),14),14);

  auto corrupt=v2;corrupt[Settings::TakRole]=0;corrupt[318]^=1;
  Backend badCrc;badCrc.records["mtextra/state0"]=dense(corrupt.data());Journal rejectCrc;
  assert(rejectCrc.load(badCrc)==-EINVAL&&badCrc.writes.empty());
  corrupt=v2;corrupt[Settings::TakTeam]=1;sealInner(corrupt.data());
  Backend reserved;reserved.records["mtextra/state0"]=dense(corrupt.data());Journal rejectReserved;
  assert(rejectReserved.load(reserved)==-EINVAL&&reserved.writes.empty());

  Backend store;Journal journal;assert(journal.load(store)==0&&journal.state()==Journal::State::Fresh);
  assert(journal.save(store,s)==0&&store.writes.size()==1&&store.writes[0]=="mtextra/state0");
  const auto saved=store.records["mtextra/state0"];Journal reload;assert(reload.load(store)==0&&reload.config().same(s));
  const auto reads=store.reads.size();const auto writes=store.writes.size();assert(reload.save(store,s)==0&&store.writes.size()==writes);
  assert(store.reads.size()==reads&&store.records["mtextra/state0"]==saved);
  for(const auto &key:store.reads)assert(key=="mtextra/state0"||key=="mtextra/state1");
  for(const auto &key:store.writes)assert(key=="mtextra/state0"||key=="mtextra/state1");
  puts("PASS M139 TAK settings: enum admission/roundtrip, legacy inner-version migration through dense and sparse wrappers, CRC/reserved rejection, and load/no-op-write behavior");
}
