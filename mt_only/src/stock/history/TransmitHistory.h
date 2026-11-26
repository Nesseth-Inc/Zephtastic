// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d, src/mesh/TransmitHistory.cpp. Public schedule times only.
#pragma once
#include <stdint.h>
namespace mt::history {
enum class Kind : uint8_t { NodeInfo, Position, Device, Environment, Power, Air, Health, Beacon, Count };
struct Clock {uint64_t uptimeMs;uint32_t utc;};

inline uint32_t recoveredMillis(uint32_t stamp,bool relative,Clock clock){
  if(!stamp)return 0;
  const uint32_t uptime=uint32_t(clock.uptimeMs);
  uint32_t age=0;
  if(relative){
    if(clock.utc||clock.uptimeMs/1000>UINT32_MAX)return 0;
    const uint32_t now=uint32_t(clock.uptimeMs/1000);
    if(stamp>now){if(stamp-now>120)return 0;}
    else {age=now-stamp;if(age>120)return 0;}
  }else{
    const uint32_t now=clock.utc?clock.utc:uint32_t(clock.uptimeMs/1000);
    if(now<2||stamp>now)return 0;
    age=now-stamp;if(age>86400)return 0;
  }
  const uint32_t last=uptime-age*1000u;
  return last?last:1u;
}
}
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
#include "../../SettingsJournal.h"
namespace mt::history {
class TransmitHistory {
  static constexpr unsigned Count=unsigned(Kind::Count);
  static constexpr unsigned Size=82; // magic/version, gen, ID, public key, stamps, relative bits, reserved, CRC
  static constexpr uint64_t SaveInterval=300000;
  uint32_t stamps_[Count]{},runtime_[Count]{};
  uint32_t generation_=0,node_=0;uint8_t public_[32]{},relative_=0;
  uint64_t lastWrite_=0,retryAt_=0;
  bool readable_=false,wrote_=false,dirty_=false;
  int active_=-1;
  static const char *key(unsigned i){return i?"mttx/history1":"mttx/history0";}
  static bool valid(const uint8_t *p){
    if(memcmp(p,"ZTH\1",4)||!Config::get32(p+4)||!Config::get32(p+8)||p[77]||p[78]||p[79]||
       Config::checksum(p,Size-2)!=(uint16_t(p[80])|(uint16_t(p[81])<<8)))return false;
    for(unsigned i=0;i<Count;++i)if((p[76]&(1u<<i))&&Config::get32(p+44+4*i)<2)return false;
    return true;
  }
  void encode(uint8_t *p,uint32_t generation)const{
    memcpy(p,"ZTH\1",4);Config::put32(p+4,generation);Config::put32(p+8,node_);
    memcpy(p+12,public_,sizeof(public_));
    for(unsigned i=0;i<Count;++i)Config::put32(p+44+4*i,stamps_[i]);
    p[76]=relative_;const auto crc=Config::checksum(p,Size-2);p[80]=uint8_t(crc);p[81]=uint8_t(crc>>8);
  }
public:
  using ReadClock=Clock (*)();
  ReadClock clock=nullptr;
  int load(SettingsBackend &store,uint32_t node,const uint8_t *pub,ReadClock source){
    *this={};clock=source;
    if(!source||!node||node==UINT32_MAX)return -EINVAL;
    node_=node;if(pub)memcpy(public_,pub,sizeof(public_));
    uint8_t rows[2][Size]{};uint32_t generations[2]{};
    bool unknown=false;
    for(unsigned i=0;i<2;++i){
      const int n=store.read(key(i),rows[i],Size);
      if(n==-ENOENT)continue;
      if(n<0){return n;}
      if(n!=Size||!valid(rows[i])){
        // The NVS commit is atomic. Also tolerate a truncated supported bank
        // on backends that reveal partial data. Never overwrite a future format.
        const auto prefix=n<4?size_t(n):size_t(4);
        if(!prefix||memcmp(rows[i],"ZTH\1",prefix))unknown=true;
        continue;
      }
      generations[i]=Config::get32(rows[i]+4);
    }
    if(generations[0]&&generations[0]==generations[1])return -EINVAL;
    if(generations[0]||generations[1]){
      active_=generations[1]>generations[0]?1:0;const auto p=rows[active_];
      generation_=generations[active_];
      if(Config::get32(p+8)==node_&&!memcmp(p+12,public_,sizeof(public_))){
        for(unsigned i=0;i<Count;++i)stamps_[i]=Config::get32(p+44+4*i);
        relative_=p[76];
      }
    }
    // A partially written supported bank is recoverable. A future or
    // unrecognizable store stays intact. Individual callers retain RAM cadence.
    readable_=!unknown;
    return readable_?0:-EINVAL;
  }
  bool readable()const{return readable_;}
  bool has(Kind k)const{return stamps_[unsigned(k)]||runtime_[unsigned(k)];}
  uint32_t last(Kind kind)const{
    const unsigned k=unsigned(kind);return runtime_[k]?runtime_[k]:clock?
      recoveredMillis(stamps_[k],(relative_&(1u<<k))!=0,clock()):0;
  }
  bool allows(Kind k,uint64_t interval)const{
    const auto prior=last(k);
    return !prior||uint32_t(uint32_t(clock().uptimeMs)-prior)>=interval;
  }
  void record(Kind kind){
    if(!clock)return;
    const auto now=clock();const auto k=unsigned(kind);const uint32_t ms=uint32_t(now.uptimeMs);
    runtime_[k]=ms?ms:1;
    const auto sec=now.utc?uint64_t(now.utc):now.uptimeMs/1000;
    if(sec<2||sec>UINT32_MAX||!readable_)return;
    const auto mask=uint8_t(1u<<k);const auto flags=uint8_t(now.utc?(relative_&~mask):(relative_|mask));
    if(stamps_[k]==sec&&flags==relative_)return;
    stamps_[k]=uint32_t(sec);relative_=flags;dirty_=true;
  }
  bool due(uint64_t now)const{return dirty_&&readable_&&now>=retryAt_&&(!wrote_||now-lastWrite_>=SaveInterval);}
  void retry(uint64_t now){retryAt_=now+1000;}
  int save(SettingsBackend &store,uint64_t now){
    if(!readable_)return -EINVAL;
    if(!dirty_)return 0;
    if(generation_==UINT32_MAX)return -EOVERFLOW;
    retry(now);uint8_t candidate[Size]{},got[Size]{};
    encode(candidate,generation_+1);const unsigned target=active_==0?1:0;
    const int rc=store.write(key(target),candidate,Size);
    if(rc)return rc;
    const int n=store.read(key(target),got,Size);
    if(n!=Size||memcmp(candidate,got,Size))return n<0?n:-EIO;
    active_=int(target);++generation_;dirty_=false;wrote_=true;lastWrite_=now;return 0;
  }
};
} // namespace mt::history
#endif
