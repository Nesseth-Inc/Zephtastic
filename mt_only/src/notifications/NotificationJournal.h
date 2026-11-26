// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "NotificationSettings.h"
#include "../SettingsJournal.h"

namespace mt {
// Literal ModuleJournal algorithm in an independent notification namespace.
// No legacy/core/module key, migration, erase or retry path is introduced.
class NotificationJournal {
public:
  static constexpr size_t RecordSize=32;
  enum class State:uint8_t {Unloaded,Fresh,Loaded,Recovered,Invalid,IoError};
private:
  NotificationSettings config_;
  uint32_t generation_=0;
  int8_t active_=-1;
  State state_=State::Unloaded;
  static const char *key(unsigned slot){return slot?"mtnotify/state1":"mtnotify/state0";}
  static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
  static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
  static uint32_t crc32(const uint8_t *p,size_t n){
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}
    return ~c;
  }
  static void encode(const NotificationSettings &c,uint32_t generation,uint8_t *out){
    memset(out,0,RecordSize);out[0]='Z';out[1]='T';out[2]='N';out[3]=1;
    put32(out+4,generation);out[8]=NotificationSettings::Size;
    memcpy(out+12,c.bytes,NotificationSettings::Size);put32(out+RecordSize-4,crc32(out,RecordSize-4));
  }
  static bool decode(const uint8_t *p,int n,NotificationSettings &c,uint32_t &generation){
    if(n!=int(RecordSize)||p[0]!='Z'||p[1]!='T'||p[2]!='N'||p[3]!=1||p[8]!=NotificationSettings::Size||p[9]||p[10]||p[11]||
       get32(p+RecordSize-4)!=crc32(p,RecordSize-4))return false;
    NotificationSettings candidate;memcpy(candidate.bytes,p+12,NotificationSettings::Size);
    if(!candidate.valid())return false;
    c=candidate;
    generation=get32(p+4);return true;
  }
public:
  const NotificationSettings &config()const{return config_;}
  State state()const{return state_;}
  uint32_t generation()const{return generation_;}
  int activeSlot()const{return active_;}
  bool ready()const{return state_==State::Fresh||state_==State::Loaded||state_==State::Recovered;}
  int load(SettingsBackend &backend){
    config_=NotificationSettings{};generation_=0;active_=-1;state_=State::Unloaded;
    uint8_t raw[RecordSize];NotificationSettings candidates[2];uint32_t generations[2]{};bool good[2]{},present[2]{};
    for(unsigned i=0;i<2;++i){
      const int n=backend.read(key(i),raw,sizeof(raw));
      if(n<0&&n!=-ENOENT&&n!=-E2BIG){state_=State::IoError;return n;}
      present[i]=n!=-ENOENT;good[i]=decode(raw,n,candidates[i],generations[i]);
    }
    if(good[0]||good[1]){
      unsigned slot=good[1]?1:0;
      if(good[0]&&good[1]){
        const uint32_t delta=generations[1]-generations[0];
        if((!delta&&!candidates[0].same(candidates[1]))||delta==0x80000000u){state_=State::Invalid;return -EINVAL;}
        slot=delta&&delta<0x80000000u?1:0;
      }
      config_=candidates[slot];generation_=generations[slot];active_=int8_t(slot);
      state_=(present[0]&&!good[0])||(present[1]&&!good[1])?State::Recovered:State::Loaded;return 0;
    }
    if(!present[0]&&!present[1]){state_=State::Fresh;return 0;}
    state_=State::Invalid;return -EINVAL;
  }
  int save(SettingsBackend &backend,const NotificationSettings &candidate){
    if(!ready())return -EACCES;
    if(!candidate.valid())return -EINVAL;
    if(active_>=0&&config_.same(candidate))return 0;
    const unsigned slot=active_==0?1:0;
    const uint32_t next=generation_+1;
    uint8_t record[RecordSize],readback[RecordSize];encode(candidate,next,record);
    const int ret=backend.write(key(slot),record,sizeof(record));
    if(ret){state_=State::IoError;return ret;}
    const int n=backend.read(key(slot),readback,sizeof(readback));
    if(n!=int(sizeof(record))||memcmp(record,readback,sizeof(record))){
      state_=State::IoError;return n<0?n:-EIO;
    }
    config_=candidate;generation_=next;active_=int8_t(slot);state_=State::Loaded;return 0;
  }
};
}
