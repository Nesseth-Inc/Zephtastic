// SPDX-License-Identifier: GPL-3.0-only
// Exact USB command for explicit compact-contact lifecycle enable.
#pragma once

#include <cstddef>
#include <cstdint>

#include "UsbStorageCommand.h"

namespace mt::pki {
namespace detail {
constexpr char kContactsEnablePrefix[] = "zt_pki_contacts_enable_v1 ";
constexpr size_t kContactsEnablePrefixLen = sizeof(kContactsEnablePrefix) - 1;
constexpr size_t kContactsEnableNodeOff = kContactsEnablePrefixLen;
constexpr size_t kContactsEnablePubOff = kContactsEnableNodeOff + detail::kNodeHexLen + 1;
constexpr size_t kContactsEnableFrameSize = kContactsEnablePubOff + detail::kPubHexLen;
static_assert(kContactsEnableFrameSize == sizeof(kContactsEnablePrefix) - 1 + 8 + 1 + 64,
              "Contact-enable frame is exact prefix plus node and key");
}  // namespace detail

struct UsbContactsParser {
  static bool parse(const uint8_t *bytes,size_t size,UsbStorageCommand &out) {
    if(!bytes||size!=detail::kContactsEnableFrameSize)return false;
    for(size_t i=0;i<detail::kContactsEnablePrefixLen;++i)
      if(bytes[i]!=static_cast<uint8_t>(detail::kContactsEnablePrefix[i]))return false;
    uint32_t node=0;uint8_t publicKey[detail::kPubByteLen]{};
    if(!detail::decodeNode(bytes,detail::kContactsEnableNodeOff,node)||
       bytes[detail::kContactsEnableNodeOff+detail::kNodeHexLen]!=' '||
       !detail::decodePub(bytes,detail::kContactsEnablePubOff,publicKey)||
       node<detail::kNodeMin||node>detail::kNodeMax)return false;
    out.localNode=node;
    for(size_t i=0;i<detail::kPubByteLen;++i)out.localPublic[i]=publicKey[i];
    return true;
  }
};
}  // namespace mt::pki
