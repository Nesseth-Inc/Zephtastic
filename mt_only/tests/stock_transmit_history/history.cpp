// SPDX-License-Identifier: GPL-3.0-only
#include "stock/history/TransmitHistory.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using mt::history::Clock;using mt::history::Kind;using mt::history::TransmitHistory;
static Clock sample{};
static Clock clockNow(){return sample;}
namespace oracle {
struct Time {static uint32_t getMillis(){return uint32_t(sample.uptimeMs);}
             static uint32_t skipZero(uint32_t value){return value?value:1;}};
static unsigned getTime(){return sample.utc?sample.utc:unsigned(sample.uptimeMs/1000);}
constexpr unsigned RTCQualityNone=0;
static unsigned getRTCQuality(){return sample.utc?1:RTCQualityNone;}
struct TransmitHistory {
  static constexpr uint32_t BOOT_RELATIVE_RECOVERY_WINDOW_SEC=120;
  uint32_t getLastSentAbsoluteMillis(uint32_t)const;
  uint32_t getLastSentBootRelativeMillis(uint32_t)const;
};
#include "getLastSentAbsoluteMillis.inc"
#include "getLastSentBootRelativeMillis.inc"
}
using Bytes=std::vector<uint8_t>;
struct Store : mt::SettingsBackend {
  std::map<std::string,Bytes> rows;unsigned writes=0;
  int cut=-1;bool ambiguous=false,readFail=false;
  int read(const char *k,uint8_t *p,size_t n)override{
    if(readFail)return -EIO;
    const auto it=rows.find(k);if(it==rows.end()||it->second.empty())return -ENOENT;
    if(it->second.size()>n)return -E2BIG;
    memcpy(p,it->second.data(),it->second.size());return int(it->second.size());
  }
  int write(const char *k,const uint8_t *p,size_t n)override {
    ++writes;
    if(cut>=0){rows[k]={p,p+std::min(n,size_t(cut))};return -EIO;}
    rows[k]={p,p+n};return ambiguous?-EIO:0;
  }
};
static uint8_t ownerA[32]{1},ownerB[32]{2};
static void load(TransmitHistory &h,Store &s,const uint8_t *key=ownerA,uint32_t node=1234){
  assert(!h.load(s,node,key,clockNow));
}
static void checkClock(){
  oracle::TransmitHistory donor;
  for(const uint64_t up:{0ull,1ull,2000ull,120001ull,240999ull,86400001ull,0xffffffffull,0x100000dfaull}){
    for(uint32_t utc:{0u,1u,1000u,1800000000u}){
      sample={up,utc};
      for(uint32_t stamp:{2u,119u,120u,121u,239u,240u,241u,999u,1000u,
                          1799913599u,1799913600u,1799999940u,1800000000u,1800000001u,UINT32_MAX}){
        assert(mt::history::recoveredMillis(stamp,true,sample)==donor.getLastSentBootRelativeMillis(stamp));
        assert(mt::history::recoveredMillis(stamp,false,sample)==donor.getLastSentAbsoluteMillis(stamp));
      }
    }
  }
}
static void allKinds(){
  Store s;TransmitHistory h;load(h,s);
  for(unsigned i=0;i<unsigned(Kind::Count);++i){
    const auto kind=Kind(i);
    assert(!h.has(kind)&&h.allows(kind,60000));
    sample={123000+i*1000u,1800000000u+i};h.record(kind);
    assert(h.has(kind)&&!h.allows(kind,60000));
    if(i==0){assert(h.due(sample.uptimeMs));assert(!h.save(s,sample.uptimeMs));}
    else assert(!h.due(sample.uptimeMs));
  }
  assert(s.writes==1);sample={423000,1800000300u};assert(h.due(sample.uptimeMs));assert(!h.save(s,sample.uptimeMs));
  assert(s.writes==2&&!h.due(sample.uptimeMs+1000000));
  assert(!h.save(s,sample.uptimeMs+1000000)&&s.writes==2);
  TransmitHistory reset;load(reset,s);sample={1200,0};
  for(unsigned i=0;i<unsigned(Kind::Count);++i)assert(reset.has(Kind(i))&&reset.allows(Kind(i),60000));
  for(unsigned i=0;i<unsigned(Kind::Count);++i){
    const auto k=Kind(i);sample={3000,1800000000u+i+59};assert(!reset.allows(k,60000));++sample.utc;assert(reset.allows(k,60000));
  }
  sample={1000,1800000030u};assert(!reset.allows(Kind::NodeInfo,60000));
  TransmitHistory other;load(other,s,ownerB);assert(!other.has(Kind::NodeInfo)&&other.allows(Kind::NodeInfo,60000));
  other.record(Kind::Health);assert(!other.save(s,1000));
  TransmitHistory firstAgain;load(firstAgain,s);assert(!firstAgain.has(Kind::NodeInfo));
  // Old row with the same node ID cannot revive through a changed owner key.
  TransmitHistory changedNode;load(changedNode,s,ownerB,5678);assert(!changedNode.has(Kind::Health));
}
static void interruptions(){
  Store initial;TransmitHistory h;load(h,initial);sample={3000,0};h.record(Kind::NodeInfo);assert(!h.save(initial,3000));
  const auto size=initial.rows.at("mttx/history0").size();
  for(size_t n=0;n<=size;++n){
    Store s=initial;TransmitHistory active;load(active,s);sample={9100,0};
    active.record(Kind::Power);s.cut=int(n);assert(active.save(s,9100)==-EIO);
    s.cut=-1;TransmitHistory cold;load(cold,s);assert(cold.has(Kind::NodeInfo));
    assert(cold.has(Kind::Power)==(n==size));cold.record(Kind::Air);assert(!cold.save(s,9150));
    TransmitHistory proven;load(proven,s);assert(proven.has(Kind::Air)&&proven.has(Kind::NodeInfo));
  }
  Store s;TransmitHistory fresh;load(fresh,s);sample={1000,0};fresh.record(Kind::Device);assert(!fresh.due(1000));
  sample={60000,0};fresh.record(Kind::Device);s.ambiguous=true;assert(fresh.save(s,60000)==-EIO);
  s.ambiguous=false;assert(!fresh.save(s,61000));TransmitHistory reboot;load(reboot,s);
  sample={6000,0};assert(!reboot.allows(Kind::Device,60000));sample={120000,0};assert(reboot.allows(Kind::Device,60000));
  sample={6000,1800000000u};assert(reboot.allows(Kind::Device,60000)); // Once UTC is acquired, discard boot-relative recovery.
  Store failed=s;failed.readFail=true;TransmitHistory blocked;assert(blocked.load(failed,1234,ownerA,clockNow)==-EIO);
  blocked.record(Kind::Power);assert(blocked.save(failed,500000)<0&&failed.writes==s.writes);
  const auto keep=s.rows;s.rows["mttx/history1"]={'Z','T','H',99};
  assert(blocked.load(s,1234,ownerA,clockNow)==-EINVAL);assert(blocked.save(s,500000)<0);
  assert(s.rows.at("mttx/history0")==keep.at("mttx/history0")&&s.rows.at("mttx/history1").size()==4);
}
int main(){checkClock();allKinds();interruptions();
  std::puts("PASS pinned UTC/relative behavior, eight independent clocks, wear limits, owner changes, future formats and all 83 partial/reported-error commit points");}
