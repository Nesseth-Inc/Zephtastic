// SPDX-License-Identifier: GPL-3.0-only
// Actual Config and pure helpers; original historical wire lengths, no storage/RF.
#include "Config.h"
#include "modules/RouterLateRoleDefaults.h"
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
using Bytes=std::vector<uint8_t>;
static void group(const char *s){std::cout<<"PASS "<<s<<'\n';}
static void seal(Bytes &b){const auto crc=mt::Config::checksum(b.data(),b.size()-2);b[b.size()-2]=uint8_t(crc);b.back()=uint8_t(crc>>8);}
static uint8_t hash(const char *name,const uint8_t *key,size_t size){uint8_t h=0;for(;*name;++name)h^=uint8_t(*name);for(size_t i=0;i<size;++i)h^=key[i];return h;}
static Bytes former(unsigned version,unsigned preset=0){
  const mt::Config current;const size_t size=version==1?80:version<4?96:112;Bytes b(size);
  memcpy(b.data(),current.bytes,78);b[2]=uint8_t(version);b[4]=1;
  if(version>1){b[78]=17;b[82]=1;if(version>=3)b[83]=2;}
  if(version>=4){b[84]=3;mt::Config::putSigned32(b.data()+85,123);mt::Config::putSigned32(b.data()+89,-456);mt::Config::putSigned32(b.data()+93,789);}
  if(version>=5)mt::Config::put32(b.data()+97,42);
  if(version>=6)b[101]=1;
  if(version>=7)b[102]=1;
  if(version>=9)b[103]=1;
  if(version==10){b[104]=uint8_t(preset);b[105]=1;}
  seal(b);return b;
}
static void refused(const Bytes &b,const std::string &label){
  mt::Config out;assert(out.setDisplay(123,true));const auto before=out;
  const bool accepted=mt::Config::decode(b.data(),b.size(),out);
  if(accepted||!out.same(before)){std::fprintf(stderr,"REFUSAL_CASE_FAILED: %s (accepted=%d, output_unchanged=%d, bytes=%zu)\n",label.c_str(),accepted,out.same(before),b.size());std::abort();}
}
static void custom(mt::Config &c,uint8_t first,size_t size=16){
  uint8_t key[32]{};key[0]=first;mt::PrimaryChannel::Selection s;
  assert(mt::PrimaryChannel::normalize(key,size,s,c.effectivePrimaryName()));auto record=c.node();record.setKey(s.hash,s.bytes,s.length);mt::PrimaryChannel::clear(s);c.node(record);
}
int main(){
  mt::Config fresh;assert(sizeof(fresh)==640&&fresh.Size==640&&fresh.RadioSize==608&&fresh.BankSize==576&&fresh.PrimarySize==128&&fresh.FormerSize==112&&fresh.PreviousSize==96&&fresh.LegacySize==80);
  assert(fresh.valid()&&fresh.bytes[2]==mt::Config::Version&&fresh.role()==1&&fresh.rebroadcastMode()==0&&!fresh.primaryName()[0]&&!strcmp(fresh.effectivePrimaryName(),"LongFast")&&fresh.channelConfigured()&&!fresh.provisioned());
  for(unsigned i=109;i<126;++i)if(i!=121)assert(!fresh.bytes[i]);
  assert(fresh.hopLimit()==3);
  group("v19 exact640 layout retains the608-byte radio record and defaults to mute/all with canonical empty LongFast primary and no position consent");
  for(unsigned version=1;version<=10;++version){
    auto old=former(version,4);mt::Config out;assert(mt::Config::decode(old.data(),old.size(),out)&&out.valid()&&out.provisioned());
    assert(out.bytes[2]==mt::Config::Version&&out.role()==1&&out.rebroadcastMode()==0&&!memcmp(out.bytes,old.data(),2)&&!memcmp(out.bytes+3,old.data()+3,75));
    assert(!memcmp(out.node().bytes,old.data()+6,72));
    if(version>1)assert(out.displaySeconds()==17&&out.displayFlip());else assert(out.displaySeconds()==15&&!out.displayFlip());
    assert(out.txIntentSpecified()==(version>=3)&&out.fixedPosition()==(version>=4)&&out.channelMuted()==(version>=6)&&out.positionSharing()==(version>=7)&&out.displayLongNames()==(version>=9));
    assert(out.modemPreset()==(version==10?4u:0u)&&out.channelNumber()==(version==10?1u:20u));
    assert(!strcmp(out.primaryName(),version==10?"LongFast":"")&&!strcmp(out.effectivePrimaryName(),"LongFast"));
    mt::Config again;assert(mt::Config::decode(out.bytes,out.Size,again)&&again.same(out));
    auto bad=old;bad.back()^=1;refused(bad,"historical-v"+std::to_string(version)+"-crc-torn");
  }
  group("all ten historical versions validate original wire CRC before migration preserving authorization and key/owner bytes");
  for(unsigned version=4;version<=10;++version){
    auto old=former(version);mt::Config holder;memcpy(holder.bytes,old.data(),old.size());assert(holder.validBody(uint8_t(version)));
    for(unsigned i=107;i<110;++i){auto bad=old;bad[i]=1;seal(bad);refused(bad,"historical-v"+std::to_string(version)+"-reserved-"+std::to_string(i));}
    if(version<10){auto bad=old;bad[104]=1;seal(bad);refused(bad,"historical-v"+std::to_string(version)+"-premature-lora-field");}
    Bytes widened(128);memcpy(widened.data(),old.data(),old.size());seal(widened);refused(widened,"historical-v"+std::to_string(version)+"-unsupported-width-128");
  }
  for(const auto &pair:std::vector<std::pair<unsigned,unsigned>>{{4,97},{5,101},{6,102},{7,103},{8,103}}){auto bad=former(pair.first);bad[pair.second]=1;seal(bad);refused(bad,"historical-v"+std::to_string(pair.first)+"-reserved-"+std::to_string(pair.second));}
  for(unsigned version:{2u,3u}){auto bad=former(version);bad[84]=1;seal(bad);refused(bad,"historical-v"+std::to_string(version)+"-reserved-fixed-altitude");}
  auto v2=former(2);v2[83]=1;seal(v2);refused(v2,"historical-v2-invalid-tx-intent");
  group("old112 reserved boundaries and old80/96 authorization rules cannot be bypassed by new128 CRC or layout");
  for(unsigned version:{7u,8u}){
    auto old=former(version);bench::MtNodeRecord n;memcpy(n.bytes,old.data()+6,72);n.write32(bench::MtNodeRecord::Interval,100000);n.seal();memcpy(old.data()+6,n.bytes,72);seal(old);
    if(version==7)refused(old,"historical-v7-interval-out-of-range");else {mt::Config out;assert(mt::Config::decode(old.data(),old.size(),out)&&out.node().interval()==100000);}
  }
  group("pre-v8 interval ceiling remains enforced while newer wider intervals migrate unchanged");
  Bytes current(fresh.bytes,fresh.bytes+fresh.Size);
  for(size_t n=0;n<current.size();++n)refused(Bytes(current.begin(),current.begin()+n),"current-truncated-length-"+std::to_string(n));
  auto oversized=current;oversized.push_back(0);refused(oversized,"current-oversized-width");
  for(size_t i=0;i<current.size();++i){auto bad=current;bad[i]^=1;refused(bad,"current-single-byte-torn-"+std::to_string(i));}
  mt::Config sentinel;const auto untouched=sentinel;assert(!mt::Config::decode(nullptr,128,sentinel)&&sentinel.same(untouched));
  assert(mt::Config::decode(sentinel.bytes,sentinel.Size,sentinel)&&sentinel.same(untouched));
  group("all truncated and single-byte-torn current inputs refuse with output unchanged; alias decode is safe");
  for(unsigned role:{0u,1u,2u,3u,11u,12u,255u,UINT_MAX})for(unsigned mode:{0u,1u,2u,3u,4u,5u,6u,255u,UINT_MAX}){
    auto c=fresh;const auto before=c;const bool valid=mt::DeviceRolePolicy::valid(role,mode);assert(c.setDeviceRole(role,mode)==valid);
    if(valid){assert(c.valid()&&c.role()==role&&c.rebroadcastMode()==mode);mt::Config out;assert(mt::Config::decode(c.bytes,c.Size,out)&&out.same(c));}
    else assert(c.same(before));
  }
  for(auto change:std::vector<std::pair<unsigned,uint8_t>>{{107,3},{108,1},{107,13},{107,255},{108,255}}){auto bad=current;bad[change.first]=change.second;seal(bad);refused(bad,"current-unsupported-role-or-rebroadcast-"+std::to_string(change.first)+"-"+std::to_string(change.second));}
  {auto bad=current;bad[107]=2;bad[108]=4;seal(bad);refused(bad,"current-router-none-pair");}
  group("role/mode setters and persisted raw values use the same bounded valid-pair policy atomically");
  {auto base=fresh;const auto before=base;const auto beforeNode=before.node();assert(base.setDeviceRole(12,0)&&base.valid());
   assert(mt::DeviceRolePolicy::supportedRole(12)&&mt::DeviceRolePolicy::storedValid(12,0)&&
          mt::DeviceRolePolicy::defaultRebroadcast(12)==0&&mt::DeviceRolePolicy::relays(12,0)&&
          !mt::DeviceRolePolicy::relays(12,4));
   assert(base.role()==12&&base.rebroadcastMode()==0&&base.bytes[122]==before.bytes[122]&&
          base.bytes[123]==before.bytes[123]&&base.bytes[101]==before.bytes[101]&&
          !memcmp(base.node().bytes,beforeNode.bytes,sizeof(beforeNode.bytes)));
   mt::Config decoded;assert(mt::Config::decode(base.bytes,base.Size,decoded)&&decoded.same(base));
   auto invalid=base;assert(!invalid.setDeviceRole(13,0)&&invalid.same(base));}
  group("CLIENT_BASE persists as role 12 without changing position-radio-channel or rebroadcast policy");
  group("deprecated ROUTER_CLIENT and REPEATER remain setter-only aliases, not supported stored roles");
  assert(!mt::DeviceRolePolicy::valid(3,0)&&!mt::DeviceRolePolicy::valid(4,0)&&
         !mt::DeviceRolePolicy::storedValid(3,0)&&!mt::DeviceRolePolicy::storedValid(4,0));
  {auto late=fresh;assert(mt::DeviceRolePolicy::supportedRole(11)&&mt::DeviceRolePolicy::storedValid(11,0)&&
     mt::DeviceRolePolicy::defaultRebroadcast(11)==0&&mt::DeviceRolePolicy::relays(11,0)&&
     !mt::DeviceRolePolicy::valid(11,4)&&!mt::DeviceRolePolicy::storedValid(11,4));
   assert(late.setDeviceRole(11,0)&&late.valid());mt::Config decoded;assert(mt::Config::decode(late.bytes,late.Size,decoded)&&decoded.same(late));
   const auto before=late;assert(!late.setDeviceRole(11,4)&&late.same(before));
   assert(late.setDeviceRole(11,2)&&late.rebroadcastMode()==2&&late.valid());}
  group("ROUTER_LATE role 11 persists with stock ALL default and rejects NONE while retaining explicit supported modes");
  {
    mt::Config source=fresh;assert(source.setDeviceRole(0,2));
    const auto originalNode=source.node();
    mt::ModuleSettings modules;meshtastic_ModuleConfig telemetry{};
    assert(modules.get(telemetry,meshtastic_ModuleConfig_telemetry_tag));
    telemetry.payload_variant.telemetry.device_update_interval=30;
    assert(modules.set(telemetry));
    mt::modules::RouterLateRoleDefaults::Projection projected{};
    assert(mt::modules::RouterLateRoleDefaults::project(0,11,source,modules,projected,2)==
           mt::modules::RouterLateRoleDefaults::Result::Applied);
    assert(projected.core.role()==11&&projected.core.rebroadcastMode()==2&&projected.core.ownerUnmessagable());
    assert(!memcmp(projected.core.node().bytes,originalNode.bytes,sizeof(originalNode.bytes))&&
           projected.core.modemPreset()==source.modemPreset()&&projected.core.channelNumber()==source.channelNumber()&&
           projected.core.regionCode()==source.regionCode()&&projected.core.primaryPositionPrecision()==source.primaryPositionPrecision()&&
           !memcmp(projected.core.bytes+84,source.bytes+84,13)&&!memcmp(projected.core.bytes+606,source.bytes+606,32));
    meshtastic_ModuleConfig changed{};
    assert(projected.modules.get(changed,meshtastic_ModuleConfig_telemetry_tag));
    assert(changed.payload_variant.telemetry.device_update_interval==86400&&
           changed.payload_variant.telemetry.environment_measurement_enabled==telemetry.payload_variant.telemetry.environment_measurement_enabled&&
           changed.payload_variant.telemetry.power_measurement_enabled==telemetry.payload_variant.telemetry.power_measurement_enabled);
    mt::modules::RouterLateRoleDefaults::Projection same{};
    assert(mt::modules::RouterLateRoleDefaults::project(11,11,projected.core,projected.modules,same)==
           mt::modules::RouterLateRoleDefaults::Result::SameRole&&same.core.same(projected.core)&&same.modules.same(projected.modules));
  }
  group("ROUTER_LATE transition applies owner and daily device telemetry defaults while preserving explicit mode and other core settings");
  for(const char *name:{"","A","A B","ABCDEFGHIJK","LongFast"}){
    auto c=fresh;assert(c.setPrimaryName(name)&&c.valid()&&c.channelConfigured()&&!strcmp(c.primaryName(),name));
    auto n=c.node();assert(n.bytes[4]==hash(c.effectivePrimaryName(),n.bytes+bench::MtNodeRecord::Key,n.bytes[3]));
    const size_t len=strlen(name);for(size_t i=len;i<12;++i)assert(!c.bytes[109+i]);
    mt::Config out;assert(mt::Config::decode(c.bytes,c.Size,out)&&out.same(c));
  }
  for(const char *name:{"ABCDEFGHIJKL","bad\nname","bad\x7f","bad\x80"}){auto c=fresh;assert(!c.setPrimaryName(name)&&c.same(fresh));}
  {auto c=fresh;assert(!c.setPrimaryName(nullptr)&&c.same(fresh));assert(c.setPrimaryName("Trail"));assert(c.setPrimaryName(c.primaryName()+2)&&!strcmp(c.primaryName(),"ail")&&c.channelConfigured());}
  for(unsigned precision:{0u,1u,15u,32u}){auto wire=current;wire[122]=uint8_t(precision);seal(wire);mt::Config out;
    assert(mt::Config::decode(wire.data(),wire.size(),out)&&out.valid()&&out.primaryPositionPrecision()==precision);
    assert(!out.positionSharing()&&!out.fixedPosition()&&!out.positionPrecision(0));}
  for(unsigned precision:{33u,255u}){auto bad=current;bad[122]=uint8_t(precision);seal(bad);refused(bad,"current-position-precision-"+std::to_string(precision));}
  for(unsigned i:{109u,120u,125u}){auto bad=current;bad[i]=1;seal(bad);refused(bad,"current-invalid-name-or-reserved-byte-"+std::to_string(i));}
  {auto bad=current;memset(bad.data()+109,'A',12);seal(bad);refused(bad,"current-name-missing-terminator");}
  {auto bad=current;bad[109]='A';bad[111]='B';seal(bad);refused(bad,"current-name-nonzero-after-terminator");}
  {auto consent=current;consent[123]=1;seal(consent);mt::Config out;assert(mt::Config::decode(consent.data(),consent.size(),out)&&out.primaryDownlinkEnabled()&&!out.ignoreMqtt());}
  {auto ignoreMqtt=current;ignoreMqtt[124]=1;seal(ignoreMqtt);mt::Config out;assert(mt::Config::decode(ignoreMqtt.data(),ignoreMqtt.size(),out)&&!out.primaryDownlinkEnabled()&&out.ignoreMqtt());}
  group("v17 primary downlink consent and MQTT ignore flags remain valid persisted booleans, not reserved bytes");
  group("primary names retain canonical padding; current precision accepts0..32 without inventing consent and rejects larger values");
  for(unsigned preset:{0u,1u,3u,4u,5u,6u,7u,8u,9u,16u}){
    auto c=fresh;assert(c.setLoRa(preset,0)&&c.valid()&&c.channelConfigured()&&!c.primaryName()[0]);
    const auto n=c.node();assert(!strcmp(c.effectivePrimaryName(),mt::PrimaryChannel::presetName(preset))&&n.bytes[4]==hash(c.effectivePrimaryName(),n.bytes+bench::MtNodeRecord::Key,n.bytes[3]));
    mt::LoRaPresets::Profile p{};mt::USChannelSelection s{};assert(mt::RadioSettings::resolve(preset,0,p,s,c.effectivePrimaryName()));
    uint32_t h=5381;for(const char *x=c.effectivePrimaryName();*x;++x)h=h*33u+uint8_t(*x);
    assert(s.slot==h%(26000000u/p.bandwidthHz));
    const auto before=c;assert(!c.setLoRa(2,0)&&c.same(before));auto clamped=c;assert(clamped.setLoRa(preset,UINT_MAX)&&clamped.channelNumber()==uint32_t(s.slot)+1);
  }
  {auto c=fresh;assert(c.setPrimaryName("LongFast"));const auto before=c.node();assert(c.setLoRa(4,0)&&c.channelConfigured()&&!strcmp(c.effectivePrimaryName(),"LongFast")&&!memcmp(c.node().bytes,before.bytes,72));assert(c.setPrimaryName("")&&!strcmp(c.effectivePrimaryName(),"MediumFast")&&c.channelConfigured());}
  group("empty name follows all ten preset names and hash/auto-slot; explicit name preserves hash across modulation");
  {auto c=fresh;custom(c,'A');const auto key=c.node();assert(c.setPrimaryName("A")&&c.channelConfigured()&&c.node().bytes[4]==0&&!memcmp(c.node().bytes+bench::MtNodeRecord::Key,key.bytes+bench::MtNodeRecord::Key,32));}
  {auto c=fresh;custom(c,hash("MediumFast",nullptr,0),32);const auto key=c.node();assert(c.setLoRa(4,0)&&c.channelConfigured()&&c.node().bytes[4]==0&&!memcmp(c.node().bytes+bench::MtNodeRecord::Key,key.bytes+bench::MtNodeRecord::Key,32));}
  {auto c=fresh;custom(c,'A',32);const auto before=c.node();assert(c.setPrimaryName("Custom")&&c.channelConfigured()&&!memcmp(c.node().bytes+bench::MtNodeRecord::Key,before.bytes+bench::MtNodeRecord::Key,32));}
  {auto c=fresh;auto n=c.node();n.setKey(0,nullptr,0);c.node(n);assert(c.setPrimaryName("Disabled")&&c.setLoRa(4,0)&&c.valid()&&!c.channelConfigured()&&!c.provisioned());}
  group("hash-zero configured-key name/preset transitions remain valid; raw keys survive renames and disabled channels stay disabled");
  {auto pos=fresh;assert(pos.setFixedPosition(12,-34,true,56)&&pos.sameExceptPosition(fresh)&&!pos.sameExceptChannelMute(fresh));
   auto mute=fresh;mute.setChannelMuted(true);assert(mute.sameExceptChannelMute(fresh)&&!mute.sameExceptPosition(fresh));
   for(unsigned i=107;i<126;++i){auto c=fresh;c.bytes[i]^=1;c.seal();assert(!c.sameExceptPosition(fresh)&&!c.sameExceptChannelMute(fresh));}
   auto role=fresh;assert(role.setDeviceRole(0,0)&&!role.sameExceptPosition(fresh)&&!role.sameExceptChannelMute(fresh));
   auto name=fresh;assert(name.setPrimaryName("Trail")&&!name.sameExceptPosition(fresh)&&!name.sameExceptChannelMute(fresh));
  }
  group("narrow position/mute comparisons include every new persisted field and exclude only their defined slice plus CRC");
  return 0;
}
