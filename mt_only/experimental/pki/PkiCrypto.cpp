// SPDX-License-Identifier: GPL-3.0-only
#include "PkiCrypto.h"
#include "monocypher.h"
#include <string.h>
namespace mt::experimental {
namespace {
void write32(uint8_t *p,uint32_t x){for(unsigned i=0;i<4;++i)p[i]=uint8_t(x>>(8*i));}
uint32_t read32(const uint8_t *p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
bool nonzero(const uint8_t *p){uint8_t v=0;for(unsigned i=0;i<32;++i)v|=p[i];return v!=0;}
}
bool PkiCrypto::shared(const uint8_t secret[32],const uint8_t peer[32],uint8_t key[32]) {
  if(!secret||!peer||!nonzero(secret)||(secret[0]&7)||(secret[31]&0xc0)!=0x40)return false;
  if(sessionKey_&&sessionKey_->valid){memcpy(key,sessionKey_->bytes,32);return true;}
  uint8_t product[32]{};crypto_x25519(product,secret,peer);
  const bool ok=nonzero(product)&&backend_.sha256(product,32,key);
  crypto_wipe(product,sizeof(product));
  if(!ok)crypto_wipe(key,32);
  else if(sessionKey_){memcpy(sessionKey_->bytes,key,32);sessionKey_->valid=true;}
  return ok;
}
void PkiCrypto::nonce(uint32_t from,uint32_t id,uint32_t extra,uint8_t out[13]) {
  memset(out,0,13);write32(out,id);write32(out+4,extra);write32(out+8,from);
}
bool PkiCrypto::encrypt(const uint8_t secret[32],const uint8_t peer[32],uint32_t from,
                       uint32_t id,uint32_t extra,const uint8_t *plain,size_t n,
                       uint8_t *out,size_t cap,size_t &len) {
  len=0;if(!plain||!out||!n||n>MaxPlaintext||cap<n+Overhead)return false;
  uint8_t key[32]{},iv[13]{},tmp[239]{};nonce(from,id,extra,iv);
  const bool ok=shared(secret,peer,key)&&backend_.ccm(true,key,iv,plain,n,tmp,tmp+n);
  if(ok){write32(tmp+n+8,extra);memcpy(out,tmp,n+Overhead);len=n+Overhead;}
  crypto_wipe(key,sizeof(key));crypto_wipe(iv,sizeof(iv));crypto_wipe(tmp,sizeof(tmp));return ok;
}
bool PkiCrypto::decrypt(const uint8_t secret[32],const uint8_t peer[32],uint32_t from,
                       uint32_t id,const uint8_t *cipher,size_t n,uint8_t *out,size_t cap,size_t &len) {
  return decryptClassified(secret,peer,from,id,cipher,n,out,cap,len)==DecryptResult::Ok;
}
DecryptResult PkiCrypto::decryptClassified(const uint8_t secret[32],const uint8_t peer[32],uint32_t from,
                       uint32_t id,const uint8_t *cipher,size_t n,uint8_t *out,size_t cap,size_t &len) {
  len=0;if(!cipher||!out||n<=Overhead||n>239||cap<n-Overhead)return DecryptResult::BackendFailure;
  const size_t plainSize=n-Overhead;
  uint8_t key[32]{},iv[13]{},tmp[MaxPlaintext]{},tag[8]{};
  nonce(from,id,read32(cipher+n-4),iv);memcpy(tag,cipher+plainSize,8);
  const bool derived=shared(secret,peer,key);
  const auto ccm=derived?backend_.ccmClassified(false,key,iv,cipher,plainSize,tmp,tag):
    PkiBackend::CcmResult::BackendFailure;
  const bool ok=derived&&ccm==PkiBackend::CcmResult::Ok;
  if(ok){memcpy(out,tmp,plainSize);len=plainSize;}
  // Backend may write unauthenticated plaintext; never publish it on failure.
  crypto_wipe(key,sizeof(key));crypto_wipe(iv,sizeof(iv));crypto_wipe(tmp,sizeof(tmp));crypto_wipe(tag,sizeof(tag));
  if(ok)return DecryptResult::Ok;
  return derived&&ccm==PkiBackend::CcmResult::AuthenticationFailed?
    DecryptResult::AuthenticationFailed:DecryptResult::BackendFailure;
}
bool PkiCrypto::proof(const uint8_t secret[32],const uint8_t peer[32],uint32_t from,
                     uint32_t to,uint32_t request,const uint8_t *routing,size_t n,uint8_t out[8]) {
  if(!out||(!routing&&n)||n>233)return false;
  uint8_t key[32]{},data[248]{},digest[32]{};memcpy(data,"ack",3);
  write32(data+3,from);write32(data+7,to);write32(data+11,request);if(n)memcpy(data+15,routing,n);
  const bool ok=shared(secret,peer,key)&&backend_.hmac256(key,data,15+n,digest);
  if(ok)memcpy(out,digest,8);
  crypto_wipe(key,sizeof(key));crypto_wipe(data,sizeof(data));crypto_wipe(digest,sizeof(digest));return ok;
}
bool PkiCrypto::verify(const uint8_t secret[32],const uint8_t peer[32],uint32_t from,
                      uint32_t to,uint32_t request,const uint8_t *routing,size_t n,const uint8_t received[8]) {
  if(!received)return false;
  uint8_t expected[16]{},actual[16]{};memcpy(actual,received,8);
  const bool ok=proof(secret,peer,from,to,request,routing,n,expected)&&crypto_verify16(expected,actual)==0;
  crypto_wipe(expected,sizeof(expected));crypto_wipe(actual,sizeof(actual));return ok;
}
}
