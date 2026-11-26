// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../../stock/peripherals/AudioModule.h"
#include <cstddef>
#include <cstdint>
#include <string.h>

namespace mt::audio {
// Meshtastic AudioModule puts its codec mode (Codec2 mode value, not the
// protobuf enum) after C0 DE C2. Frame sizes below are bits_per_frame rounded
// up to whole bytes from the pinned Meshtastic Codec2 revision in Codec2.lock.
// Deprecated 700/700B are remapped to 700C by AudioConfig.
struct AudioWire : stock::audio::Frames {
  static constexpr uint8_t frameBytes(uint8_t mode){
    return mode==0?8:mode==1?6:mode==2?8:mode==3?7:mode==4?7:mode==5?6:mode==8?4:mode==9?3:0;
  }
  static bool validPayload(const uint8_t *p,size_t n){
    if(!p||n<=HeaderSize||n>MaxPayload||p[0]!=Magic[0]||p[1]!=Magic[1]||p[2]!=Magic[2])return false;
    const uint8_t bytes=frameBytes(p[3]);return bytes&&(n-HeaderSize)%bytes==0;
  }
};

// Generated-free strict wire check for the three uint32 fields in stock
// Paxcount. Require canonical field order, unique fields and minimal varints;
// all-default/empty messages are rejected just as nanopb encodes them empty.
struct PaxcountWire {
private:
  static bool varint(const uint8_t *p,size_t n,size_t &at,uint32_t &value){
    value=0;unsigned shift=0;size_t start=at;
    while(at<n&&shift<35){const uint8_t b=p[at++];if(shift==28&&(b&0xf0))return false;value|=uint32_t(b&0x7f)<<shift;
      if(!(b&0x80)){unsigned bytes=1;for(uint32_t v=value;v>=0x80;v>>=7)++bytes;return at-start==bytes;}
      shift+=7;
    }
    return false;
  }
  static size_t put(uint8_t *out,uint32_t value){size_t n=0;do{uint8_t b=uint8_t(value&0x7f);value>>=7;if(value)b|=0x80;out[n++]=b;}while(value);return n;}
public:
  static bool validPayload(const uint8_t *p,size_t n){
    if(!p||!n||n>18)return false;uint32_t values[3]{};bool present[3]{};size_t at=0;unsigned prior=0;
    while(at<n){uint32_t tag=0;if(!varint(p,n,at,tag)||(tag!=8&&tag!=16&&tag!=24))return false;
      const unsigned field=unsigned(tag/8);if(field<=prior||field>3)return false;prior=field;
      if(!varint(p,n,at,values[field-1])||!values[field-1])return false;present[field-1]=true;
    }
    uint8_t canonical[18]{};size_t used=0;for(unsigned i=0;i<3;++i)if(present[i]){canonical[used++]=uint8_t((i+1)*8);used+=put(canonical+used,values[i]);}
    return used==n&&!memcmp(canonical,p,n);
  }
};
}
