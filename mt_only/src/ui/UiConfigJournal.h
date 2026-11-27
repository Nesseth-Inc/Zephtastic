// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "UiConfigSettings.h"
#include "../SettingsJournal.h"
#include <errno.h>

namespace mt::ui {

// Two-slot journal for the stock DeviceUIConfig protobuf. It touches only
// mtui/state0 and mtui/state1; legacy preference storage is read-only input
// supplied by the owner and is never overwritten or erased here.
class UiConfigJournal {
public:
  static constexpr size_t HeaderSize=12;
  static constexpr size_t RecordCapacity=HeaderSize+UiConfigSettings::WireCapacity+4;
  enum class State:uint8_t {Unloaded,Fresh,Legacy,Loaded,Recovered,Invalid,IoError};

private:
  meshtastic_DeviceUIConfig current_=UiConfigSettings::defaults();
  uint32_t generation_=0;
  int active_=-1;
  State state_=State::Unloaded;

  static const char *key(unsigned slot) {
    return slot?"mtui/state1":"mtui/state0";
  }
  static uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
  }
  static void put32(uint8_t *p,uint32_t value) {
    for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));
  }
  static uint16_t get16(const uint8_t *p) {
    return uint16_t(p[0])|uint16_t(uint16_t(p[1])<<8);
  }
  static void put16(uint8_t *p,uint16_t value) {
    p[0]=uint8_t(value);p[1]=uint8_t(value>>8);
  }
  static uint32_t crc32(const uint8_t *p,size_t length) {
    uint32_t crc=UINT32_MAX;
    for(size_t i=0;i<length;++i) {
      crc^=p[i];
      for(unsigned bit=0;bit<8;++bit)
        crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));
    }
    return ~crc;
  }
  static size_t encodeRecord(const meshtastic_DeviceUIConfig &value,
                             uint32_t generation,uint8_t *out) {
    memset(out,0,RecordCapacity);
    size_t wireLength=0;
    if(!UiConfigSettings::encode(value,out+HeaderSize,UiConfigSettings::WireCapacity,wireLength))
      return 0;
    out[0]='Z';out[1]='U';out[2]='J';out[3]=1;
    put32(out+4,generation);
    put16(out+8,uint16_t(wireLength));
    const size_t recordLength=HeaderSize+wireLength+4;
    put32(out+recordLength-4,crc32(out,recordLength-4));
    return recordLength;
  }
  static bool decodeRecord(const uint8_t *raw,int length,
                           meshtastic_DeviceUIConfig &value,uint32_t &generation) {
    if(!raw||length<int(HeaderSize+4)||length>int(RecordCapacity)||
       raw[0]!='Z'||raw[1]!='U'||raw[2]!='J'||raw[3]!=1||raw[10]||raw[11])
      return false;
    const size_t wireLength=get16(raw+8);
    if(wireLength>UiConfigSettings::WireCapacity||
       HeaderSize+wireLength+4!=size_t(length)||
       get32(raw+length-4)!=crc32(raw,size_t(length)-4))
      return false;
    meshtastic_DeviceUIConfig candidate{};
    UiConfigSensitiveWipe wipeCandidate{&candidate,sizeof(candidate)};
    if(!UiConfigSettings::decodeCanonical(raw+HeaderSize,wireLength,candidate))return false;
    generation=get32(raw+4);
    value=candidate;
    return true;
  }

public:
  const meshtastic_DeviceUIConfig &config()const{return current_;}
  State state()const{return state_;}
  uint32_t generation()const{return generation_;}
  int activeSlot()const{return active_;}
  bool ready()const {
    return state_==State::Fresh||state_==State::Legacy||
      state_==State::Loaded||state_==State::Recovered;
  }

  // If a pre-existing owner has already loaded the legacy preference file,
  // pass it here. It remains read-only; the first later save goes to mtui/*.
  int load(SettingsBackend &backend,const meshtastic_DeviceUIConfig *legacy=nullptr) {
    current_=UiConfigSettings::defaults();generation_=0;active_=-1;state_=State::Unloaded;
    bool good[2]{},present[2]{};
    meshtastic_DeviceUIConfig values[2]{};
    UiConfigSensitiveWipe wipeValues{values,sizeof(values)};
    uint32_t generations[2]{};
    uint8_t raw[RecordCapacity]{};
    UiConfigSensitiveWipe wipeRaw{raw,sizeof(raw)};
    for(unsigned slot=0;slot<2;++slot) {
      const int count=backend.read(key(slot),raw,sizeof(raw));
      if(count<0&&count!=-ENOENT&&count!=-E2BIG){state_=State::IoError;return count;}
      present[slot]=count!=-ENOENT;
      good[slot]=decodeRecord(raw,count,values[slot],generations[slot]);
    }

    if(good[0]||good[1]) {
      unsigned selected=good[1]?1:0;
      if(good[0]&&good[1]) {
        const uint32_t delta=generations[1]-generations[0];
        if((!delta&&!UiConfigSettings::same(values[0],values[1]))||
           delta==0x80000000u) {
          state_=State::Invalid;
          return -EINVAL;
        }
        selected=delta&&delta<0x80000000u?1:0;
      }
      current_=values[selected];
      generation_=generations[selected];
      active_=int(selected);
      state_=(present[0]&&!good[0])||(present[1]&&!good[1])?
        State::Recovered:State::Loaded;
      return 0;
    }

    if(present[0]||present[1]) {
      state_=State::Invalid;
      return -EINVAL;
    }
    if(legacy) {
      if(!UiConfigSettings::valid(*legacy)){state_=State::Invalid;return -EINVAL;}
      current_=*legacy;
      state_=State::Legacy;
      return 0;
    }
    state_=State::Fresh;
    return 0;
  }

  int save(SettingsBackend &backend,const meshtastic_DeviceUIConfig &candidate) {
    if(!ready())return -EACCES;
    if(!UiConfigSettings::valid(candidate))return -EINVAL;
    if(state_==State::Loaded&&UiConfigSettings::same(current_,candidate))return 0;

    uint8_t record[RecordCapacity]{},readback[RecordCapacity]{};
    UiConfigSensitiveWipe wipeRecord{record,sizeof(record)},wipeReadback{readback,sizeof(readback)};
    const uint32_t next=generation_+1;
    const size_t length=encodeRecord(candidate,next,record);
    if(!length){state_=State::Invalid;return -EINVAL;}
    const unsigned slot=active_==0?1:0;
    const int result = backend.writeVerified(key(slot), record, length, readback, sizeof(readback));
    if (result) {
      state_ = State::IoError;
      return result;
    }
    current_=candidate;
    generation_=next;
    active_=int(slot);
    state_=State::Loaded;
    return 0;
  }
};

} // namespace mt::ui

