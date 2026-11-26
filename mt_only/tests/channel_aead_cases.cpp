// SPDX-License-Identifier: GPL-3.0-only
// Channel CCM unit/integration tests against the exact pinned stock CCM source.
#include "Config.h"
#include "MtChannelAead.h"
#include "MtCodec.h"
#include "CryptoEngine.h" // Small AES-block adapter supplied by test-channel-aead.py.
#include "aes-ccm.h"     // Exact pinned stock aes-ccm.cpp, compiled from a verified copy.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

CryptoEngine stockCrypto;
CryptoEngine *crypto = &stockCrypto;

void CryptoEngine::aesSetKey(const uint8_t *key, size_t keyLen) {
  assert(key && (keyLen == 16 || keyLen == 32));
  memset(key_, 0, sizeof(key_)); memcpy(key_, key, keyLen); keyLen_ = keyLen;
}
void CryptoEngine::aesEncrypt(uint8_t *input, uint8_t *output) {
  uint8_t block[16]{};
  assert(bench::mtAesCtr(nullptr, key_, keyLen_, input, block, sizeof(block)));
  memcpy(output, block, sizeof(block));
  volatile uint8_t *wipe = block; for (size_t i = 0; i < sizeof(block); ++i) wipe[i] = 0;
}

namespace {
unsigned groups = 0;
void pass(const char *name) { ++groups; std::printf("PASS %s\n",name); }
void le32(uint8_t *p, uint32_t x) { for (unsigned i=0;i<4;++i) p[i]=uint8_t(x>>(8*i)); }
struct Fault {
  size_t calls=0, failAt=0;
};
bool failingBlock(void *context,const uint8_t *key,size_t keyLen,const uint8_t nonce[16],uint8_t *bytes,size_t len) {
  auto &state=*static_cast<Fault *>(context);++state.calls;
  if(state.failAt&&state.calls==state.failAt){memset(bytes,0xD7,len);return false;}
  return bench::mtAesCtr(nullptr,key,keyLen,nonce,bytes,len);
}
bool allByte(const uint8_t *p,size_t n,uint8_t value) {
  for(size_t i=0;i<n;++i) {
    if(p[i]!=value)return false;
  }
  return true;
}
void payload(uint8_t *p,size_t n) { for(size_t i=0;i<n;++i)p[i]=uint8_t((i*29u+n*7u+3u)&0xffu); }
void stockNonceAad(uint32_t from,uint32_t to,uint32_t id,uint8_t nonce[16],uint8_t aad[8]) {
  memset(nonce,0,16);le32(nonce,id);le32(nonce+8,from);le32(aad,from);le32(aad+4,to);
}
void exactStockVector(size_t keyLen,size_t plainLen,uint32_t from=0x12345678,
                      uint32_t to=0x87654321,uint32_t id=0x3456789a) {
  uint8_t key[32]{},plain[bench::ChannelAead::MaxPlain]{},expectedCipher[bench::ChannelAead::MaxPlain]{},expectedTag[12]{};
  for(size_t i=0;i<keyLen;++i)key[i]=uint8_t(0x91u+i*7u);
  payload(plain,plainLen);
  uint8_t nonce[16]{},aad[8]{};stockNonceAad(from,to,id,nonce,aad);
  assert(aes_ccm_ae(key,keyLen,nonce,bench::ChannelAead::TagSize,plain,plainLen,aad,sizeof(aad),expectedCipher,expectedTag)==0);
  uint8_t actual[bench::ChannelAead::MaxWire+1]{};size_t actualLen=999;
  assert(bench::ChannelAead::encrypt(bench::mtAesCtr,nullptr,key,keyLen,from,to,id,plain,plainLen,
       actual,sizeof(actual),actualLen)==bench::ChannelAeadResult::Success);
  assert(actualLen==plainLen+12&&!memcmp(actual,expectedCipher,plainLen)&&!memcmp(actual+plainLen,expectedTag,12));
  uint8_t decrypted[bench::ChannelAead::MaxPlain+1];memset(decrypted,0xA5,sizeof(decrypted));size_t decryptedLen=999;
  assert(aes_ccm_ad(key,keyLen,nonce,12,expectedCipher,plainLen,aad,sizeof(aad),expectedTag,plain));
  assert(bench::ChannelAead::decrypt(bench::mtAesCtr,nullptr,key,keyLen,from,to,id,actual,actualLen,
       decrypted,sizeof(decrypted),decryptedLen)==bench::ChannelAeadResult::Success);
  assert(decryptedLen==plainLen&&!memcmp(decrypted,plain,plainLen)&&decrypted[plainLen]==0xA5);
  volatile uint8_t *wipe=key;for(size_t i=0;i<sizeof(key);++i)wipe[i]=0;
  wipe=plain;for(size_t i=0;i<sizeof(plain);++i)wipe[i]=0;
  wipe=actual;for(size_t i=0;i<sizeof(actual);++i)wipe[i]=0;
  wipe=decrypted;for(size_t i=0;i<sizeof(decrypted);++i)wipe[i]=0;
  wipe=expectedTag;for(size_t i=0;i<sizeof(expectedTag);++i)wipe[i]=0;
  wipe=expectedCipher;for(size_t i=0;i<sizeof(expectedCipher);++i)wipe[i]=0;
  wipe=nonce;for(size_t i=0;i<sizeof(nonce);++i)wipe[i]=0;
  wipe=aad;for(size_t i=0;i<sizeof(aad);++i)wipe[i]=0;
}
}

int main() {
  std::puts("M187 channel AEAD tests use the pinned stock CCM implementation as an independent wire oracle");
  for(size_t keyLen:std::initializer_list<size_t>{16,32})
    for(size_t size:std::initializer_list<size_t>{1,15,16,17,bench::ChannelAead::MaxPlain})
    exactStockVector(keyLen,size);
  exactStockVector(16,16,0u,1u,0u); // Crypto primitive preserves caller field bit patterns; codec owns admission.
  pass("key16/key32 CCM bytes and tag exactly match pinned stock for boundary payloads and reserved crypto-context words");

  {
    uint8_t key[16];memset(key,0x34,sizeof(key));uint8_t plain[17];payload(plain,sizeof(plain));
    uint8_t wire[64];memset(wire,0xA5,sizeof(wire));size_t n=99;
    assert(bench::ChannelAead::encrypt(bench::mtAesCtr,nullptr,key,sizeof(key),0x12345678,0xffffffffu,42,
      plain,sizeof(plain),wire,sizeof(wire),n)==bench::ChannelAeadResult::Success&&n==29);
    uint8_t output[32];size_t outLen=99;
    assert(bench::ChannelAead::decrypt(bench::mtAesCtr,nullptr,key,sizeof(key),0x12345678,0xffffffffu,42,
      wire,n,output,sizeof(output),outLen)==bench::ChannelAeadResult::Success&&outLen==sizeof(plain));
    uint8_t overlap[64]{};memcpy(overlap,plain,sizeof(plain));size_t overlapLen=0;
    assert(bench::ChannelAead::encrypt(bench::mtAesCtr,nullptr,key,sizeof(key),0x12345678,0xffffffffu,42,
      overlap,sizeof(plain),overlap,sizeof(overlap),overlapLen)==bench::ChannelAeadResult::Success&&overlapLen==n);
    assert(!memcmp(overlap,wire,n));
    assert(bench::ChannelAead::decrypt(bench::mtAesCtr,nullptr,key,sizeof(key),0x12345678,0xffffffffu,42,
      overlap,overlapLen,overlap,sizeof(overlap),outLen)==bench::ChannelAeadResult::Success&&outLen==sizeof(plain));
    assert(!memcmp(overlap,plain,sizeof(plain)));
    uint8_t capOut[64];memset(capOut,0xA5,sizeof(capOut));size_t capLen=99;
    assert(bench::ChannelAead::encrypt(bench::mtAesCtr,nullptr,key,sizeof(key),0x12345678,0xffffffffu,42,
      plain,sizeof(plain),capOut,sizeof(plain)+11,capLen)==bench::ChannelAeadResult::BackendFailed&&capLen==0&&allByte(capOut,sizeof(capOut),0xA5));
    assert(bench::ChannelAead::decrypt(bench::mtAesCtr,nullptr,key,sizeof(key),0x12345678,0xffffffffu,42,
      wire,n,capOut,sizeof(plain)-1,capLen)==bench::ChannelAeadResult::BackendFailed&&capLen==0&&allByte(capOut,sizeof(capOut),0xA5));
    for(size_t i=0;i<n;++i){uint8_t changed[64];memcpy(changed,wire,n);changed[i]^=1;memset(output,0xA5,sizeof(output));outLen=99;
      assert(bench::ChannelAead::decrypt(bench::mtAesCtr,nullptr,key,sizeof(key),0x12345678,0xffffffffu,42,
        changed,n,output,sizeof(output),outLen)==bench::ChannelAeadResult::AuthenticationFailed);
      assert(outLen==0&&allByte(output,sizeof(plain),0)&&output[sizeof(plain)]==0xA5);
    }
    uint8_t wrongKey[16];memcpy(wrongKey,key,sizeof(key));wrongKey[0]^=1;
    for(auto context:{0u,1u,2u,3u}){uint32_t from=0x12345678,to=0xffffffffu,id=42;
      if(context==1)from++;
      if(context==2)to=0x87654321;
      if(context==3)id++;
      memset(output,0xA5,sizeof(output));outLen=99;
      assert(bench::ChannelAead::decrypt(bench::mtAesCtr,nullptr,context==0?wrongKey:key,sizeof(key),from,to,id,
        wire,n,output,sizeof(output),outLen)==bench::ChannelAeadResult::AuthenticationFailed);
      assert(outLen==0&&allByte(output,sizeof(plain),0));
    }
    for(size_t shortLen=0;shortLen<=12;++shortLen){outLen=99;memset(output,0xA5,sizeof(output));
      assert(bench::ChannelAead::decrypt(bench::mtAesCtr,nullptr,key,sizeof(key),0x12345678,0xffffffffu,42,
        wire,shortLen,output,sizeof(output),outLen)==bench::ChannelAeadResult::AuthenticationFailed&&outLen==0);
    }
    n=99;memset(wire,0xA5,sizeof(wire));
    assert(bench::ChannelAead::encrypt(bench::mtAesCtr,nullptr,key,sizeof(key),0x12345678,0xffffffffu,42,
      plain,0,wire,sizeof(wire),n)==bench::ChannelAeadResult::BackendFailed&&n==0&&allByte(wire,sizeof(wire),0xA5));
    uint8_t oversized[228]{};
    assert(bench::ChannelAead::encrypt(bench::mtAesCtr,nullptr,key,sizeof(key),0x12345678,0xffffffffu,42,
      oversized,sizeof(oversized),wire,sizeof(wire),n)==bench::ChannelAeadResult::BackendFailed&&n==0);
    pass("every ciphertext/tag byte, wrong key/context, truncation and length/cap refusal fail closed without CTR fallback");
  }

  {
    uint8_t key[16];memset(key,0x5c,sizeof(key));uint8_t plain[31];payload(plain,sizeof(plain));
    constexpr uint32_t From=0x23456789,To=0x87654321,Id=0x24681357;uint8_t wire[64]{};size_t wireLen=0;
    assert(bench::ChannelAead::encrypt(bench::mtAesCtr,nullptr,key,sizeof(key),From,To,Id,plain,sizeof(plain),wire,sizeof(wire),wireLen)==bench::ChannelAeadResult::Success);
    const size_t calls=3+2*((sizeof(plain)+15)/16);
    for(size_t failure=1;failure<=calls;++failure){Fault fault{0,failure};uint8_t out[64];memset(out,0xA5,sizeof(out));size_t outLen=99;
      assert(bench::ChannelAead::encrypt(failingBlock,&fault,key,sizeof(key),From,To,Id,plain,sizeof(plain),out,sizeof(out),outLen)==bench::ChannelAeadResult::BackendFailed);
      assert(outLen==0&&allByte(out,sizeof(plain)+12,0)&&out[sizeof(plain)+12]==0xA5);
      Fault decryptFault{0,failure};memset(out,0xA5,sizeof(out));outLen=99;
      assert(bench::ChannelAead::decrypt(failingBlock,&decryptFault,key,sizeof(key),From,To,Id,wire,wireLen,out,sizeof(out),outLen)==bench::ChannelAeadResult::BackendFailed);
      assert(outLen==0&&allByte(out,sizeof(plain),0)&&out[sizeof(plain)]==0xA5);
    }
    pass("failure injected at each CCM block call wipes bounded output and never publishes plaintext");
  }

  {
    uint8_t key[16];memset(key,0x2d,sizeof(key));
    constexpr uint8_t data[]={8,1,18,1,'x',72,0}; // canonical Data, bitfield present, no request/signature.
    constexpr uint8_t hash=0x4b;bench::MtCodec aead,ctr;assert(aead.setKey(uint8_t(hash^0xaeu),key,sizeof(key),true));
    assert(ctr.setKey(hash,key,sizeof(key)));
    uint8_t rfAead[64]{},rfCtr[64]{};size_t aeadLen=0,ctrLen=0;
    assert(aead.encodeZeroHopData(0x12345678,0xffffffffu,91,data,sizeof(data),rfAead,sizeof(rfAead),aeadLen));
    assert(ctr.encodeZeroHopData(0x12345678,0xffffffffu,91,data,sizeof(data),rfCtr,sizeof(rfCtr),ctrLen));
    assert(aead.aead()&&aeadLen==sizeof(data)+28&&rfAead[13]==uint8_t(hash^0xaeu));
    assert(!ctr.aead()&&ctrLen==sizeof(data)+16&&rfCtr[13]==hash);
    bench::RouteMetadata m{};assert(aead.decode(rfAead,aeadLen,m)&&m.channelDecoded&&m.portnum==1);
    assert(ctr.decode(rfCtr,ctrLen,m)&&m.channelDecoded&&m.portnum==1);
    for(unsigned offset:{0u,4u,8u}){uint8_t bad[64];memcpy(bad,rfAead,aeadLen);bad[offset]^=1;
      assert(!aead.decode(bad,aeadLen,m)&&!m.channelDecoded&&!m.backendFailed);}
    uint8_t hopChanged[64];memcpy(hopChanged,rfAead,aeadLen);hopChanged[12]^=0x23;
    assert(aead.decode(hopChanged,aeadLen,m)&&m.channelDecoded);
    uint8_t badTag[64];memcpy(badTag,rfAead,aeadLen);badTag[aeadLen-1]^=1;
    assert(!aead.decode(badTag,aeadLen,m)&&!m.channelDecoded&&!m.backendFailed);
    // Largest canonical text Data accepted by the AEAD envelope is 227 bytes.
    uint8_t maximum[227]{};maximum[0]=8;maximum[1]=1;maximum[2]=18;
    maximum[3]=0xdc;maximum[4]=1;memset(maximum+5,'x',220);maximum[225]=72;
    uint8_t fullFrame[255]{};size_t fullSize=0;
    assert(aead.encodeZeroHopData(0x12345678,0xffffffffu,92,maximum,sizeof(maximum),fullFrame,sizeof(fullFrame),fullSize)&&fullSize==255);
    assert(aead.decode(fullFrame,fullSize,m)&&m.channelDecoded&&m.payloadLength==220);
    uint8_t tooLarge[228]{};tooLarge[0]=8;tooLarge[1]=1;tooLarge[2]=18;
    tooLarge[3]=0xdd;tooLarge[4]=1;memset(tooLarge+5,'x',221);tooLarge[226]=72;
    assert(!aead.encodeZeroHopData(0x12345678,0xffffffffu,93,tooLarge,sizeof(tooLarge),fullFrame,sizeof(fullFrame),fullSize)&&fullSize==0);
    pass("MtCodec AEAD roundtrips add exactly 12 bytes; default CTR wire stays unchanged; nonce/AAD bind header and hop stays mutable");
  }

  {
    mt::Config config;assert(config.valid()&&config.channelConfigured());
    assert(config.setPrimaryAead(true));mt::PrimaryChannel::Selection primary;
    assert(config.channelSelection(0,primary)&&primary.aead&&primary.hash==uint8_t(config.node().bytes[4]^0xaeu));
    const uint8_t secondaryKey[16]={0x44};mt::ChannelRecord secondary;
    assert(secondary.set(2,"AeadChild",secondaryKey,sizeof(secondaryKey),0,false,false,false,true));
    assert(config.setSecondaryChannel(1,secondary));mt::Config v22Secondary=config;
    mt::PrimaryChannel::Selection child;
    uint8_t childBaseHash=0;assert(mt::ChannelName::hash("AeadChild",secondaryKey,sizeof(secondaryKey),childBaseHash));
    assert(config.channelSelection(1,child)&&child.aead&&child.hash==uint8_t(childBaseHash^0xaeu));
    // The mode marker follows promotion into the primary Config byte.
    assert(config.promoteChannel(1)&&config.primaryAead()&&config.channelSelection(0,primary)&&primary.aead);
    assert(config.valid());
    mt::Config old;old.bytes[2]=22;old.seal();
    mt::Config migrated;assert(mt::Config::decode(old.bytes,sizeof(old.bytes),migrated)&&!migrated.primaryAead());
    mt::Config corrupt=old;corrupt.bytes[604]=1;corrupt.seal();assert(!mt::Config::decode(corrupt.bytes,sizeof(corrupt.bytes),migrated));
    assert(v22Secondary.setPrimaryAead(false));v22Secondary.bytes[2]=22;v22Secondary.bytes[604]=0;v22Secondary.seal();
    assert(!mt::Config::decode(v22Secondary.bytes,sizeof(v22Secondary.bytes),migrated));
    pass("Config V23 stores primary/secondary AEAD mode, promotion preserves it, and V22 reserved-byte corruption is rejected");
  }
  std::printf("PASS channel AEAD suite groups=%u\n",groups);
}
