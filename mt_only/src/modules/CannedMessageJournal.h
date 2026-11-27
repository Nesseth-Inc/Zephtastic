// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "CannedMessageSettings.h"
#include "../SettingsJournal.h"

namespace mt::modules {
// Independent two-slot canned-list journal. It does not read or modify
// Config, NodeDB, ModuleSettings, message history, identity, or peer keys.
class CannedMessageJournal {
public:
  static constexpr size_t LegacyInnerSize=208;
  static constexpr size_t LegacyRecordSize=224;
  static constexpr size_t V2HeaderSize=12;
  static constexpr size_t V2Overhead=16;
  static constexpr size_t RecordSize=LegacyRecordSize; // Maximum read capacity.
  enum class State:uint8_t {Unloaded,Fresh,Loaded,Recovered,Invalid,IoError};

private:
  CannedMessageSettings config_;
  uint32_t generation_=0;
  int8_t active_=-1;
  State state_=State::Unloaded;

  static const char *key(unsigned slot){return slot?"mtcanned/state1":"mtcanned/state0";}
  static uint32_t get32(const uint8_t *p){
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
  }
  static uint16_t get16(const uint8_t *p){return uint16_t(p[0])|uint16_t(uint16_t(p[1])<<8);}
  static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
  static uint32_t crc32(const uint8_t *p,size_t n){
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}
    return ~c;
  }

  static void encodeInner(const CannedMessageSettings &c,uint8_t *out){
    memset(out,0,LegacyInnerSize);out[0]='Z';out[1]='T';out[2]='C';out[3]=1;
    memcpy(out+4,c.messages,CannedMessageSettings::Capacity);
    const auto crc=Config::checksum(out,206);out[206]=uint8_t(crc);out[207]=uint8_t(crc>>8);
  }
  static bool decodeInner(const uint8_t *p,CannedMessageSettings &out){
    if(p[0]!='Z'||p[1]!='T'||p[2]!='C'||p[3]!=1||p[205]||
       get16(p+206)!=Config::checksum(p,206))return false;
    CannedMessageSettings candidate;
    memcpy(candidate.messages,p+4,CannedMessageSettings::Capacity);
    if(!candidate.valid())return false;
    out=candidate;return true;
  }
  static size_t encodeV2(const CannedMessageSettings &c,uint32_t generation,uint8_t *out){
    const size_t n=c.length();
    memset(out,0,V2HeaderSize);out[0]='Z';out[1]='T';out[2]='C';out[3]=2;
    put32(out+4,generation);out[8]=uint8_t(n);out[9]=uint8_t(n>>8);
    memcpy(out+V2HeaderSize,c.messages,n);
    put32(out+V2HeaderSize+n,crc32(out,V2HeaderSize+n));
    return V2Overhead+n;
  }
  static bool decodeLegacy(const uint8_t *p,int n,CannedMessageSettings &c,uint32_t &generation){
    if(n!=int(LegacyRecordSize)||p[0]!='Z'||p[1]!='T'||p[2]!='C'||p[3]!=1||
       get16(p+8)!=LegacyInnerSize||p[10]||p[11]||
       get32(p+LegacyRecordSize-4)!=crc32(p,LegacyRecordSize-4))return false;
    CannedMessageSettings candidate;
    if(!decodeInner(p+12,candidate))return false;
    c=candidate;generation=get32(p+4);return true;
  }
  static bool decodeV2(const uint8_t *p,int n,CannedMessageSettings &c,uint32_t &generation){
    if(n<int(V2Overhead)||n>int(V2Overhead+CannedMessageSettings::MaxBytes)||
       p[0]!='Z'||p[1]!='T'||p[2]!='C'||p[3]!=2||p[10]||p[11])return false;
    const size_t raw=get16(p+8);
    if(raw==0||raw>CannedMessageSettings::MaxBytes||size_t(n)!=V2Overhead+raw||
       get32(p+V2HeaderSize+raw)!=crc32(p,V2HeaderSize+raw))return false;
    const uint8_t *bytes=p+V2HeaderSize;
    if(memchr(bytes,0,raw))return false;
    CannedMessageSettings candidate;
    const auto assigned=candidate.assign(reinterpret_cast<const char *>(bytes),raw);
    if(assigned!=CannedMessageSettings::AssignResult::Changed&&
       assigned!=CannedMessageSettings::AssignResult::Unchanged)return false;
    c=candidate;generation=get32(p+4);return true;
  }
  static bool decode(const uint8_t *p,int n,CannedMessageSettings &c,uint32_t &generation){
    if(n<4||p[0]!='Z'||p[1]!='T'||p[2]!='C')return false;
    if(p[3]==1)return decodeLegacy(p,n,c,generation);
    if(p[3]==2)return decodeV2(p,n,c,generation);
    return false;
  }

public:
  const CannedMessageSettings &config()const{return config_;}
  State state()const{return state_;}
  uint32_t generation()const{return generation_;}
  int activeSlot()const{return active_;}
  bool ready()const{return state_==State::Fresh||state_==State::Loaded||state_==State::Recovered;}

  int load(SettingsBackend &backend){
    config_=CannedMessageSettings{};generation_=0;active_=-1;state_=State::Unloaded;
    uint8_t raw[RecordSize]{};CannedMessageSettings candidates[2];uint32_t generations[2]{};bool good[2]{},present[2]{};
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

  int save(SettingsBackend &backend,const CannedMessageSettings &candidate){
    if(!ready())return -EACCES;
    if(!candidate.valid())return -EINVAL;
    if(active_>=0&&config_.same(candidate))return 0;
    const unsigned slot=active_==0?1:0;const uint32_t next=generation_+1;
    uint8_t record[RecordSize],readback[RecordSize];const size_t encoded=encodeV2(candidate,next,record);
    const int result = backend.writeVerified(key(slot), record, encoded, readback, sizeof(readback));
    if (result) {
      state_ = State::IoError;
      return result;
    }
    config_=candidate;generation_=next;active_=int8_t(slot);state_=State::Loaded;return 0;
  }
};
static_assert(CannedMessageJournal::LegacyInnerSize==4+CannedMessageSettings::Capacity+1+2);
static_assert(CannedMessageJournal::LegacyRecordSize==12+CannedMessageJournal::LegacyInnerSize+4);
static_assert(CannedMessageJournal::V2Overhead==CannedMessageJournal::V2HeaderSize+4);
static_assert(sizeof(CannedMessageJournal)<=256,"canned message live store must remain within the M130 RAM budget");
}
