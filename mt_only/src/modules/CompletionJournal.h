// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "CompletionSettings.h"
#include "../SettingsJournal.h"

namespace mt::modules {
// New namespace only. Compact storage omits the record's zero padding; CRC32
// and a byte-exact readback protect each generation. No silent repair/retry.
class CompletionJournal {
public:
  static constexpr size_t RecordSize=CompletionSettings::Size+18;
  enum class State:uint8_t {Unloaded,Fresh,Loaded,Recovered,Invalid,IoError};
private:
  CompletionSettings current_;
  uint32_t generation_=0;int active_=-1;State state_=State::Unloaded;
  static const char *key(unsigned n){return n?"mtcomplete/state1":"mtcomplete/state0";}
  static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;}
  static bool decode(const uint8_t *raw,int length,CompletionSettings &out,uint32_t &generation){
    if(length<26||length>int(RecordSize)||raw[0]!='Z'||raw[1]!='C'||raw[2]!='J'||raw[3]!=1||raw[10]||raw[11])return false;
    const size_t payload=size_t(raw[8])|(size_t(raw[9])<<8);
    if(payload<CompletionSettings::Header+2||payload>CompletionSettings::Size||payload+16!=size_t(length)||
       Config::get32(raw+length-4)!=crc(raw,size_t(length)-4))return false;
    CompletionSettings candidate;memset(candidate.bytes,0,candidate.Size);
    memcpy(candidate.bytes,raw+12,payload-2);memcpy(candidate.bytes+candidate.Size-2,raw+12+payload-2,2);
    if(!candidate.valid()||candidate.storedPrefix()+2!=payload)return false;
    generation=Config::get32(raw+4);out=candidate;return true;
  }
public:
  const CompletionSettings &config()const{return current_;}
  State state()const{return state_;}
  bool ready()const{return state_==State::Fresh||state_==State::Loaded||state_==State::Recovered;}
  bool trusted()const{return state_==State::Fresh||state_==State::Loaded;}
  int load(SettingsBackend &backend){
    current_=CompletionSettings{};generation_=0;active_=-1;state_=State::Unloaded;
    bool good[2]{},present[2]{};CompletionSettings values[2];uint32_t gens[2]{};uint8_t raw[RecordSize];
    for(unsigned n=0;n<2;++n){const int count=backend.read(key(n),raw,sizeof(raw));
      if(count<0&&count!=-ENOENT&&count!=-E2BIG){state_=State::IoError;return count;}
      present[n]=count!=-ENOENT;good[n]=decode(raw,count,values[n],gens[n]);}
    if(!good[0]&&!good[1]){state_=present[0]||present[1]?State::Invalid:State::Fresh;return ready()?0:-EINVAL;}
    unsigned selected=good[1]?1:0;
    if(good[0]&&good[1]){const uint32_t delta=gens[1]-gens[0];
      if((delta==0&&!values[0].same(values[1]))||delta==0x80000000u){state_=State::Invalid;return -EINVAL;}
      selected=delta&&delta<0x80000000u?1:0;}
    current_=values[selected];generation_=gens[selected];active_=int(selected);
    state_=(present[0]&&!good[0])||(present[1]&&!good[1])?State::Recovered:State::Loaded;return 0;
  }
  int save(SettingsBackend &backend,const CompletionSettings &value){
    if(!ready())return -EACCES;if(!value.valid())return -EINVAL;
    if(state_==State::Loaded&&value.same(current_))return 0;
    uint8_t raw[RecordSize]{},readback[RecordSize]{};const size_t prefix=value.storedPrefix(),payload=prefix+2,n=payload+16;
    raw[0]='Z';raw[1]='C';raw[2]='J';raw[3]=1;Config::put32(raw+4,generation_+1);
    raw[8]=uint8_t(payload);raw[9]=uint8_t(payload>>8);memcpy(raw+12,value.bytes,prefix);
    memcpy(raw+12+prefix,value.bytes+value.Size-2,2);Config::put32(raw+n-4,crc(raw,n-4));
    const unsigned slot=active_==0?1:0;const int written=backend.write(key(slot),raw,n);
    if(written){state_=State::IoError;return written;}
    const int count=backend.read(key(slot),readback,sizeof(readback));
    if(count!=int(n)||memcmp(raw,readback,n)){state_=State::IoError;return count<0?count:-EIO;}
    current_=value;++generation_;active_=int(slot);state_=State::Loaded;return 0;
  }
};
} // namespace mt::modules
