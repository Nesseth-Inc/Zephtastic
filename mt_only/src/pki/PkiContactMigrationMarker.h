// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiIdentity.h"
#include "AppendStore.h"
#include <string.h>

namespace mt::pki {
// A small replacement for the redundant NVS append selector during explicit
// contact migration. The complete original baseline and history stay in their
// immutable flash region. This codec performs no storage operation.
class PkiContactMigrationMarker {
public:
  static constexpr size_t RecordBytes=64;
  static bool encode(const IdentityBinding &local,const AppendAnchor &root,
                     uint8_t *out,size_t size){
    if(!out||size!=RecordBytes||!local.node||!local.generation||
       !root.generation||root.capacity!=172032)return false;
    memset(out,0,size);memcpy(out,"ZTCM",4);out[4]=1;
    put(out+8,local.generation);put(out+12,local.node);
    memcpy(out+16,local.publicKey,32);put(out+48,root.generation);
    put(out+52,root.snapshotCrc);put(out+56,root.rootCrc);
    put(out+60,crc(out,60));return true;
  }
  static bool matches(const uint8_t *record,size_t size,
                      const IdentityBinding &local,const AppendAnchor &root){
    uint8_t expected[RecordBytes];
    return record&&encode(local,root,expected,sizeof(expected))&&
      size==sizeof(expected)&&!memcmp(record,expected,sizeof(expected));
  }
private:
  static void put(uint8_t *p,uint32_t value){
    for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));
  }
  static uint32_t crc(const uint8_t *p,size_t size){
    uint32_t value=~0u;
    for(size_t i=0;i<size;++i){value^=p[i];
      for(unsigned bit=0;bit<8;++bit)value=(value>>1)^((value&1)?0xedb88320u:0u);
    }
    return ~value;
  }
};
}
