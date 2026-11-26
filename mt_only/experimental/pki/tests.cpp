// SPDX-License-Identifier: GPL-3.0-only
#include "PkiCrypto.h"
#include "monocypher.h"
#ifdef MT_TEST_PSA
#include "PsaPkiBackend.h"
#else
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif
#include <cassert>
#include <cstring>
#include <iostream>
#include <algorithm>
extern "C" bool stockEncrypt(const uint8_t *,const uint8_t *,uint32_t,uint32_t,uint32_t,const uint8_t *,size_t,uint8_t *);
extern "C" bool stockDecrypt(const uint8_t *,const uint8_t *,uint32_t,uint32_t,const uint8_t *,size_t,uint8_t *);
extern "C" bool stockProof(const uint8_t *,const uint8_t *,uint32_t,uint32_t,uint32_t,const uint8_t *,size_t,uint8_t *);
extern "C" void stockPublic(const uint8_t *,uint8_t *);
#ifdef MT_TEST_PSA
struct Backend:mt::experimental::PkiBackend {
  mt::pki::PsaPkiBackend real;
  bool failHash=false,failMac=false,failCcm=false;
  Backend(){assert(real.begin());}
  bool sha256(const uint8_t *p,size_t n,uint8_t out[32])override{return !failHash&&real.sha256(p,n,out);}
  bool hmac256(const uint8_t key[32],const uint8_t *p,size_t n,uint8_t out[32])override{return !failMac&&real.hmac256(key,p,n,out);}
  bool ccm(bool encrypt,const uint8_t key[32],const uint8_t nonce[13],const uint8_t *p,size_t n,uint8_t *out,uint8_t tag[8])override {
    if(failCcm){memset(out,0xab,n);return false;}
    return real.ccm(encrypt,key,nonce,p,n,out,tag);
  }
};
void runPsaFaultTests();
#else
struct Backend:mt::experimental::PkiBackend {
  bool failHash=false,failMac=false,failCcm=false;
  bool sha256(const uint8_t *p,size_t n,uint8_t out[32])override {
    unsigned len=0;return !failHash&&EVP_Digest(p,n,out,&len,EVP_sha256(),nullptr)==1&&len==32;
  }
  bool hmac256(const uint8_t key[32],const uint8_t *p,size_t n,uint8_t out[32])override {
    unsigned len=0;return !failMac&&HMAC(EVP_sha256(),key,32,p,n,out,&len)&&len==32;
  }
  bool ccm(bool encrypt,const uint8_t key[32],const uint8_t nonce[13],const uint8_t *p,size_t n,uint8_t *out,uint8_t tag[8])override {
    // Deliberately scribble output on injected failure; wrapper must contain it.
    if(failCcm){memset(out,0xab,n);return false;}
    EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();if(!ctx)return false;
    int count=0;bool ok;
    if(encrypt) {
      ok=EVP_EncryptInit_ex(ctx,EVP_aes_256_ccm(),nullptr,nullptr,nullptr)==1&&
        EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_CCM_SET_IVLEN,13,nullptr)==1&&
        EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_CCM_SET_TAG,8,nullptr)==1&&
        EVP_EncryptInit_ex(ctx,nullptr,nullptr,key,nonce)==1&&
        EVP_EncryptUpdate(ctx,nullptr,&count,nullptr,int(n))==1&&
        EVP_EncryptUpdate(ctx,out,&count,p,int(n))==1&&count==int(n)&&
        EVP_EncryptFinal_ex(ctx,out+count,&count)==1&&count==0&&
        EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_CCM_GET_TAG,8,tag)==1;
    }else {
      ok=EVP_DecryptInit_ex(ctx,EVP_aes_256_ccm(),nullptr,nullptr,nullptr)==1&&
        EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_CCM_SET_IVLEN,13,nullptr)==1&&
        EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_CCM_SET_TAG,8,tag)==1&&
        EVP_DecryptInit_ex(ctx,nullptr,nullptr,key,nonce)==1&&
        EVP_DecryptUpdate(ctx,nullptr,&count,nullptr,int(n))==1&&
        EVP_DecryptUpdate(ctx,out,&count,p,int(n))==1&&count==int(n);
    }
    EVP_CIPHER_CTX_free(ctx);return ok;
  }
};
#endif
void runAckTests(mt::experimental::PkiBackend &,const uint8_t *,const uint8_t *,const uint8_t *,const uint8_t *);
void runSessionTests(mt::experimental::PkiBackend &,const uint8_t *,const uint8_t *,const uint8_t *,const uint8_t *);
int main() {
  Backend backend;mt::experimental::PkiCrypto candidate(backend);
  // PUBLIC deterministic fixtures. Never generate/provision production identity.
  uint8_t a[32],b[32],A[32],B[32],other[32];
  for(unsigned i=0;i<32;++i){a[i]=uint8_t(3+i*17);b[i]=uint8_t(99+i*29);other[i]=uint8_t(55+i*11);}
  for(auto scalar:{a,b,other}){scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;}
  crypto_x25519_public_key(A,a);crypto_x25519_public_key(B,b);
  uint8_t expectedPublic[32];stockPublic(a,expectedPublic);assert(!memcmp(A,expectedPublic,32));
  stockPublic(b,expectedPublic);assert(!memcmp(B,expectedPublic,32));
  uint8_t plain[233],wire[239],expected[239],recovered[227];
  for(unsigned i=0;i<233;++i)plain[i]=uint8_t(i*7+31);
  unsigned vectors=0;size_t len=0;
  for(size_t n=1;n<=227;++n)for(uint32_t extra:{0u,0x12345678u,0xffffffffu}) {
    const uint32_t from=0x76543210u+uint32_t(n),id=0xffffff00u+uint32_t(n);
    assert(candidate.encrypt(a,B,from,id,extra,plain,n,wire,sizeof(wire),len)&&len==n+12);
    assert(stockEncrypt(a,B,from,id,extra,plain,n,expected)&&!memcmp(wire,expected,len));
    assert(candidate.decrypt(b,A,from,id,expected,n+12,recovered,sizeof(recovered),len)&&len==n&&!memcmp(plain,recovered,n));
    assert(stockDecrypt(b,A,from,id,wire,n+12,recovered)&&!memcmp(plain,recovered,n));++vectors;
  }
  std::cout<<"PASS "<<vectors<<" exact stock PKI ciphertext/nonce/tag and bidirectional decrypt vectors\n";
  assert(candidate.encrypt(a,B,123,456,789,plain,227,wire,sizeof(wire),len));
  for(size_t i=0;i<239;++i) {
    memcpy(expected,wire,239);expected[i]^=1;memset(recovered,0x5a,sizeof(recovered));len=999;
    assert(!candidate.decrypt(b,A,123,456,expected,239,recovered,sizeof(recovered),len)&&len==0);
    assert(std::all_of(recovered,recovered+227,[](uint8_t v){return v==0x5a;}));
    assert(!stockDecrypt(b,A,123,456,expected,239,recovered));
  }
  assert(!candidate.decrypt(b,A,124,456,wire,239,recovered,sizeof(recovered),len));
  assert(!candidate.decrypt(b,A,123,457,wire,239,recovered,sizeof(recovered),len));
  assert(!candidate.decrypt(other,A,123,456,wire,239,recovered,sizeof(recovered),len));
  uint8_t proof[8],stock[8];unsigned proofs=0;
  for(size_t n=0;n<=233;++n) {
    assert(candidate.proof(a,B,123,456,789,plain,n,proof));
    assert(stockProof(a,B,123,456,789,plain,n,stock)&&!memcmp(proof,stock,8));
    assert(candidate.verify(b,A,123,456,789,plain,n,proof));
    assert(!candidate.verify(b,A,456,123,789,plain,n,proof));
    assert(!candidate.verify(b,A,123,456,790,plain,n,proof));
    for(unsigned i=0;i<8;++i){proof[i]^=1;assert(!candidate.verify(b,A,123,456,789,plain,n,proof));proof[i]^=1;}++proofs;
  }
  std::cout<<"PASS "<<proofs<<" exact stock ACK proofs; direction/request binding and every proof byte tamper\n";
  for(unsigned variant=0;variant<6;++variant) {
    uint8_t weak[32]{};if(variant%3==1)weak[0]=1;
    if(variant%3==2){memset(weak,0xff,32);weak[0]=0xec;weak[31]=0x7f;}
    if(variant>=3)weak[31]|=0x80;
    assert(!candidate.encrypt(a,weak,123,456,789,plain,12,wire,sizeof(wire),len));
    assert(!stockEncrypt(a,weak,123,456,789,plain,12,expected));
  }
  uint8_t unclamped[32];memcpy(unclamped,a,32);unclamped[0]|=1;
  assert(!candidate.encrypt(unclamped,B,123,456,789,plain,12,wire,sizeof(wire),len));
  uint8_t zero[32]{};assert(!candidate.proof(zero,B,123,456,789,plain,2,proof));
  assert(!stockProof(zero,B,123,456,789,plain,2,stock));
  for(size_t n=0;n<=12;++n)assert(!candidate.decrypt(b,A,123,456,wire,n,recovered,227,len));
  assert(!candidate.encrypt(a,B,123,456,789,plain,228,wire,239,len));
  assert(!candidate.encrypt(a,B,123,456,789,plain,227,wire,238,len));
  assert(!candidate.decrypt(b,A,123,456,wire,239,recovered,226,len));
  assert(!candidate.decrypt(b,A,123,456,wire,240,recovered,227,len));
  assert(!candidate.proof(a,B,123,456,789,nullptr,1,proof));
  assert(!candidate.proof(a,B,123,456,789,plain,234,proof));
  assert(candidate.encrypt(a,B,123,456,789,plain,227,wire,239,len));
  backend.failCcm=true;memset(recovered,0x5a,227);
  assert(!candidate.decrypt(b,A,123,456,wire,239,recovered,227,len)&&len==0);
  assert(std::all_of(recovered,recovered+227,[](uint8_t v){return v==0x5a;}));
  memset(expected,0x5a,239);assert(!candidate.encrypt(a,B,123,456,789,plain,227,expected,239,len)&&len==0);
  assert(std::all_of(expected,expected+239,[](uint8_t v){return v==0x5a;}));
  backend.failCcm=false;backend.failHash=true;
  assert(!candidate.encrypt(a,B,123,456,789,plain,12,wire,239,len));backend.failHash=false;backend.failMac=true;
  memset(proof,0x5a,8);assert(!candidate.proof(a,B,123,456,789,plain,2,proof));
  assert(std::all_of(proof,proof+8,[](uint8_t v){return v==0x5a;}));
  std::cout<<"PASS bounded inputs, weak points, identity absence and backend failure containment\n";
  backend.failMac=false;runAckTests(backend,a,b,A,B);
  runSessionTests(backend,a,b,A,B);
#ifdef MT_TEST_PSA
  runPsaFaultTests();
#endif
  std::cout<<"PASS host run only; no runtime callers, credentials, devices or RF\n";
}
