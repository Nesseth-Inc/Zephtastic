// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Config.h"
#include <errno.h>

namespace mt {
// A settings backend must return the full stored length (or -E2BIG if it exceeds
// capacity), -ENOENT for absence, and other negative errno values for I/O errors.
// Writes return zero only after the backend's own commit has completed. Calls
// are serialized by the application's service lock, never from radio callbacks.
struct SettingsBackend {
  virtual int read(const char *key,uint8_t *out,size_t capacity)=0;
  virtual int write(const char *key,const uint8_t *data,size_t length)=0;
};

class SettingsJournal {
public:
  static constexpr size_t PreviousRecordSize=112,FormerRecordSize=128,PrimaryRecordSize=144,BankRecordSize=592,RecordSize=624;
  enum class State:uint8_t {Unloaded,Fresh,Legacy,Loaded,Recovered,Invalid,IoError};
private:
  Config config_;
  uint32_t generation_=0;
  int8_t active_=-1;
  State state_=State::Unloaded;
  static const char *key(unsigned slot){return slot?"mtbase/state1":"mtbase/state0";}
  static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
  static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
  static uint32_t crc32(const uint8_t *p,size_t n){
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}
    return ~c;
  }
  static void encode(const Config &c,uint32_t generation,uint8_t *out){
    memset(out,0,RecordSize);out[0]='Z';out[1]='T';out[2]='J';out[3]=5;
    put32(out+4,generation);out[8]=uint8_t(Config::Size);out[9]=uint8_t(Config::Size>>8);
    memcpy(out+12,c.bytes,Config::Size);put32(out+RecordSize-4,crc32(out,RecordSize-4));
  }
  static bool decode(const uint8_t *p,int n,Config &c,uint32_t &generation){
    size_t recordSize=0,configSize=0;
    if(n==int(RecordSize)&&p[3]==5&&(uint16_t(p[8])|(uint16_t(p[9])<<8))==Config::Size){recordSize=RecordSize;configSize=Config::Size;}
    else if(n==int(BankRecordSize)&&p[3]==4&&(uint16_t(p[8])|(uint16_t(p[9])<<8))==Config::BankSize){recordSize=BankRecordSize;configSize=Config::BankSize;}
    else if(n==int(PrimaryRecordSize)&&p[3]==3&&p[8]==Config::PrimarySize&&!p[9]){recordSize=PrimaryRecordSize;configSize=Config::PrimarySize;}
    else if(n==int(FormerRecordSize)&&p[3]==2&&p[8]==Config::FormerSize){recordSize=FormerRecordSize;configSize=Config::FormerSize;}
    else if(n==int(PreviousRecordSize)&&p[3]==1&&p[8]==Config::PreviousSize){recordSize=PreviousRecordSize;configSize=Config::PreviousSize;}
    else return false;
    if(p[0]!='Z'||p[1]!='T'||p[2]!='J'||(p[3]<4&&p[9])||p[10]||p[11]||
       get32(p+recordSize-4)!=crc32(p,recordSize-4)||!Config::decode(p+12,configSize,c))return false;
    generation=get32(p+4);return true;
  }
public:
  const Config &config()const{return config_;}
  State state()const{return state_;}
  uint32_t generation()const{return generation_;}
  int activeSlot()const{return active_;}
  bool ready()const{return state_==State::Fresh||state_==State::Legacy||state_==State::Loaded||state_==State::Recovered;}
  int load(SettingsBackend &backend){
    config_=Config{};generation_=0;active_=-1;state_=State::Unloaded;
    uint8_t raw[RecordSize];Config candidates[2];uint32_t generations[2]{};bool good[2]{},present[2]{};
    for(unsigned i=0;i<2;++i){
      const int n=backend.read(key(i),raw,sizeof(raw));
      if(n<0&&n!=-ENOENT&&n!=-E2BIG){state_=State::IoError;return n;}
      present[i]=n!=-ENOENT;good[i]=decode(raw,n,candidates[i],generations[i]);
    }
    if(good[0]||good[1]){
      unsigned slot=good[1]?1:0;
      if(good[0]&&good[1]){
        const uint32_t delta=generations[1]-generations[0];
        // Conflicting same-generation or half-range records have no reliable
        // order. Do not silently choose names/channel from an ambiguous state.
        if((!delta&&!candidates[0].same(candidates[1]))||delta==0x80000000u){state_=State::Invalid;return -EINVAL;}
        slot=delta&&delta<0x80000000u?1:0;
      }
      config_=candidates[slot];generation_=generations[slot];active_=int8_t(slot);
      state_=(present[0]&&!good[0])||(present[1]&&!good[1])?State::Recovered:State::Loaded;return 0;
    }
    const int n=backend.read("mtbase/config",raw,sizeof(raw));
    if(n<0&&n!=-ENOENT&&n!=-E2BIG){state_=State::IoError;return n;}
    // Only the explicitly supported v1-v3 standalone Config layouts are
    // migration sources. V4 is migrated from the current journal record above.
    // A future or arbitrary blob in this key must not silently become a journal.
    if((n==int(Config::LegacySize)||n==int(Config::PreviousSize))&&Config::decode(raw,size_t(n),config_)){
      state_=present[0]||present[1]?State::Recovered:State::Legacy;return 0;
    }
    if(n==-ENOENT&&!present[0]&&!present[1]){state_=State::Fresh;return 0;}
    state_=State::Invalid;return -EINVAL;
  }
  int save(SettingsBackend &backend,const Config &candidate){
    if(!ready())return -EACCES;
    if(!candidate.valid())return -EINVAL;
    if(active_>=0&&config_.same(candidate))return 0;
    const unsigned slot=active_==0?1:0;
    const uint32_t next=generation_+1;
    uint8_t record[RecordSize],readback[RecordSize];encode(candidate,next,record);
    const int ret=backend.write(key(slot),record,sizeof(record));
    if(ret){
      // A write error is not proof that nothing committed. In particular, do
      // not later elide a save of the stale RAM config and acknowledge it as
      // durable while the backend could contain the new generation.
      state_=State::IoError;return ret;
    }
    const int n=backend.read(key(slot),readback,sizeof(readback));
    if(n!=int(sizeof(record))||memcmp(record,readback,sizeof(record))){
      // Commit outcome is uncertain. Retain the old RAM configuration, but
      // require an explicit reload/reboot before any further settings writes.
      state_=State::IoError;return n<0?n:-EIO;
    }
    config_=candidate;generation_=next;active_=int8_t(slot);state_=State::Loaded;return 0;
  }
};
}
