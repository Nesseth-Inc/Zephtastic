// SPDX-License-Identifier: GPL-3.0-only
// Frozen M102 helper-contract fixtures. Synthetic PINs only; no BLE or device I/O.
#include "BluetoothSettings.h"
#include "BluetoothJournal.h"
#include "BluetoothPairingPolicy.h"
#include "BlePairingEvent.h"
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#ifdef NDEBUG
#error Bluetooth regression assertions must remain enabled
#endif
using Settings=mt::BluetoothSettings;
using Journal=mt::BluetoothJournal;
using Bytes=std::vector<uint8_t>;
using Event=mt::ble::BlePairingEvent;
using Plan=mt::ble::BluetoothPairingPlan;
static unsigned groups=0;
static void pass(const char *name){++groups;std::printf("PASS %s\n",name);}
static Settings settings(bool enabled,uint8_t mode,uint32_t pin){
  Settings value;value.enabled=enabled;value.mode=mode;value.fixedPin=pin;return value;
}
static bool same(const Settings&a,const Settings&b){return a.enabled==b.enabled&&a.mode==b.mode&&a.fixedPin==b.fixedPin;}
static bool same(const Plan&a,const Plan&b){
  return a.advertise==b.advertise&&a.bondable==b.bondable&&a.displayPasskey==b.displayPasskey&&a.security==b.security&&a.passkeySource==b.passkeySource;
}
static uint32_t get32(const uint8_t*p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static void put32(uint8_t*p,uint32_t n){for(unsigned i=0;i<4;++i){p[i]=uint8_t(n>>(8*i));}}
static uint32_t crc32(const uint8_t*p,size_t n){
  uint32_t c=UINT32_MAX;
  for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j){c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}}
  return ~c;
}
static void generation(Bytes&raw,uint32_t value){assert(raw.size()==32);put32(raw.data()+4,value);put32(raw.data()+28,crc32(raw.data(),28));}
struct Memory final:mt::SettingsBackend {
  enum class Fault {None,BeforeWrite,AfterWrite,Corrupt,ReadbackError,ReadbackShort,Torn};
  std::map<std::string,Bytes> files;
  unsigned writes=0;int readError=0;Fault fault=Fault::None;size_t cut=0;
  bool nextReadError=false,nextReadShort=false;
  Memory(){files["mtbase/state0"]={0xc0,0x52,0x45};files["mtmod/state0"]={0x4d,0x4f,0x44};}
  int read(const char*key,uint8_t*out,size_t capacity)override{
    if(readError){return readError;}
    if(nextReadError){nextReadError=false;return -EIO;}
    const auto found=files.find(key);if(found==files.end()){return -ENOENT;}
    if(found->second.size()>capacity){return -E2BIG;}
    size_t n=found->second.size();if(nextReadShort){nextReadShort=false;if(n){--n;}}
    if(n){std::memcpy(out,found->second.data(),n);}
    return int(n);
  }
  int write(const char*key,const uint8_t*data,size_t n)override{
    assert((!std::strcmp(key,"mtble/state0")||!std::strcmp(key,"mtble/state1"))&&n==32);
    ++writes;if(fault==Fault::BeforeWrite){return -ENOSPC;}
    if(fault==Fault::Torn){assert(cut<n);files[key]=Bytes(data,data+cut);return -EIO;}
    files[key]=Bytes(data,data+n);
    if(fault==Fault::Corrupt){files[key][12]^=1;}
    if(fault==Fault::ReadbackError){nextReadError=true;}
    if(fault==Fault::ReadbackShort){nextReadShort=true;}
    return fault==Fault::AfterWrite?-EIO:0;
  }
  void coreUnchanged()const{
    const auto a=files.find("mtbase/state0"),b=files.find("mtmod/state0");
    assert(a!=files.end()&&a->second==Bytes({0xc0,0x52,0x45}));
    assert(b!=files.end()&&b->second==Bytes({0x4d,0x4f,0x44}));
  }
};
static const Settings A=settings(true,Settings::FixedPin,123456);
static const Settings B=settings(false,Settings::NoPin,654321);
static void seed(Memory&memory){Journal journal;assert(!journal.load(memory)&&!journal.save(memory,A)&&!journal.save(memory,B));}
static void codecCases(){
  static_assert(Settings::Size==12&&Journal::RecordSize==32,"Explicit wire geometry");
  static_assert(Settings::RandomPin==0&&Settings::FixedPin==1&&Settings::NoPin==2,"Pinned stock enum mapping");
  const auto defaults=Settings::defaults();assert(defaults.enabled&&defaults.mode==Settings::NoPin&&!defaults.fixedPin&&Settings::valid(defaults,false));
  for(bool enabled:{false,true}){for(uint8_t mode:{uint8_t(Settings::RandomPin),uint8_t(Settings::FixedPin),uint8_t(Settings::NoPin)}){
    for(uint32_t pin:{0u,1u,42u,999999u}){
      const auto value=settings(enabled,mode,pin);assert(Settings::valid(value)&&Settings::valid(value,true));
      assert(Settings::valid(value,false)==(mode!=Settings::RandomPin));
      std::array<uint8_t,14> raw;raw.fill(0xa5);size_t written=87;
      assert(Settings::encode(value,raw.data()+1,12,written)&&written==12&&raw.front()==0xa5&&raw.back()==0xa5);
      assert(raw[1]=='Z'&&raw[2]=='B'&&raw[3]==1&&raw[4]==uint8_t(enabled)&&raw[5]==mode);
      assert(!raw[6]&&!raw[7]&&!raw[8]&&get32(raw.data()+9)==pin);
      auto decoded=settings(!enabled,Settings::FixedPin,7);assert(Settings::decode(raw.data()+1,12,decoded)&&same(value,decoded)&&value.same(decoded));
      auto other=value;other.enabled=!other.enabled;assert(!value.same(other));other=value;other.fixedPin=pin==999999?0:pin+1;assert(!value.same(other));
    }
  }}
  pass("codec defaults and all-mode full PIN roundtrip use exact12 bytes without touching guard bytes");
  for(uint8_t mode:{uint8_t(3),uint8_t(255)}){const auto invalid=settings(true,mode,0);assert(!Settings::valid(invalid));std::array<uint8_t,16> out;out.fill(0x6d);const auto before=out;size_t n=99;assert(!Settings::encode(invalid,out.data(),out.size(),n)&&out==before&&n==99);}
  for(uint32_t pin:{1000000u,UINT32_MAX}){for(uint8_t mode:{uint8_t(0),uint8_t(1),uint8_t(2)}){
    auto invalid=settings(true,mode,pin);assert(!Settings::valid(invalid));std::array<uint8_t,16> out;out.fill(0x6d);const auto before=out;size_t n=99;
    assert(!Settings::encode(invalid,out.data(),out.size(),n)&&out==before&&n==99);
  }}
  for(size_t cap=0;cap<12;++cap){std::array<uint8_t,16> out;out.fill(0x5c);const auto before=out;size_t n=67;assert(!Settings::encode(A,out.data(),cap,n)&&out==before&&n==67);}
  size_t n=57;assert(!Settings::encode(A,nullptr,12,n)&&n==57);
  pass("invalid stored mode/PIN and null/short encode fail atomically with no output overwrite");
  std::array<uint8_t,13> good{};n=0;assert(Settings::encode(A,good.data(),12,n));
  auto reject=[&](const uint8_t*raw,size_t size){auto destination=B;assert(!Settings::decode(raw,size,destination)&&same(destination,B));};
  reject(nullptr,12);reject(nullptr,0);for(size_t size=0;size<12;++size){reject(good.data(),size);}reject(good.data(),13);
  for(unsigned offset:{0u,1u,2u,3u,4u,5u,6u,7u}){
    auto bad=good;if(offset<3){bad[offset]^=0x40;}else{bad[offset]=offset==3?2:offset==4?255:1;}reject(bad.data(),12);
  }
  auto bad=good;put32(bad.data()+8,1000000);reject(bad.data(),12);
  pass("wrong length/header/bool/mode/reserved and oversized numeric decode preserve prior destination");
}
static void journalCases(){
  {Memory memory;Journal j;assert(!j.ready()&&j.state()==Journal::State::Unloaded&&j.activeSlot()==-1&&!j.generation());
    assert(j.save(memory,A)<0&&!memory.writes);assert(!j.load(memory)&&j.ready()&&j.state()==Journal::State::Fresh&&same(j.config(),Settings::defaults()));
    assert(!j.save(memory,Settings::defaults())&&!memory.writes&&j.activeSlot()==-1&&!j.generation());memory.coreUnchanged();}
  pass("fresh absent Bluetooth keys preserve enabled NoPin defaults and default save is a no-op without core writes");
  {Memory memory;Journal j;assert(!j.load(memory)&&!j.save(memory,A)&&j.activeSlot()==0&&j.generation()==1&&memory.writes==1);
    const auto first=memory.files["mtble/state0"];assert(first.size()==32&&first[0]=='Z'&&first[1]=='T'&&first[2]=='B'&&first[3]==1&&get32(first.data()+4)==1);
    for(unsigned i:{8u,9u,10u,11u,24u,25u,26u,27u}){assert(!first[i]);}
    uint8_t value[12];size_t n=0;assert(Settings::encode(A,value,sizeof(value),n)&&!std::memcmp(first.data()+12,value,12)&&get32(first.data()+28)==crc32(first.data(),28));
    assert(!j.save(memory,A)&&memory.writes==1&&!j.save(memory,B)&&j.activeSlot()==1&&j.generation()==2&&memory.writes==2);
    assert(memory.files["mtble/state0"]==first);Journal reboot;assert(!reboot.load(memory)&&same(reboot.config(),B)&&reboot.generation()==2&&reboot.activeSlot()==1);memory.coreUnchanged();}
  pass("dual-slot verified32-byte record alternation CRC and reboot select current settings without rewriting old slot");
  {Memory base;seed(base);for(size_t byte=0;byte<32;++byte){for(unsigned bit=0;bit<8;++bit){Memory memory=base;memory.files["mtble/state1"][byte]^=uint8_t(1u<<bit);Journal reboot;
      assert(!reboot.load(memory)&&same(reboot.config(),A)&&reboot.state()==Journal::State::Recovered&&reboot.activeSlot()==0);memory.coreUnchanged();
    }}
    for(size_t size:{size_t(0),size_t(31),size_t(33)}){Memory memory;memory.files["mtble/state0"]=Bytes(size,0);Journal bad;assert(bad.load(memory)<0&&!bad.ready()&&bad.state()==Journal::State::Invalid&&!memory.writes);}
    for(unsigned offset:{3u,8u,9u,10u,11u,24u,25u,26u,27u}){Memory memory;auto raw=base.files["mtble/state0"];raw[offset]=offset==3?2:1;put32(raw.data()+28,crc32(raw.data(),28));memory.files["mtble/state0"]=raw;Journal bad;assert(bad.load(memory)<0&&!bad.ready()&&!memory.writes);}
    for(unsigned offset:{12u,15u,16u,17u,20u}){Memory memory;auto raw=base.files["mtble/state0"];
      if(offset==20){put32(raw.data()+20,1000000);}else{raw[offset]=offset==15?2:offset==16?255:1;}
      put32(raw.data()+28,crc32(raw.data(),28));memory.files["mtble/state0"]=raw;Journal bad;assert(bad.load(memory)<0&&!bad.ready()&&!memory.writes);
    }}
  pass("every record single-bit corruption recovers old slot while malformed-present and resealed reserved/future records never default");
  {Memory base;seed(base);struct Pair{uint32_t first,second;int slot;};
    for(const auto row:std::array<Pair,5>{{{UINT32_MAX-1,UINT32_MAX,1},{UINT32_MAX,0,1},{0,1,1},{1,0,0},{0,UINT32_MAX,0}}}){
      Memory memory=base;generation(memory.files["mtble/state0"],row.first);generation(memory.files["mtble/state1"],row.second);Journal reboot;
      assert(!reboot.load(memory)&&reboot.activeSlot()==row.slot&&same(reboot.config(),row.slot?B:A));
    }
    for(uint32_t distance:{0u,0x80000000u}){Memory memory=base;generation(memory.files["mtble/state0"],7);generation(memory.files["mtble/state1"],7+distance);Journal bad;assert(bad.load(memory)<0&&!bad.ready()&&bad.state()==Journal::State::Invalid);}
    Memory sameRecord=base;sameRecord.files["mtble/state1"]=sameRecord.files["mtble/state0"];Journal equal;assert(!equal.load(sameRecord)&&same(equal.config(),A));
    Memory wrap=base;generation(wrap.files["mtble/state0"],UINT32_MAX);generation(wrap.files["mtble/state1"],UINT32_MAX-1);Journal j;assert(!j.load(wrap)&&j.activeSlot()==0&&!j.save(wrap,B)&&j.generation()==0&&j.activeSlot()==1);}
  pass("generation wrap ordering identical ties conflicting ties and half-range ambiguity preserve reliable ordering only");
  {for(size_t cut=0;cut<32;++cut){Memory memory;Journal j;assert(!j.load(memory)&&!j.save(memory,A));memory.fault=Memory::Fault::Torn;memory.cut=cut;
      assert(j.save(memory,B)<0&&!j.ready()&&j.state()==Journal::State::IoError&&same(j.config(),A)&&j.generation()==1&&j.activeSlot()==0);
      const auto writes=memory.writes;memory.fault=Memory::Fault::None;assert(j.save(memory,A)<0&&memory.writes==writes);
      Journal reboot;assert(!reboot.load(memory)&&same(reboot.config(),A)&&reboot.state()==Journal::State::Recovered&&!reboot.save(memory,B));memory.coreUnchanged();
    }}
  pass("all32 partial-write cuts poison saves until reload and preserve the prior committed settings");
  {for(auto fault:{Memory::Fault::BeforeWrite,Memory::Fault::AfterWrite,Memory::Fault::Corrupt,Memory::Fault::ReadbackError,Memory::Fault::ReadbackShort}){
      Memory memory;Journal j;assert(!j.load(memory)&&!j.save(memory,A));memory.fault=fault;
      assert(j.save(memory,B)<0&&!j.ready()&&j.state()==Journal::State::IoError&&same(j.config(),A)&&j.generation()==1&&j.activeSlot()==0);
      const auto writes=memory.writes;memory.fault=Memory::Fault::None;assert(j.save(memory,A)<0&&j.save(memory,B)<0&&memory.writes==writes);
      Journal reboot;assert(!reboot.load(memory));const bool committed=fault==Memory::Fault::AfterWrite||fault==Memory::Fault::ReadbackError||fault==Memory::Fault::ReadbackShort;
      assert(same(reboot.config(),committed?B:A));memory.coreUnchanged();
    }}
  pass("write-before/after-commit and corrupt/error/short readback retain old RAM and block stale durable acknowledgments");
  {Memory memory;Journal j;memory.readError=-EIO;assert(j.load(memory)==-EIO&&!j.ready()&&j.state()==Journal::State::IoError&&!memory.writes);memory.readError=0;assert(!j.load(memory));
    for(auto invalid:{settings(true,255,0),settings(true,Settings::FixedPin,1000000)}){const auto before=j.config();assert(j.save(memory,invalid)<0&&j.ready()&&same(j.config(),before)&&!memory.writes);}
    memory.coreUnchanged();}
  pass("load I/O error remains unavailable and invalid candidates cannot mutate a healthy fresh journal");
}
static void policyCases(){
  using Security=mt::ble::Security;using Source=mt::ble::PasskeySource;
  for(bool enabled:{false,true}){for(bool display:{false,true}){for(bool store:{false,true}){for(uint8_t mode:{uint8_t(0),uint8_t(1),uint8_t(2)}){
    const auto value=settings(enabled,mode,999999);Plan before;before.advertise=true;before.bondable=true;before.displayPasskey=true;before.security=Security::Authenticated;before.passkeySource=Source::Random;auto out=before;
    const bool expected=(mode!=Settings::RandomPin||display)&&(!enabled||mode==Settings::NoPin||store);
    assert(mt::ble::resolve(value,display,store,out)==expected);
    if(!expected){assert(same(out,before));continue;}
    if(!enabled){assert(!out.advertise&&!out.bondable&&!out.displayPasskey&&out.security==Security::None&&out.passkeySource==Source::None);}
    else if(mode==Settings::NoPin){assert(out.advertise&&!out.bondable&&!out.displayPasskey&&out.security==Security::Encrypted&&out.passkeySource==Source::None);}
    else{assert(out.advertise&&out.bondable&&out.security==Security::Authenticated&&out.displayPasskey==(mode==Settings::RandomPin)&&out.passkeySource==(mode==Settings::FixedPin?Source::Fixed:Source::Random));}
  }}}}
  pass("pairing full enabled/display/bond-store matrix preserves intent and never downgrades unavailable authenticated modes");
  for(auto invalid:{settings(true,255,0),settings(false,Settings::NoPin,1000000)}){Plan before;before.bondable=true;before.security=Security::Authenticated;auto out=before;assert(!mt::ble::resolve(invalid,true,true,out)&&same(before,out));}
  pass("invalid pairing settings leave caller plan unchanged even when disabled");
}
static void eventCases(){
  Event event;Event::Snapshot out;out.active=true;out.passkey=765432;out.session=99;
  assert(!event.snapshot(0,out)&&!out.active&&!out.passkey&&!out.session);assert(!event.publishPasskey(0,0)&&!event.clear(0));
  event.begin(7);assert(event.snapshot(7,out)&&!out.active&&!out.passkey&&out.session==7);
  assert(event.publishPasskey(7,42)&&event.snapshot(7,out)&&out.active&&out.passkey==42&&out.session==7);
  assert(!event.publishPasskey(6,999999)&&!event.clear(6));assert(event.snapshot(7,out)&&out.active&&out.passkey==42);
  event.begin(8);assert(!event.publishPasskey(7,123456)&&!event.clear(7));assert(event.snapshot(8,out)&&!out.active&&!out.passkey&&out.session==8);
  out.active=true;out.passkey=765432;out.session=7;assert(!event.snapshot(7,out)&&!out.active&&!out.passkey&&!out.session);
  pass("session replacement rejects stale callbacks and stale snapshot clears every field instead of leaking old PIN");
  for(uint32_t pin:{0u,999999u}){assert(event.publishPasskey(8,pin)&&event.snapshot(8,out)&&out.active&&out.passkey==pin);}
  for(uint32_t pin:{1000000u,UINT32_MAX}){assert(!event.publishPasskey(8,pin)&&event.snapshot(8,out)&&out.active&&out.passkey==999999);}
  assert(event.clear(8)&&event.clear(8)&&event.snapshot(8,out)&&!out.active&&!out.passkey&&out.session==8);
  event.begin(UINT32_MAX);assert(event.publishPasskey(UINT32_MAX,0));event.begin(1);assert(!event.publishPasskey(UINT32_MAX,42)&&event.snapshot(1,out)&&!out.active&&!out.passkey);
  event.begin(0);out.active=true;out.passkey=765432;out.session=1;assert(!event.snapshot(1,out)&&!out.active&&!out.passkey&&!out.session&&!event.clear(1));
  pass("PIN endpoints invalid-value atomicity idempotent clear zero reset and token replacement leave no stale PIN output");
}
int main(){codecCases();journalCases();policyCases();eventCases();assert(groups==14);std::printf("BLUETOOTH_TEST_METADATA groups=%u record_size=%zu codec_size=%zu corruption_bits=%zu torn_cut_points=%zu\n",groups,Journal::RecordSize,Settings::Size,Journal::RecordSize*8,Journal::RecordSize);}
