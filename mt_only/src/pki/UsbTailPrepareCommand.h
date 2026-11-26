// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "UsbPeerCommand.h"
namespace mt::pki {
// Exact114B: prefix23, node8, space, pub64, space, generation8, space, crc8.
// No caller-supplied digest, address, length, force or retry flag.
struct UsbTailPrepareCommand {
  uint32_t localNode=0;uint8_t localPublic[32]{};uint32_t generation=0,crc=0;
};
struct UsbTailPrepareParser {
  static bool parse(const uint8_t *p,size_t n,UsbTailPrepareCommand &out){
    constexpr char prefix[]="zt_pki_tail_prepare_v1 ";
    if(!p||n!=114)return false;
    for(size_t i=0;i<sizeof(prefix)-1;++i)if(p[i]!=uint8_t(prefix[i]))return false;
    UsbTailPrepareCommand value;
    if(p[31]!=' '||p[96]!=' '||p[105]!=' '||!detail::decodeNode(p,23,value.localNode)||
       !detail::decodePub(p,32,value.localPublic)||!detail::decodeNode(p,97,value.generation)||
       !detail::decodeNode(p,106,value.crc)||value.localNode<detail::kNodeMin||
       value.localNode>detail::kNodeMax||!value.generation)return false;
    out=value;return true;
  }
};
}
