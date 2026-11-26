// SPDX-License-Identifier: GPL-3.0-only
// ExtendedModuleJournal.h — compact dual-slot journal for ExtendedModuleSettings.
// V1 remains readable as its exact 336-byte dense wrapper. V2 stores the exact
// 320-byte inner record sparsely: 12-byte header, 40-byte bitmap, nonzero bytes
// in index order, then CRC32. V2 is written only when strictly smaller than V1.
// Only mtextra/state0 and mtextra/state1 are ever read or written; no other
// keys are read, written, migrated, deleted, or aliased. Candidate bytes are
// stored verbatim; the journal performs no credential handling. Missing
// mtextra keys mean the default config; Fresh requires the default to validate.
#pragma once
#include "ExtendedModuleSettings.h"
#include "../SettingsJournal.h"

namespace mt {
namespace modules {
// Independent dual-slot extended-module journal, using the existing backend.
// A failed or ambiguous write poisons further saves until reload. Missing
// mtextra keys mean the default config; Fresh is valid only when both keys
// are absent and the default config validates.
class ExtendedModuleJournal {
public:
  static constexpr size_t RecordSize=336,HeaderSize=12,BitmapSize=40,V2FixedSize=HeaderSize+BitmapSize;
  static constexpr uint8_t DenseVersion=1,SparseVersion=2;
  enum class State:uint8_t {Unloaded,Fresh,Loaded,Recovered,Invalid,IoError};
private:
  ExtendedModuleSettings config_;
  uint32_t generation_=0;
  int8_t active_=-1;
  State state_=State::Unloaded;
  static_assert(ExtendedModuleSettings::Size==320,"wrapper layout assumes the 320-byte record");
  static const char *key(unsigned slot){return slot?"mtextra/state1":"mtextra/state0";}
  static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
  static uint16_t get16(const uint8_t *p){return uint16_t(p[0])|uint16_t(uint16_t(p[1])<<8);}
  static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
  static uint32_t crc32(const uint8_t *p,size_t n){
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}
    return ~c;
  }
  static size_t encode(const ExtendedModuleSettings &c,uint32_t generation,uint8_t *out){
    size_t packed=0;for(uint8_t value:c.bytes)if(value)++packed;
    if(V2FixedSize+packed+4<RecordSize){
      memset(out,0,RecordSize);out[0]='Z';out[1]='T';out[2]='X';out[3]=SparseVersion;
      put32(out+4,generation);
      out[8]=uint8_t(ExtendedModuleSettings::Size);out[9]=uint8_t(ExtendedModuleSettings::Size>>8);
      out[10]=uint8_t(packed);out[11]=uint8_t(packed>>8);
      size_t at=V2FixedSize;
      for(size_t i=0;i<ExtendedModuleSettings::Size;++i)if(c.bytes[i]){
        out[HeaderSize+i/8]|=uint8_t(1u<<(i%8));out[at++]=c.bytes[i];
      }
      put32(out+at,crc32(out,at));return at+4;
    }
    memset(out,0,RecordSize);out[0]='Z';out[1]='T';out[2]='X';out[3]=DenseVersion;
    put32(out+4,generation);
    out[8]=uint8_t(ExtendedModuleSettings::Size);out[9]=uint8_t(ExtendedModuleSettings::Size>>8);
    memcpy(out+HeaderSize,c.bytes,ExtendedModuleSettings::Size);
    put32(out+RecordSize-4,crc32(out,RecordSize-4));return RecordSize;
  }
  static bool decode(const uint8_t *p,int n,ExtendedModuleSettings &c,uint32_t &generation){
    if(n<int(V2FixedSize)||n>int(RecordSize)||p[0]!='Z'||p[1]!='T'||p[2]!='X'||
       get16(p+8)!=ExtendedModuleSettings::Size)return false;
    ExtendedModuleSettings candidate;uint32_t candidateGeneration=get32(p+4);
    if(p[3]==DenseVersion){
      if(n!=int(RecordSize)||p[10]||p[11]||
         get32(p+RecordSize-4)!=crc32(p,RecordSize-4))return false;
      memcpy(candidate.bytes,p+HeaderSize,ExtendedModuleSettings::Size);
    }else if(p[3]==SparseVersion){
      const size_t packed=get16(p+10);
      if(size_t(n)>=RecordSize||V2FixedSize+packed+4!=size_t(n)||
         get32(p+n-4)!=crc32(p,size_t(n)-4))return false;
      memset(candidate.bytes,0,ExtendedModuleSettings::Size);size_t at=V2FixedSize;
      for(size_t i=0;i<ExtendedModuleSettings::Size;++i){
        if(p[HeaderSize+i/8]&(1u<<(i%8))){
          if(at>=V2FixedSize+packed||p[at]==0)return false;
          candidate.bytes[i]=p[at++];
        }
      }
      if(at!=V2FixedSize+packed)return false;
    }else return false;
    if(!candidate.loadStored())return false;
    c=candidate;generation=candidateGeneration;return true;
  }
public:
  const ExtendedModuleSettings &config()const{return config_;}
  State state()const{return state_;}
  uint32_t generation()const{return generation_;}
  int activeSlot()const{return active_;}
  bool ready()const{return state_==State::Fresh||state_==State::Loaded||state_==State::Recovered;}
  int load(SettingsBackend &backend){
    config_=ExtendedModuleSettings{};generation_=0;active_=-1;state_=State::Unloaded;
    uint8_t raw[RecordSize];ExtendedModuleSettings candidates[2];uint32_t generations[2]{};bool good[2]{},present[2]{};
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
        // order. Do not silently choose activity from an ambiguous state.
        if((!delta&&memcmp(candidates[0].bytes,candidates[1].bytes,ExtendedModuleSettings::Size)!=0)||
           delta==0x80000000u){state_=State::Invalid;return -EINVAL;}
        slot=delta&&delta<0x80000000u?1:0;
      }
      config_=candidates[slot];generation_=generations[slot];active_=int8_t(slot);
      state_=(present[0]&&!good[0])||(present[1]&&!good[1])?State::Recovered:State::Loaded;return 0;
    }
    if(!present[0]&&!present[1]&&ExtendedModuleSettings{}.valid()){state_=State::Fresh;return 0;}
    state_=State::Invalid;return -EINVAL;
  }
  int save(SettingsBackend &backend,const ExtendedModuleSettings &candidate){
    if(!ready())return -EACCES;
    if(!candidate.valid())return -EINVAL;
    if(state_==State::Loaded&&memcmp(config_.bytes,candidate.bytes,ExtendedModuleSettings::Size)==0)return 0;
    const unsigned slot=active_==0?1:0;
    const uint32_t next=generation_+1;
    uint8_t record[RecordSize],readback[RecordSize];const size_t recordSize=encode(candidate,next,record);
    const int ret=backend.write(key(slot),record,recordSize);
    if(ret){
      // A write error is not proof that nothing committed. Do not later elide
      // a save of the stale RAM config and acknowledge it as durable while the
      // backend could contain the new generation.
      state_=State::IoError;return ret;
    }
    const int n=backend.read(key(slot),readback,sizeof(readback));
    if(n!=int(recordSize)||memcmp(record,readback,recordSize)){
      // Commit outcome is uncertain. Retain the old RAM configuration, but
      // require an explicit reload/reboot before any further settings writes.
      state_=State::IoError;return n<0?n:-EIO;
    }
    config_=candidate;generation_=next;active_=int8_t(slot);state_=State::Loaded;return 0;
  }
};
}
}
