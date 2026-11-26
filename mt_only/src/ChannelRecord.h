// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <string.h>

namespace mt {
// Local persistent representation, never a stock protobuf or key authority.
// Raw zero-length secondary keys are resolved against the current primary.
struct ChannelRecord {
  static constexpr size_t Size=64;
  uint8_t bytes[Size]{};
  static uint16_t checksum(const uint8_t *p,size_t n) {
    uint16_t c=0xffff;
    for(size_t i=0;i<n;++i){c^=uint16_t(p[i])<<8;for(unsigned j=0;j<8;++j)c=c&0x8000?uint16_t((c<<1)^0x1021):uint16_t(c<<1);}
    return c;
  }
  void seal(){const auto c=checksum(bytes,62);bytes[62]=uint8_t(c);bytes[63]=uint8_t(c>>8);}
  ChannelRecord(){seal();}
  static bool canonical(const uint8_t *p,size_t n) {
    if(!p||n!=Size||p[0]>2||p[1]>32||(p[2]&~15u)||p[3]>32)return false;
    bool end=false;
    for(unsigned i=4;i<16;++i){if(!p[i])end=true;else if(end||p[i]<0x20||p[i]>0x7e)return false;}
    if(!end)return false;
    for(unsigned i=16+p[1];i<62;++i)if(p[i])return false;
    return (uint16_t(p[62])|(uint16_t(p[63])<<8))==checksum(p,62);
  }
  bool valid()const{return canonical(bytes,Size);}
  unsigned role()const{return bytes[0];}
  size_t keySize()const{return bytes[1];}
  const uint8_t *key()const{return bytes+16;}
  const char *name()const{return reinterpret_cast<const char *>(bytes+4);}
  bool muted()const{return bytes[2]&4;}
  bool uplink()const{return bytes[2]&1;}
  bool downlink()const{return bytes[2]&2;}
  bool aead()const{return (bytes[2]&8u)!=0;}
  unsigned precision()const{return bytes[3];}
  bool set(unsigned role,const char *name,const uint8_t *key,size_t n,
           unsigned precision=0,bool muted=false,bool uplink=false,bool downlink=false,bool aead=false) {
    if(role>2||(aead&&role==0)||!name||n>32||(n&&!key)||precision>32)return false;
    size_t len=0;while(len<12&&name[len]){if(uint8_t(name[len])<0x20||uint8_t(name[len])>0x7e)return false;++len;}
    if(len>=12)return false;
    ChannelRecord candidate;candidate.bytes[0]=uint8_t(role);candidate.bytes[1]=uint8_t(n);
    candidate.bytes[2]=uint8_t((muted?4:0)|(uplink?1:0)|(downlink?2:0)|(aead?8:0));candidate.bytes[3]=uint8_t(precision);
    memcpy(candidate.bytes+4,name,len);if(n)memcpy(candidate.bytes+16,key,n);candidate.seal();*this=candidate;return true;
  }
};
static_assert(sizeof(ChannelRecord)==ChannelRecord::Size);
}
