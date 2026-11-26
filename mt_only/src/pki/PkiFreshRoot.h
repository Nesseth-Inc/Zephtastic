// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiIdentity.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::pki {
struct PkiFreshRoot {
  static constexpr size_t RecordBytes=56;
  static constexpr uint32_t Magic=0x4446545au; // "ZTFD"
  static constexpr uint16_t Version=1;
  uint32_t generation=1,identityGeneration=0,node=0,crc=0;
  uint8_t publicKey[32]{};
  static uint32_t checksum(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;++i){c^=p[i];for(unsigned b=0;b<8;++b)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}return ~c;}
  static uint32_t word(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
  static void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
  static bool encode(const IdentityBinding &b,uint8_t out[RecordBytes],PkiFreshRoot *decoded=nullptr){
    if(!out||b.version!=1||b.algorithm!=1||b.generation!=1||b.node<=1||b.node>=0xfffffffeu)return false;
    memset(out,0,RecordBytes);put(out,Magic);out[4]=uint8_t(Version);
    put(out+8,1);put(out+12,b.generation);put(out+16,b.node);memcpy(out+20,b.publicKey,32);put(out+52,checksum(out,52));
    if(decoded){decoded->generation=1;decoded->identityGeneration=b.generation;decoded->node=b.node;
      memcpy(decoded->publicKey,b.publicKey,32);decoded->crc=word(out+52);}return true;
  }
  static bool decode(const uint8_t *in,size_t n,PkiFreshRoot &out){
    if(!in||n!=RecordBytes||word(in)!=Magic||in[4]!=Version||in[5]||in[6]||in[7]||word(in+8)!=1||
       word(in+12)!=1||word(in+16)<=1||word(in+16)>=0xfffffffeu||word(in+52)!=checksum(in,52))return false;
    memcpy(out.publicKey,in+20,32);out.generation=1;out.identityGeneration=word(in+12);out.node=word(in+16);out.crc=word(in+52);return true;
  }
  bool matches(const IdentityBinding &b)const{return b.generation==identityGeneration&&b.node==node&&!memcmp(b.publicKey,publicKey,32);}
};
}
