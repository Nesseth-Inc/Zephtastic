// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "NodeCache.h"
#include "meshtastic/mesh.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include <stdio.h>

namespace mt {
// One pinned protobuf-to-cache conversion for both endpoint decisions and
// per-client delivery. A parseable routing envelope alone is not admission.
struct NodeUser {
  template<typename Enum> static uint32_t enumValue(const Enum &value) {
    // Nanopb is C code and may decode future/invalid enum integers. Inspect
    // their representation without an undefined C++ out-of-range enum load.
    __underlying_type(Enum) raw;
    memcpy(&raw,&value,sizeof(raw));return uint32_t(raw);
  }
  // Match 2.8.2's local presentation of NODEINFO. Valid IDs retain the original
  // serialized bytes, including future protobuf fields.
  static bool coerceId(meshtastic_Data &data,uint32_t source) {
    if(data.portnum!=meshtastic_PortNum_NODEINFO_APP)return false;
    meshtastic_User user{};
    auto input=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
    if(!pb_decode(&input,meshtastic_User_fields,&user))return false;
    char expected[sizeof(user.id)]{};
    snprintf(expected,sizeof(expected),"!%08x",source);
    if(!strcmp(expected,user.id))return false;
    memcpy(user.id,expected,sizeof(user.id));
    static_assert(meshtastic_User_size<=sizeof(data.payload.bytes));
    auto output=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
    if(!pb_encode(&output,meshtastic_User_fields,&user))return false;
    data.payload.size=output.bytes_written;return true;
  }
  static bool decode(NodeCache::Entry &entry,uint32_t from,const uint8_t *payload,size_t length) {
    meshtastic_User user{};auto stream=pb_istream_from_buffer(payload,length);
    if(!pb_decode(&stream,meshtastic_User_fields,&user))return false;
    using E=NodeCache::Entry;entry={};entry.identity(from,enumValue(user.hw_model),enumValue(user.role));
    if(!E::text(entry.bytes+E::Name,40,user.long_name,sizeof(user.long_name))||
       !E::text(entry.bytes+E::Short,5,user.short_name,sizeof(user.short_name))||user.public_key.size>32)return false;
    entry.bytes[E::KeyLength]=uint8_t(user.public_key.size);
    memcpy(entry.bytes+E::Key,user.public_key.bytes,user.public_key.size);
    entry.bytes[E::Flags]=uint8_t((user.has_is_unmessagable?1:0)|(user.is_unmessagable?2:0)|
      (user.is_licensed?4:0));
    return true;
  }
  static int accept(NodeCache &cache,uint32_t self,uint32_t from,const uint8_t *payload,
                    size_t length,int8_t snr,uint8_t hops) {
    NodeCache::Entry entry;
    return decode(entry,from,payload,length)?cache.accept(entry,self,snr,hops):-EINVAL;
  }
  static int permits(const NodeCache &cache,uint32_t self,uint32_t from,const uint8_t *payload,
                     size_t length,uint8_t hops) {
    NodeCache::Entry entry;
    return decode(entry,from,payload,length)?cache.permits(entry,self,hops):-EINVAL;
  }
  static bool accepted(const NodeCache &cache,uint32_t from,const uint8_t *payload,size_t length) {
    if(!cache.ready())return false;
    NodeCache::Entry entry;const int slot=cache.find(from);
    return slot>=0&&decode(entry,from,payload,length)&&
      // Local favorite/ignore metadata is not part of received User identity.
      !memcmp(entry.bytes,cache.get(unsigned(slot))->bytes,NodeCache::Entry::LocalFlags);
  }
};
}
