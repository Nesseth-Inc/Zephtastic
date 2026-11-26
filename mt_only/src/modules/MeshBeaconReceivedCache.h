// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "SameChannelBeaconPayloads.h"

namespace mt { namespace beacon {

// Volatile advisory offers only. The stored protobuf may contain an offered
// channel PSK, just as stock's listener cache does; it is never persisted or
// interpreted as an authenticated identity. The original packet remains the
// phone-forwarded object. Two slots bound T096 storage to 576 bytes.
class MeshBeaconReceivedCache {
 public:
  static constexpr size_t Capacity=2, MaxPayload=233, MaxKey=32;
  static constexpr uint64_t TtlMs=21600000ULL;
  struct Entry {
    uint64_t observedMs=0;
    uint32_t source=0;
    uint16_t payloadLength=0;
    uint8_t channel=0, keyLength=0;
    uint8_t key[MaxKey]{};
    uint8_t payload[MaxPayload]{};
  };
  bool remember(uint32_t source,uint8_t channel,const uint8_t *key,size_t keyLength,
                const uint8_t *payload,size_t payloadLength,uint64_t observedMs,uint64_t nowMs) {
    if(source<=1||source>=0xfffffffeu||channel>=8||!validKey(key,keyLength)||
       !payload||payloadLength<1||payloadLength>MaxPayload||observedMs>nowMs||
       !canonicalPayload(payload,payloadLength))return false;
    // Stock retains only the offer in its advisory cache. Text remains in the
    // original port37 packet for phone forwarding, never duplicated here.
    meshtastic_MeshBeacon offer{};auto input=pb_istream_from_buffer(payload,payloadLength);
    if(!pb_decode(&input,meshtastic_MeshBeacon_fields,&offer)||input.bytes_left)return false;
    uint8_t advisory[MaxPayload]{};
    auto encoded=pb_ostream_from_buffer(advisory,sizeof(advisory));
    if(!stock::MeshBeaconListenerModule::handleReceivedProtobuf(&offer,[&](const auto &received) {
      return pb_encode(&encoded,meshtastic_MeshBeacon_fields,&received)&&encoded.bytes_written;
    }))return false;
    const size_t advisoryLength=encoded.bytes_written;
    size_t slot=find(source,channel,key,keyLength);
    if(slot==Capacity){for(size_t i=0;i<Capacity;++i)if(!entries_[i].source||expired(entries_[i].observedMs,nowMs)){slot=i;break;}}
    if(slot==Capacity)return false;
    Entry &entry=entries_[slot];clearEntry(entry);entry.source=source;entry.channel=channel;
    entry.keyLength=uint8_t(keyLength);memcpy(entry.key,key,keyLength);
    entry.observedMs=observedMs;entry.payloadLength=uint16_t(advisoryLength);
    memcpy(entry.payload,advisory,advisoryLength);return true;
  }
  bool copy(uint32_t source,uint8_t channel,const uint8_t *key,size_t keyLength,uint64_t nowMs,
            uint8_t *out,size_t capacity,size_t *outLength)const {
    if(!validKey(key,keyLength)||!out||!outLength)return false;
    const size_t slot=find(source,channel,key,keyLength);if(slot==Capacity)return false;
    const Entry &e=entries_[slot];if(nowMs<e.observedMs||expired(e.observedMs,nowMs)||capacity<e.payloadLength)return false;
    memcpy(out,e.payload,e.payloadLength);*outLength=e.payloadLength;return true;
  }
  bool forget(uint32_t source,uint8_t channel,const uint8_t *key,size_t keyLength) {
    if(!validKey(key,keyLength))return false;const size_t slot=find(source,channel,key,keyLength);
    if(slot==Capacity)return false;clearEntry(entries_[slot]);return true;
  }
  bool forgetSource(uint32_t source){
    bool removed=false;for(auto &entry:entries_)if(entry.source==source){clearEntry(entry);removed=true;}return removed;
  }
  void clear(){for(auto &e:entries_)clearEntry(e);}
 private:
  Entry entries_[Capacity]{};
  static bool validKey(const uint8_t *key,size_t n){return key&&(n==1||n==16||n==32);}
  static bool expired(uint64_t observed,uint64_t now){return now<observed||now-observed>TtlMs;}
  size_t find(uint32_t source,uint8_t channel,const uint8_t *key,size_t n)const {
    for(size_t i=0;i<Capacity;++i){const Entry &e=entries_[i];if(e.source==source&&e.channel==channel&&e.keyLength==n&&!memcmp(e.key,key,n))return i;}return Capacity;
  }
  static void clearEntry(Entry &e){memset(&e,0,sizeof(e));}
};
static_assert(sizeof(MeshBeaconReceivedCache)<=576,"received beacon cache exceeds T096 RAM budget");

}} // namespace mt::beacon
