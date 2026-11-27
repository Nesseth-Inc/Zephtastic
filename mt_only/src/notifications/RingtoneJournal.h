// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "RingtoneSettings.h"
#include "../SettingsJournal.h"

namespace mt::notification {
class RingtoneJournal {
public:
  static constexpr size_t RecordSize=12+RingtoneSettings::Capacity+4;
  enum class State:uint8_t {Unloaded,Fresh,Loaded,Recovered,Invalid,IoError};
private:
  RingtoneSettings settings_{};uint32_t generation_=0;int8_t active_=-1;State state_=State::Unloaded;
  static const char *key(unsigned slot){return slot?"mtringtone/state1":"mtringtone/state0";}
  static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
  static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
  static uint32_t crc32(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;}
  static void encode(const RingtoneSettings &s,uint32_t gen,uint8_t *out){
    memset(out,0,RecordSize);out[0]='Z';out[1]='T';out[2]='R';out[3]=1;put32(out+4,gen);
    const uint16_t n=uint16_t(s.length());out[8]=uint8_t(n);out[9]=uint8_t(n>>8);memcpy(out+12,s.ringtone,RingtoneSettings::Capacity);
    put32(out+RecordSize-4,crc32(out,RecordSize-4));
  }
  static bool decode(const uint8_t *p,int n,RingtoneSettings &s,uint32_t &gen){
    if(n!=int(RecordSize)||p[0]!='Z'||p[1]!='T'||p[2]!='R'||p[3]!=1||p[10]||p[11]||
       get32(p+RecordSize-4)!=crc32(p,RecordSize-4))return false;
    const size_t len=size_t(p[8])|(size_t(p[9])<<8);if(!len||len>RingtoneSettings::MaxBytes)return false;
    RingtoneSettings candidate;memcpy(candidate.ringtone,p+12,RingtoneSettings::Capacity);
    if(candidate.length()!=len||!candidate.valid())return false;s=candidate;gen=get32(p+4);return true;
  }
public:
  const RingtoneSettings &config()const{return settings_;}State state()const{return state_;}
  uint32_t generation()const{return generation_;}int activeSlot()const{return active_;}
  bool ready()const{return state_==State::Fresh||state_==State::Loaded||state_==State::Recovered;}
  int load(SettingsBackend &backend){
    settings_=RingtoneSettings{};generation_=0;active_=-1;state_=State::Unloaded;
    uint8_t raw[RecordSize]{};RingtoneSettings candidates[2];uint32_t generations[2]{};bool good[2]{},present[2]{};
    for(unsigned i=0;i<2;++i){const int n=backend.read(key(i),raw,sizeof(raw));if(n<0&&n!=-ENOENT&&n!=-E2BIG){state_=State::IoError;return n;}present[i]=n!=-ENOENT;good[i]=decode(raw,n,candidates[i],generations[i]);}
    if(good[0]||good[1]){
      unsigned slot=good[1]?1:0;
      if(good[0]&&good[1]){const uint32_t delta=generations[1]-generations[0];if((!delta&&!candidates[0].same(candidates[1]))||delta==0x80000000u){state_=State::Invalid;return -EINVAL;}slot=delta&&delta<0x80000000u?1:0;}
      settings_=candidates[slot];generation_=generations[slot];active_=int8_t(slot);state_=(present[0]&&!good[0])||(present[1]&&!good[1])?State::Recovered:State::Loaded;return 0;
    }
    if(!present[0]&&!present[1]){state_=State::Fresh;return 0;}state_=State::Invalid;return -EINVAL;
  }
  int save(SettingsBackend &backend,const RingtoneSettings &candidate){
    if(!ready())return -EACCES;if(!candidate.valid())return -EINVAL;if(active_>=0&&settings_.same(candidate))return 0;
    const unsigned slot=active_==0?1:0;const uint32_t next=generation_+1;uint8_t record[RecordSize],readback[RecordSize];encode(candidate,next,record);
    const int result = backend.writeVerified(key(slot), record, sizeof(record), readback, sizeof(readback));
    if (result) {
      state_ = State::IoError;
      return result;
    }
    settings_=candidate;generation_=next;active_=int8_t(slot);state_=State::Loaded;return 0;
  }
};
static_assert(RingtoneJournal::RecordSize==247);
}
