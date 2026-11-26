// SPDX-License-Identifier: GPL-3.0-only
#include "PsaPkiBackend.h"
#include <psa/crypto_extra.h>
#include <cassert>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <set>
using namespace mt::pki;
extern unsigned pkiWipe32,pkiWipe235;
namespace {
unsigned entropyRequests=0;
enum class Fault {None,Init,Hash,HashLength,ImportBefore,ImportAfter,ImportZero,
                  Mac,MacLength,Encrypt,Decrypt,CcmLength,DestroyBefore,DestroyAfter};
struct Api:PsaApi {
  Fault fault=Fault::None;unsigned calls=0,imports=0,destroys=0;
  std::set<psa_key_id_t> alive;psa_key_id_t lastImported=0;
  psa_status_t init()override{++calls;return fault==Fault::Init?PSA_ERROR_HARDWARE_FAILURE:PsaApi::init();}
  psa_status_t hash(const uint8_t *p,size_t n,uint8_t *o,size_t cap,size_t *len)override {
    ++calls;if(fault==Fault::Hash){memset(o,0xa7,cap);return PSA_ERROR_HARDWARE_FAILURE;}
    const auto status=PsaApi::hash(p,n,o,cap,len);if(fault==Fault::HashLength)++*len;return status;
  }
  psa_status_t importKey(const psa_key_attributes_t *attrs,const uint8_t *p,size_t n,psa_key_id_t *id)override {
    ++calls;++imports;
    assert(psa_get_key_lifetime(attrs)==PSA_KEY_LIFETIME_VOLATILE);
    assert(psa_get_key_id(attrs)==0 && psa_get_key_bits(attrs)==256 && n==32);
    const auto type=psa_get_key_type(attrs);const auto usage=psa_get_key_usage_flags(attrs);
    assert(type==PSA_KEY_TYPE_AES||type==PSA_KEY_TYPE_HMAC);
    assert(usage==PSA_KEY_USAGE_ENCRYPT||usage==PSA_KEY_USAGE_DECRYPT||usage==PSA_KEY_USAGE_SIGN_MESSAGE);
    if(fault==Fault::ImportBefore)return PSA_ERROR_INSUFFICIENT_MEMORY;
    if(fault==Fault::ImportZero){*id=0;return PSA_SUCCESS;}
    const auto status=PsaApi::importKey(attrs,p,n,id);
    if(status==PSA_SUCCESS){alive.insert(*id);lastImported=*id;}
    return fault==Fault::ImportAfter?PSA_ERROR_HARDWARE_FAILURE:status;
  }
  psa_status_t destroy(psa_key_id_t id)override {
    ++calls;++destroys;if(fault==Fault::DestroyBefore)return PSA_ERROR_HARDWARE_FAILURE;
    const auto status=PsaApi::destroy(id);if(status==PSA_SUCCESS)alive.erase(id);
    return fault==Fault::DestroyAfter?PSA_ERROR_HARDWARE_FAILURE:status;
  }
  psa_status_t mac(psa_key_id_t id,const uint8_t *p,size_t n,uint8_t *o,size_t cap,size_t *len)override {
    ++calls;if(fault==Fault::Mac){memset(o,0xa7,cap);return PSA_ERROR_HARDWARE_FAILURE;}
    const auto status=PsaApi::mac(id,p,n,o,cap,len);if(fault==Fault::MacLength)++*len;return status;
  }
  psa_status_t aead(bool encrypt,psa_key_id_t id,const uint8_t *nonce,const uint8_t *p,size_t n,uint8_t *o,size_t cap,size_t *len)override {
    ++calls;if(fault==(encrypt?Fault::Encrypt:Fault::Decrypt)) {memset(o,0xa7,cap);return PSA_ERROR_INVALID_SIGNATURE;}
    const auto status=PsaApi::aead(encrypt,id,nonce,p,n,o,cap,len);if(fault==Fault::CcmLength)++*len;return status;
  }
};
void untouched(const uint8_t *p,size_t n){assert(std::all_of(p,p+n,[](uint8_t b){return b==0x5a;}));}
}
extern "C" psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *,uint8_t *,size_t,size_t *len) {
  ++entropyRequests;*len=0;return PSA_ERROR_INSUFFICIENT_ENTROPY;
}
void runPsaFaultTests() {
  uint8_t key[32],nonce[13],plain[227],cipher[227],tag[8],out[227],mac[32];
  for(unsigned i=0;i<32;++i)key[i]=uint8_t(33+i*7);
  for(unsigned i=0;i<13;++i)nonce[i]=uint8_t(11+i*17);
  for(unsigned i=0;i<227;++i)plain[i]=uint8_t(13+i*3);
  unsigned cases=0;
  {Api api;PsaPkiBackend backend(api);memset(mac,0x5a,32);
   assert(!backend.sha256(plain,32,mac)&&!api.calls);untouched(mac,32);
   assert(backend.begin()&&backend.begin());
   assert(backend.ccm(true,key,nonce,plain,227,cipher,tag)&&api.alive.empty());
   assert(backend.ccm(false,key,nonce,cipher,227,out,tag)&&!memcmp(out,plain,227)&&api.alive.empty());
   for(unsigned i=0;i<500;++i)assert(backend.hmac256(key,plain,227,mac)&&api.alive.empty());
   assert(api.imports==api.destroys);++cases;}
  for(const auto fault:{Fault::Init,Fault::Hash,Fault::HashLength,Fault::ImportBefore,Fault::ImportAfter,
                        Fault::ImportZero,Fault::Mac,Fault::MacLength,Fault::Encrypt,Fault::Decrypt,Fault::CcmLength}) {
    Api api;PsaPkiBackend backend(api);api.fault=fault;
    memset(out,0x5a,227);memset(mac,0x5a,32);uint8_t callerTag[8];memcpy(callerTag,tag,8);
    if(fault==Fault::Init) {
      assert(!backend.begin()&&backend.poisoned());api.fault=Fault::None;assert(!backend.begin());
      assert(!backend.sha256(plain,32,mac));untouched(mac,32);++cases;continue;
    }
    assert(backend.begin());const unsigned wipe32=pkiWipe32,wipe235=pkiWipe235;
    if(fault==Fault::Hash||fault==Fault::HashLength) {
      assert(!backend.sha256(plain,32,mac));untouched(mac,32);assert(pkiWipe32>wipe32);
    }else if(fault==Fault::Encrypt||fault==Fault::Decrypt||fault==Fault::CcmLength) {
      assert(!backend.ccm(fault!=Fault::Decrypt,key,nonce,fault==Fault::Decrypt?cipher:plain,227,out,callerTag));
      untouched(out,227);assert(!memcmp(callerTag,tag,8));assert(pkiWipe235>=wipe235+2);
    }else {
      assert(!backend.hmac256(key,plain,227,mac));untouched(mac,32);
      if(fault==Fault::Mac||fault==Fault::MacLength)assert(pkiWipe32>wipe32);
    }
    assert(api.alive.empty()&&!backend.poisoned());++cases;
  }
  for(const auto fault:{Fault::DestroyBefore,Fault::DestroyAfter}) {
    Api api;psa_key_id_t replacement=0,uncertain=0;
    {
      PsaPkiBackend backend(api);assert(backend.begin());api.fault=fault;
      memset(out,0x5a,227);uint8_t t[8];memset(t,0x5a,8);
      assert(!backend.ccm(true,key,nonce,plain,227,out,t));untouched(out,227);untouched(t,8);
      assert(backend.poisoned()&&backend.hasQuarantinedKey());
      const auto calls=api.calls;memset(mac,0x5a,32);
      assert(!backend.sha256(plain,32,mac));assert(!backend.hmac256(key,plain,32,mac));
      assert(!backend.ccm(true,key,nonce,plain,227,out,t));assert(!backend.begin());
      untouched(mac,32);untouched(out,227);untouched(t,8);assert(api.calls==calls);
      if(fault==Fault::DestroyAfter) {
        psa_key_attributes_t attrs=PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attrs,PSA_KEY_TYPE_AES);psa_set_key_bits(&attrs,256);
        psa_set_key_usage_flags(&attrs,PSA_KEY_USAGE_ENCRYPT);
        psa_set_key_algorithm(&attrs,PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM,8));
        assert(api.PsaApi::importKey(&attrs,key,32,&replacement)==PSA_SUCCESS);
        assert(replacement==api.lastImported); // Exact volatile ID reuse regression.
        psa_reset_key_attributes(&attrs);
      }else uncertain=api.lastImported;
      assert(!backend.close()&&backend.poisoned()&&!backend.begin());
      assert(api.calls==calls); // No blind destroy of an uncertain/reused ID.
    }
    if(replacement) {
      psa_key_attributes_t attrs=PSA_KEY_ATTRIBUTES_INIT;
      assert(psa_get_key_attributes(replacement,&attrs)==PSA_SUCCESS);
      psa_reset_key_attributes(&attrs);assert(api.PsaApi::destroy(replacement)==PSA_SUCCESS);
    }
    // Test harness knows the injected pre-destroy fault left its own key intact.
    // Runtime cannot infer that and must report unresolved cleanup, not retry.
    if(uncertain)assert(api.PsaApi::destroy(uncertain)==PSA_SUCCESS);
    ++cases;
  }
  {Api api;PsaPkiBackend backend(api);assert(backend.begin());memset(mac,0x5a,32);
   assert(!backend.sha256(nullptr,1,mac)&&!backend.sha256(plain,257,mac));
   assert(!backend.hmac256(nullptr,plain,32,mac)&&!backend.hmac256(key,nullptr,1,mac));
   assert(!backend.ccm(true,key,nonce,plain,0,out,tag)&&!backend.ccm(true,key,nonce,plain,228,out,tag));
   assert(!backend.ccm(true,nullptr,nonce,plain,1,out,tag));assert(api.imports==0);untouched(mac,32);++cases;}
  // All crypto above imports synthetic fixtures, never asks PSA for key material.
  assert(entropyRequests==0);
  uint8_t neverRandom[32];assert(psa_generate_random(neverRandom,32)!=PSA_SUCCESS&&entropyRequests>0);
  std::cout<<"PASS "<<cases<<" real PSA fault groups: volatile imports, cleanup quarantine, no plaintext/output escape, actual wipes, entropy refusal\n";
}
