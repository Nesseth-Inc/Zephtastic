// SPDX-License-Identifier: GPL-3.0-only
// Literal pinned-stock stored-key/hash functions with bounded synthetic slots.
// No radio, durable nonce authority, or secondary-channel implementation claim.
#include "PrimaryChannel.hpp"
#include "meshtastic/channel.pb.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#define LOG_DEBUG(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define defaultpsk bench::MtNodeRecord::PublicKey
using ChannelIndex = unsigned;
struct CryptoKey {uint8_t bytes[32]{};int length=0;};
static uint8_t xorHash(const uint8_t *p,size_t n){uint8_t out=0;while(n--)out^=*p++;return out;}
struct Channels {
  meshtastic_Channel slots[8]{};
  unsigned primaryIndex=0;
  meshtastic_Channel &getByIndex(unsigned i){assert(i<8);return slots[i];}
  const char *getName(unsigned i){const auto &c=getByIndex(i);return c.settings.name[0]?c.settings.name:"LongFast";}
  CryptoKey getKey(ChannelIndex);
  int16_t generateHash(ChannelIndex);
};
#include "channel_key/stock_get_key.inc"
#include "channel_key/stock_generate_hash.inc"

static void compare(Channels &stock) {
  const auto key=stock.getKey(0);const auto hash=stock.generateHash(0);
  uint8_t normalized[32]{};size_t size=999;
  const auto &input=stock.slots[0].settings.psk;
  assert(mt::PskNormalizer::normalize(input.bytes,input.size,defaultpsk,sizeof(defaultpsk),normalized,sizeof(normalized),size));
  assert(size==size_t(key.length)&&!memcmp(normalized,key.bytes,32));
  mt::PrimaryChannel::Selection selection;
  const bool usable=key.length==16||key.length==32;
  assert(mt::PrimaryChannel::normalize(input.bytes,input.size,selection)==usable);
  if(usable) {
    assert(selection.length==size_t(key.length)&&selection.hash==hash&&!memcmp(selection.bytes,key.bytes,32));
    assert(selection.publicDefault==(key.length==16&&!memcmp(key.bytes,defaultpsk,16)));
  } else {
    assert(!selection.length&&!selection.hash&&!selection.publicDefault);
    for(const auto byte:selection.bytes)assert(!byte);
  }
  bench::wipeKeyBuffer(normalized,sizeof(normalized));mt::PrimaryChannel::clear(selection);
}

int main() {
  Channels stock;auto &c=stock.slots[0];c.has_settings=true;c.role=meshtastic_Channel_Role_PRIMARY;
  strcpy(c.settings.name,"LongFast");
  for(unsigned alias=0;alias<256;++alias){c.settings.psk={};c.settings.psk.size=1;c.settings.psk.bytes[0]=uint8_t(alias);compare(stock);}
  puts("PASS all256 stored aliases match literal stock expansion and hash; off and zero-hash remain explicit primary refusals");
  for(unsigned salt=0;salt<32;++salt)for(unsigned length=0;length<=32;++length) {
    c.settings.psk={};c.settings.psk.size=length;
    for(unsigned i=0;i<length;++i)c.settings.psk.bytes[i]=uint8_t(salt*11+i*17);
    compare(stock);
  }
  puts("PASS all wire key lengths0through32 across32 synthetic bodies match stock effective key padding and channel hash");
  for(unsigned length:{2u,15u,16u,17u,31u,32u}) {
    mt::PrimaryChannel::Selection selection;selection.length=32;
    for(unsigned i=0;i<32;++i)selection.bytes[i]=uint8_t(i+3);
    uint8_t original[32];memcpy(original,selection.bytes,32);
    assert(mt::PrimaryChannel::normalize(selection.bytes,length,selection));
    assert(selection.length==(length<=16?16u:32u)&&!memcmp(selection.bytes,original,length));
    for(size_t i=length;i<selection.length;++i)assert(!selection.bytes[i]);
    mt::PrimaryChannel::clear(selection);
  }
  puts("PASS in-place short and full raw-key normalization snapshots input before committing or clearing output");
  uint8_t out[32];memset(out,0xa5,32);size_t size=88;uint8_t tooLong[33]{};
  assert(!mt::PskNormalizer::normalize(tooLong,33,defaultpsk,16,out,32,size));
  assert(!mt::PskNormalizer::normalize(nullptr,2,defaultpsk,16,out,32,size));
  const uint8_t alias=7;
  assert(!mt::PskNormalizer::normalize(&alias,1,nullptr,16,out,32,size));
  assert(!mt::PskNormalizer::normalize(&alias,1,defaultpsk,15,out,32,size));
  assert(!mt::PskNormalizer::normalize(&alias,1,defaultpsk,16,out,31,size));
  assert(size==88);for(const auto byte:out)assert(byte==0xa5);
  puts("PASS invalid stored-key pointers lengths capacity and default-key contract preserve complete output and length");
}
