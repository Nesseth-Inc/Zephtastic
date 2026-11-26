// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "BluetoothSettings.h"
#include "SettingsJournal.h"

namespace mt {
// Dual-slot journal for the Bluetooth pairing-policy settings. Record layout,
// wrap-aware generation ordering, single-good-slot recovery, and
// write-poisoning semantics mirror SettingsJournal (M101) over the same
// SettingsBackend contract: read returns the full stored length, -ENOENT for
// absence, -E2BIG when the stored value exceeds capacity, and other negative
// errno values for I/O errors; write returns zero only after the backend's
// own commit has completed.
//
// Record (32 bytes, little-endian):
//   0..2   magic 'Z','T','B'
//   3      format version, currently 1
//   4..7   generation
//   8..11  reserved, zero
//   12..23 BluetoothSettings codec, exactly BluetoothSettings::Size bytes
//   24..27 reserved, zero
//   28..31 CRC-32 (IEEE 802.3) over bytes 0..27
//
// Both keys absent means Fresh with the canonical defaults (enabled, NoPin,
// fixedPin=0); a present-but-invalid record is never replaced by defaults.
// Bluetooth bonds and keys are not stored here; they remain Zephyr-owned.
class BluetoothJournal {
public:
  static constexpr size_t RecordSize=32;
  enum class State:uint8_t {Unloaded,Fresh,Loaded,Recovered,Invalid,IoError};
private:
  BluetoothSettings config_;
  uint32_t generation_=0;
  int8_t active_=-1;
  State state_=State::Unloaded;
  static const char *key(unsigned slot){return slot?"mtble/state1":"mtble/state0";}
  static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
  static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
  static uint32_t crc32(const uint8_t *p,size_t n){
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}
    return ~c;
  }
  static bool encode(const BluetoothSettings &c,uint32_t generation,uint8_t *out){
    memset(out,0,RecordSize);out[0]='Z';out[1]='T';out[2]='B';out[3]=1;
    put32(out+4,generation);
    size_t written=0;
    if(!BluetoothSettings::encode(c,out+12,BluetoothSettings::Size,written)||written!=BluetoothSettings::Size)return false;
    put32(out+RecordSize-4,crc32(out,RecordSize-4));return true;
  }
  static bool decode(const uint8_t *p,int n,BluetoothSettings &c,uint32_t &generation){
    if(n!=int(RecordSize))return false;
    if(p[0]!='Z'||p[1]!='T'||p[2]!='B'||p[3]!=1||p[8]||p[9]||p[10]||p[11]||
       p[24]||p[25]||p[26]||p[27]||get32(p+RecordSize-4)!=crc32(p,RecordSize-4))return false;
    BluetoothSettings candidate;
    if(!BluetoothSettings::decode(p+12,BluetoothSettings::Size,candidate))return false;
    // Capability-neutral: validate the stored value codec as-is.
    if(!BluetoothSettings::valid(candidate,true))return false;
    c=candidate;generation=get32(p+4);return true;
  }
public:
  const BluetoothSettings &config()const{return config_;}
  State state()const{return state_;}
  uint32_t generation()const{return generation_;}
  int activeSlot()const{return active_;}
  bool ready()const{return state_==State::Fresh||state_==State::Loaded||state_==State::Recovered;}
  int load(SettingsBackend &backend){
    config_=BluetoothSettings::defaults();generation_=0;active_=-1;state_=State::Unloaded;
    uint8_t raw[RecordSize];BluetoothSettings candidates[2];uint32_t generations[2]{};bool good[2]{},present[2]{};
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
        // order. Do not silently choose a pairing policy from an ambiguous state.
        if((!delta&&!candidates[0].same(candidates[1]))||delta==0x80000000u){state_=State::Invalid;return -EINVAL;}
        slot=delta&&delta<0x80000000u?1:0;
      }
      config_=candidates[slot];generation_=generations[slot];active_=int8_t(slot);
      state_=(present[0]&&!good[0])||(present[1]&&!good[1])?State::Recovered:State::Loaded;return 0;
    }
    if(!present[0]&&!present[1]){state_=State::Fresh;return 0;}
    state_=State::Invalid;return -EINVAL;
  }
  int save(SettingsBackend &backend,const BluetoothSettings &candidate,bool force=false){
    if(!ready())return -EACCES;
    if(!BluetoothSettings::valid(candidate,true))return -EINVAL;
    // The exact default config on a Fresh journal is already in effect; do
    // not burn a slot persisting what the device already does.
    if(!force&&state_==State::Fresh&&BluetoothSettings::defaults().same(candidate))return 0;
    if(!force&&active_>=0&&config_.same(candidate))return 0;
    const unsigned slot=active_==0?1:0;
    const uint32_t next=generation_+1;
    uint8_t record[RecordSize],readback[RecordSize];
    if(!encode(candidate,next,record))return -EINVAL;
    const int ret=backend.write(key(slot),record,sizeof(record));
    if(ret){
      // A write error is not proof that nothing committed. Enter IoError so a
      // later save of the stale RAM config is not elided and acknowledged as
      // durable while the backend could contain the new generation.
      state_=State::IoError;return ret;
    }
    const int n=backend.read(key(slot),readback,sizeof(readback));
    if(n!=int(sizeof(record))||memcmp(record,readback,sizeof(record))){
      // Commit outcome is uncertain. Retain the old RAM configuration, but
      // require an explicit reload before any further settings writes.
      state_=State::IoError;return n<0?n:-EIO;
    }
    config_=candidate;generation_=next;active_=int8_t(slot);state_=State::Loaded;return 0;
  }
};
}
