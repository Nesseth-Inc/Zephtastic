// SPDX-License-Identifier: GPL-3.0-only
// GNSS configuration and journal migration tests. The legacy-producer build uses
// literal prior Config/SettingsJournal headers supplied by the native runner.
#include "SettingsJournal.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <string>
#include <vector>

using Bytes=std::vector<uint8_t>;
struct Memory final:mt::SettingsBackend {
  std::unordered_map<std::string,Bytes> files;
  unsigned writes=0;
  bool failWrite=false,corruptReadback=false;
  int read(const char *key,uint8_t *out,size_t capacity)override {
    const auto i=files.find(key);if(i==files.end())return -ENOENT;
    if(i->second.size()>capacity)return -E2BIG;
    memcpy(out,i->second.data(),i->second.size());return int(i->second.size());
  }
  int write(const char *key,const uint8_t *data,size_t length)override {
    ++writes;files[key]=Bytes(data,data+length);
    if(corruptReadback)files[key][30]^=1;
    return failWrite?-EIO:0;
  }
};
static mt::Config richConfig() {
  mt::Config c;c.bytes[4]=1;c.seal();
  auto n=c.node();assert(n.setName("M105 migration sentinel",false)&&n.setName("M105",true));
  uint8_t key[32];for(unsigned i=0;i<32;++i)key[i]=uint8_t(0x30+i);
  n.setKey(0,key,32);n.write32(bench::MtNodeRecord::Interval,604800);n.seal();c.node(n);
  assert(c.setPrimaryName("PRI-M105")&&c.setDisplay(61,true,7));
  c.setDisplayLongNames(true);c.setTxEnabled(true);c.setChannelMuted(true);
  assert(c.setDeviceRole(2,0)&&c.setHopLimit(5));
  auto radio=c.radioRequest();radio.usePreset=false;radio.bandwidth=250;
  radio.spreadingFactor=10;radio.codingRate=5;radio.channel=0;
  radio.overrideMHz=915.5f;radio.offsetMHz=.25f;radio.powerSpecified=true;radio.power=24;
  assert(c.setRadio(radio)&&c.setFixedPosition(123456789,-987654321,true,-35));
  c.setPositionSharing(true);
#if defined(MT_GNSS_LEGACY_VERSION) && MT_GNSS_LEGACY_VERSION==14
  constexpr unsigned secondaryPrecision=0;
#else
  assert(c.setPrimaryPositionPrecision(31));constexpr unsigned secondaryPrecision=27;
#endif
  uint8_t secondaryKey[16];for(unsigned i=0;i<16;++i)secondaryKey[i]=uint8_t(0x80+i);
  mt::ChannelRecord secondary;assert(secondary.set(2,"SECOND",secondaryKey,16,secondaryPrecision,true));
  assert(c.setSecondaryChannel(5,secondary));
  mt::ChannelRecord inherited;assert(inherited.set(2,"INHERIT",nullptr,0,0,false));
  assert(c.setSecondaryChannel(6,inherited)&&c.valid());return c;
}
#ifdef MT_GNSS_LEGACY_PRODUCER
static void write(const std::string &path,const Bytes &bytes) {
  std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());out.close();assert(out);
}
int main(int argc,char **argv) {
  assert(argc==2);const auto c=richConfig();assert(sizeof(c)==608&&c.bytes[2]==MT_GNSS_LEGACY_VERSION);
  const std::string stem=std::string(argv[1])+"/v"+std::to_string(MT_GNSS_LEGACY_VERSION);
  write(stem+"-config.bin",Bytes(c.bytes,c.bytes+sizeof(c.bytes)));
  Memory memory;mt::SettingsJournal journal;assert(!journal.load(memory)&&!journal.save(memory,c));
  const auto &raw=memory.files.at("mtbase/state0");assert(raw.size()==624&&raw[3]==5);
  write(stem+"-journal.bin",raw);return 0;
}
#else
static bool sameSettings(const mt::GnssSettings &a,const mt::GnssSettings &b) {
  return a.enabled==b.enabled&&a.smart==b.smart&&a.positionFlags==b.positionFlags&&
    a.updateSecs==b.updateSecs&&a.attemptSecs==b.attemptSecs&&a.broadcastSecs==b.broadcastSecs&&
    a.smartMinDistance==b.smartMinDistance&&a.smartMinInterval==b.smartMinInterval;
}
static mt::GnssSettings settings() {
  mt::GnssSettings s;s.enabled=true;s.smart=true;s.positionFlags=0x3ff;
  s.updateSecs=0x12345678;s.attemptSecs=UINT32_MAX;s.broadcastSecs=0x10203040;
  s.smartMinDistance=UINT32_MAX;s.smartMinInterval=0x87654321;return s;
}
static mt::GnssSettings runtimeSettings() {
  auto s=settings();s.smart=false;s.positionFlags=33;
  s.smartMinDistance=s.smartMinInterval=0;return s;
}
static Bytes read(const std::string &path) {
  std::ifstream in(path,std::ios::binary);assert(in);return Bytes(std::istreambuf_iterator<char>(in),{});
}
static void seal(Bytes &raw) {
  const auto crc=mt::Config::checksum(raw.data(),raw.size()-2);
  raw[raw.size()-2]=uint8_t(crc);raw[raw.size()-1]=uint8_t(crc>>8);
}
static void rejectConfig(const Bytes &raw,const mt::Config &sentinel) {
  auto out=sentinel;assert(!mt::Config::decode(raw.data(),raw.size(),out)&&out.same(sentinel));
}
static void codecCases() {
  static_assert(mt::GnssSettings::Size==32);
  uint8_t raw[32];memset(raw,0xa5,sizeof(raw));mt::GnssSettings zero;
  assert(zero.encode(raw,sizeof(raw)));for(auto b:raw)assert(!b);
  auto out=settings();assert(mt::GnssSettings::decode(raw,sizeof(raw),out)&&sameSettings(out,zero));
  auto rich=settings();assert(rich.valid()&&rich.encode(raw,sizeof(raw)));
  assert(raw[0]==3&&raw[1]==0&&raw[2]==0xff&&raw[3]==3&&raw[6]==0x78&&raw[9]==0x12);
  for(unsigned i=26;i<32;++i)assert(!raw[i]);
  assert(mt::GnssSettings::decode(raw,sizeof(raw),out)&&sameSettings(out,rich));
  auto maxima=rich;maxima.updateSecs=maxima.attemptSecs=maxima.broadcastSecs=maxima.smartMinDistance=maxima.smartMinInterval=UINT32_MAX;
  assert(maxima.encode(raw,sizeof(raw))&&mt::GnssSettings::decode(raw,sizeof(raw),out)&&sameSettings(out,maxima));
  assert(rich.encode(raw,sizeof(raw)));const auto sentinel=out;
  const auto badDecode=[&](const uint8_t *p,size_t length){out=sentinel;assert(!mt::GnssSettings::decode(p,length,out)&&sameSettings(out,sentinel));};
  badDecode(nullptr,32);for(size_t n:{size_t(0),size_t(31),size_t(33)})badDecode(raw,n);
  for(unsigned bit=2;bit<8;++bit){auto bad=Bytes(raw,raw+32);bad[0]|=uint8_t(1u<<bit);badDecode(bad.data(),32);}
  for(unsigned index:{1u,26u,27u,28u,29u,30u,31u}){auto bad=Bytes(raw,raw+32);bad[index]=1;badDecode(bad.data(),32);}
  for(uint32_t flags:{0x400u,0x80000000u,UINT32_MAX}) {
    auto bad=Bytes(raw,raw+32);mt::Config::put32(bad.data()+2,flags);badDecode(bad.data(),32);
    auto invalid=rich;invalid.positionFlags=flags;memset(raw,0xa5,32);
    assert(!invalid.valid()&&!invalid.encode(raw,32));for(auto b:raw)assert(b==0xa5);
  }
  for(size_t n:{size_t(0),size_t(31),size_t(33)}){memset(raw,0xa5,32);assert(!rich.encode(raw,n));for(auto b:raw)assert(b==0xa5);}
  assert(!rich.encode(nullptr,32));puts("PASS GNSS32 codec boundaries, reserved bits, field persistence and failure atomicity");
}
static void migrationCases(const std::string &directory) {
  static_assert(sizeof(mt::Config)==640);
  const auto sentinel=richConfig();
  for(unsigned version:{14u,15u}) {
    const auto raw=read(directory+"/v"+std::to_string(version)+"-config.bin");assert(raw.size()==608&&raw[2]==version);
    mt::Config migrated;assert(mt::Config::decode(raw.data(),raw.size(),migrated)&&migrated.valid()&&migrated.bytes[2]==mt::Config::Version);
    assert(!migrated.ownerUnmessagable());
    for(size_t i=3;i<606;++i)assert(migrated.bytes[i]==(version==14&&i==122?15:raw[i]));
    for(size_t i=606;i<638;++i)assert(!migrated.bytes[i]);
    assert(migrated.primaryPositionPrecision()==(version==15?31u:15u)&&migrated.positionSharing());
    mt::ChannelRecord secondary;assert(migrated.channelRecord(5,secondary));
    assert(!strcmp(secondary.name(),"SECOND")&&secondary.keySize()==16&&secondary.key()[0]==0x80&&secondary.muted());
    assert(secondary.precision()==(version==15?27u:0u));
    const auto legacy=raw;
    for(size_t i=0;i<raw.size();++i){auto bad=raw;bad[i]^=1;rejectConfig(bad,sentinel);}
    assert(raw==legacy); // Migration and all rejects leave original durable bytes untouched.
    auto bad=raw;bad[123]=1;seal(bad);rejectConfig(bad,sentinel);
    bad=raw;bad[122]=version==14?1:33;seal(bad);rejectConfig(bad,sentinel);
    bad=raw;bad[2]=16;seal(bad);rejectConfig(bad,sentinel);
  }
  puts("PASS actual v14/v15 608-byte producers preserve primary precision, every legacy body byte and secondary metadata; all608 single-byte CRC corruptions reject atomically");
}
static void currentCases() {
  mt::Config c;assert(c.valid()&&!c.gnssSettings().enabled&&!c.positionPotential());
  c=richConfig();const auto before=c;const auto rich=runtimeSettings();assert(c.setGnssSettings(rich)&&c.valid());
  assert(!memcmp(c.bytes+3,before.bytes+3,603)&&sameSettings(c.gnssSettings(),rich));
  mt::Config reboot;assert(mt::Config::decode(c.bytes,sizeof(c.bytes),reboot)&&reboot.same(c));
  auto invalid=rich;invalid.positionFlags=0x400;const auto saved=c;assert(!c.setGnssSettings(invalid)&&c.same(saved));
  const auto wire=Bytes(c.bytes,c.bytes+sizeof(c.bytes));
  for(size_t i=0;i<wire.size();++i){auto bad=wire;bad[i]^=1;rejectConfig(bad,saved);}
  for(unsigned offset:{606u,607u,632u,633u,634u,635u,636u,637u}) {
    auto bad=wire;bad[offset]|=offset==606?4:1;seal(bad);rejectConfig(bad,saved);
  }
  auto bad=wire;mt::Config::put32(bad.data()+608,0x400);seal(bad);rejectConfig(bad,saved);
  auto unchanged=saved;assert(!mt::Config::decode(nullptr,640,unchanged)&&unchanged.same(saved));
  for(size_t n:{size_t(0),size_t(607),size_t(609),size_t(639),size_t(641)}){auto resized=wire;resized.resize(n);rejectConfig(resized,saved);}
  puts("PASS current Config GNSS roundtrip, outer/inner codec rejection and unchanged destination");
  // These are raw persisted uint32 values, not resolved runtime thresholds.
  // Zero/default and clamp decisions belong to the scheduling caller.
  for(bool smart:{false,true})for(bool enabled:{false,true})for(uint32_t distance:{0u,1u,UINT32_MAX})for(uint32_t interval:{0u,1u,UINT32_MAX}) {
    auto value=rich;value.smart=smart;value.enabled=enabled;value.smartMinDistance=distance;value.smartMinInterval=interval;
    auto candidate=saved;assert(mt::Config::supportedGnssSettings(value)&&candidate.setGnssSettings(value)&&candidate.valid());
    assert(!memcmp(candidate.bytes+3,saved.bytes+3,603)&&sameSettings(candidate.gnssSettings(),value));
    mt::Config decoded;assert(mt::Config::decode(candidate.bytes,sizeof(candidate.bytes),decoded)&&decoded.same(candidate));
    const auto beforeNoop=candidate;assert(candidate.setGnssSettings(value)&&candidate.same(beforeNoop));
  }
  std::vector<mt::GnssSettings> unsupported;
  auto unsupportedValue=rich;unsupportedValue.smart=true;unsupportedValue.smartMinDistance=UINT32_MAX;unsupportedValue.smartMinInterval=UINT32_MAX;
  for(uint32_t flag:{64u}) {
    auto rejected=unsupportedValue;rejected.positionFlags|=flag;unsupported.push_back(rejected);
  }
  for(auto value:unsupported)for(bool enabled:{false,true}) {
    value.enabled=enabled;assert(value.valid()&&!mt::Config::supportedGnssSettings(value));
    auto unchangedConfig=saved;assert(!unchangedConfig.setGnssSettings(value)&&unchangedConfig.same(saved));
    auto stored=wire;assert(value.encode(stored.data()+606,mt::GnssSettings::Size));
    mt::GnssSettings represented;assert(mt::GnssSettings::decode(stored.data()+606,mt::GnssSettings::Size,represented)&&sameSettings(value,represented));
    seal(stored);mt::Config direct;memcpy(direct.bytes,stored.data(),stored.size());
    assert(!direct.valid());rejectConfig(stored,saved);
  }
  for(uint32_t flags=0;flags<1024;++flags)if(!(flags&~959u)) {
    auto supported=rich;supported.positionFlags=flags;auto candidate=saved;
    assert(mt::Config::supportedGnssSettings(supported)&&candidate.setGnssSettings(supported)&&candidate.valid());
    mt::Config decoded;assert(mt::Config::decode(candidate.bytes,sizeof(candidate.bytes),decoded)&&decoded.same(candidate));
  }
  puts("PASS smart enable/zero/max uint32 thresholds persist exactly without other Config changes; unsupported flags still reject with valid CRC");
  // GNSS settings and fixed coordinates are the complete position-only family;
  // consent/channel settings and every other durable byte remain significant.
  for(size_t i=0;i<mt::Config::Size;++i) {
    auto changed=saved;changed.bytes[i]^=1;
    const bool positionFamily=(i>=84&&i<97)||(i>=606&&i<640);
    assert(saved.sameExceptPosition(changed)==positionFamily);
  }
  auto changed=saved;auto next=rich;next.smart=false;next.updateSecs=99;
  assert(changed.setGnssSettings(next)&&saved.sameExceptPosition(changed)&&!saved.sameExceptChannelMute(changed));
  changed=saved;assert(changed.setPrimaryPositionPrecision(30)&&!saved.sameExceptPosition(changed));
  changed=saved;changed.setTxEnabled(false);assert(!saved.sameExceptPosition(changed));
  auto disabled=rich;disabled.enabled=false;changed=saved;
  assert(changed.setGnssSettings(disabled)&&changed.primaryPositionPrecision()==31); // Fixed coordinates still exist.
  mt::Config live;assert(live.setGnssSettings(rich)&&live.positionPotential()&&!live.fixedPosition());
  assert(live.setPrimaryPositionPrecision(31));mt::ChannelRecord ch;
  uint8_t key[16]{};key[0]=0x80;assert(ch.set(2,"GNSS",key,16,27,true)&&live.setSecondaryChannel(3,ch));
  assert(live.setGnssSettings(disabled)&&!live.positionSharing()&&!live.primaryPositionPrecision());
  assert(live.channelRecord(3,ch)&&!ch.precision()&&ch.keySize()==16&&ch.key()[0]==0x80&&ch.muted()&&!strcmp(ch.name(),"GNSS"));
  puts("PASS exact position-only byte scope and source-disable consent revocation preserve secondary keys/name/mute");
}
static void journalCases(const std::string &directory) {
  static_assert(mt::SettingsJournal::RecordSize==656&&mt::SettingsJournal::RadioRecordSize==624);
  for(unsigned version:{14u,15u}) {
    Memory memory;const auto old=read(directory+"/v"+std::to_string(version)+"-journal.bin");
    memory.files["mtbase/state0"]=old;mt::SettingsJournal journal;
    assert(!journal.load(memory)&&journal.ready()&&journal.generation()==1&&journal.activeSlot()==0&&!memory.writes);
    const auto migrated=journal.config();auto next=migrated;auto smart=runtimeSettings();smart.smart=true;smart.smartMinDistance=smart.smartMinInterval=UINT32_MAX;assert(next.setGnssSettings(smart));
    assert(!journal.save(memory,next)&&journal.config().same(next)&&journal.generation()==2&&journal.activeSlot()==1);
    assert(memory.files.at("mtbase/state0")==old);
    const auto &raw=memory.files.at("mtbase/state1");assert(raw.size()==656&&raw[3]==7&&mt::Config::get32(raw.data()+8)==journal.channelGeneration());
    assert(!memcmp(raw.data()+12,next.bytes,640));mt::SettingsJournal reboot;
    assert(!reboot.load(memory)&&reboot.config().same(next)&&reboot.generation()==2);
    const auto count=memory.writes;assert(!reboot.save(memory,next)&&memory.writes==count);
    memory.files["mtbase/state1"][650]^=1;mt::SettingsJournal recovered;
    assert(!recovered.load(memory)&&recovered.state()==mt::SettingsJournal::State::Recovered&&recovered.config().same(migrated)&&recovered.generation()==1);
    Memory uncertain;uncertain.files["mtbase/state0"]=old;mt::SettingsJournal failed;assert(!failed.load(uncertain));
    uncertain.failWrite=true;assert(failed.save(uncertain,next)==-EIO&&failed.config().same(migrated)&&failed.state()==mt::SettingsJournal::State::IoError);
    assert(failed.save(uncertain,migrated)==-EACCES);mt::SettingsJournal committed;
    assert(!committed.load(uncertain)&&committed.config().same(next)); // Error may follow durable commit.
    Memory corrupt;corrupt.files["mtbase/state0"]=old;mt::SettingsJournal readback;assert(!readback.load(corrupt));corrupt.corruptReadback=true;
    assert(readback.save(corrupt,next)==-EIO&&readback.config().same(migrated)&&readback.state()==mt::SettingsJournal::State::IoError);
  }
  puts("PASS actual Journal5 read-only migration, Journal7 save/remount, rollback bytes, corruption fallback and uncertain-write latch");
}
int main(int argc,char **argv) {
  assert(argc==2);codecCases();migrationCases(argv[1]);currentCases();journalCases(argv[1]);return 0;
}
#endif
