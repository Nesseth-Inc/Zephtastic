// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "AppendStore.h"
#include <monocypher.h>
#include <errno.h>

namespace mt::pki {
struct TailProbeResult {
  uint8_t digest[32]{};uint32_t nonFF=0;bool blank=false,possibleJournal=false;
};
// Classification only: possibleJournal=false never proves absence of history
// and grants no permission to erase, initialize or repair this region.
class LegacyTailProbe {
  static uint32_t word(const uint8_t *p){
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
  }
public:
  static constexpr size_t Bytes=172032;
  static int inspect(AppendStore &store,TailProbeResult &out){
    struct Scratch {
      crypto_blake2b_ctx hash{};uint8_t bytes[64]{};TailProbeResult result;
      ~Scratch(){crypto_wipe(this,sizeof(*this));}
    } scratch;
    if(store.capacity()!=Bytes)return -ENOTSUP;
    crypto_blake2b_init(&scratch.hash,32);
    for(size_t offset=0;offset<Bytes;offset+=64){
      const int rc=store.read(offset,scratch.bytes,64);if(rc)return rc<0?rc:-EIO;
      const auto *b=scratch.bytes;crypto_blake2b_update(&scratch.hash,b,64);
      for(size_t i=0;i<64;++i)if(b[i]!=0xff)++scratch.result.nonFF;
      if(!offset){
        constexpr uint8_t magic[4]={'Z','T','A','H'};bool compatible=true,nonblank=false;
        for(size_t i=0;i<4;++i){if((b[i]&magic[i])!=magic[i])compatible=false;if(b[i]!=0xff)nonblank=true;}
        if(compatible&&nonblank)scratch.result.possibleJournal=true;
      }
      if(offset==4032&&word(b+60)==0x54494d43u)scratch.result.possibleJournal=true;
      if(offset>=4096){
        const uint32_t sequence=uint32_t((offset-4096)/64)+1;
        const bool header=b[0]>=1&&b[0]<=4&&b[1]==1&&!b[2]&&!b[3]&&word(b+4)==sequence;
        if(header||word(b+60)==0x54494d43u)scratch.result.possibleJournal=true;
      }
    }
    crypto_blake2b_final(&scratch.hash,scratch.result.digest);
    scratch.result.blank=scratch.result.nonFF==0;out=scratch.result;return 0;
  }
};
}
