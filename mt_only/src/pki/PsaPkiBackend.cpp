// SPDX-License-Identifier: GPL-3.0-only
#include "PsaPkiBackend.h"
#include "monocypher.h"
#include <string.h>
namespace mt::pki {
namespace {
constexpr auto Ccm=PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM,8);
template<size_t N> struct Secret {
  uint8_t bytes[N]{};
  ~Secret(){crypto_wipe(bytes,N);}
};
}
psa_status_t PsaApi::init(){return psa_crypto_init();}
psa_status_t PsaApi::hash(const uint8_t *p,size_t n,uint8_t *out,size_t cap,size_t *len) {
  return psa_hash_compute(PSA_ALG_SHA_256,p,n,out,cap,len);
}
psa_status_t PsaApi::importKey(const psa_key_attributes_t *a,const uint8_t *p,size_t n,psa_key_id_t *id) {
  return psa_import_key(a,p,n,id);
}
psa_status_t PsaApi::destroy(psa_key_id_t id){return psa_destroy_key(id);}
psa_status_t PsaApi::mac(psa_key_id_t id,const uint8_t *p,size_t n,uint8_t *out,size_t cap,size_t *len) {
  return psa_mac_compute(id,PSA_ALG_HMAC(PSA_ALG_SHA_256),p,n,out,cap,len);
}
psa_status_t PsaApi::aead(bool encrypt,psa_key_id_t id,const uint8_t *nonce,
                        const uint8_t *p,size_t n,uint8_t *out,size_t cap,size_t *len) {
  return encrypt?psa_aead_encrypt(id,Ccm,nonce,13,nullptr,0,p,n,out,cap,len):
                 psa_aead_decrypt(id,Ccm,nonce,13,nullptr,0,p,n,out,cap,len);
}
PsaPkiBackend::PsaPkiBackend():api_(systemApi_){}
PsaPkiBackend::~PsaPkiBackend(){close();}
bool PsaPkiBackend::begin() {
  if(poisoned_)return false;
  if(initialized_)return true;
  initialized_=api_.init()==PSA_SUCCESS;
  if(!initialized_)poisoned_=true;
  return initialized_;
}
bool PsaPkiBackend::release(psa_key_id_t key) {
  if(!key)return true;
  if(api_.destroy(key)==PSA_SUCCESS)return true;
  // At most one uncertain key: poison blocks every subsequent import and output.
  quarantined_=key;poisoned_=true;return false;
}
bool PsaPkiBackend::close() {
  initialized_=false;poisoned_=true;
  // Never retry an uncertain volatile ID: PSA may already have freed it and
  // another user (e.g. Bluetooth) may now own the same numeric slot. Only the
  // immediate release attempts destruction. Report unresolved cleanup forever.
  return quarantined_==0;
}
bool PsaPkiBackend::import(const uint8_t key[32],psa_key_type_t type,
                           psa_algorithm_t algorithm,psa_key_usage_t usage,
                           psa_key_id_t &id) {
  id=0;
  psa_key_attributes_t attrs=PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attrs,type);psa_set_key_bits(&attrs,256);
  psa_set_key_algorithm(&attrs,algorithm);psa_set_key_usage_flags(&attrs,usage);
  psa_set_key_lifetime(&attrs,PSA_KEY_LIFETIME_VOLATILE);
  const auto status=api_.importKey(&attrs,key,32,&id);
  psa_reset_key_attributes(&attrs);
  if(status==PSA_SUCCESS && id)return true;
  // Defensive against a provider that reports failure after allocating a slot.
  if(id)release(id);
  id=0;return false;
}
bool PsaPkiBackend::sha256(const uint8_t *p,size_t n,uint8_t out[32]) {
  if(!healthy() || !out || (!p&&n) || n>MaxInput)return false;
  Secret<32> tmp;size_t len=0;
  const bool ok=api_.hash(p,n,tmp.bytes,32,&len)==PSA_SUCCESS && len==32;
  if(ok)memcpy(out,tmp.bytes,32);
  return ok;
}
bool PsaPkiBackend::hmac256(const uint8_t key[32],const uint8_t *p,size_t n,uint8_t out[32]) {
  if(!healthy() || !key || !out || (!p&&n) || n>MaxInput)return false;
  psa_key_id_t id=0;
  if(!import(key,PSA_KEY_TYPE_HMAC,PSA_ALG_HMAC(PSA_ALG_SHA_256),PSA_KEY_USAGE_SIGN_MESSAGE,id))return false;
  Secret<32> tmp;size_t len=0;
  const bool computed=api_.mac(id,p,n,tmp.bytes,32,&len)==PSA_SUCCESS && len==32;
  const bool released=release(id);
  if(computed&&released)memcpy(out,tmp.bytes,32);
  return computed&&released;
}
bool PsaPkiBackend::ccm(bool encrypt,const uint8_t key[32],const uint8_t nonce[13],
                        const uint8_t *p,size_t n,uint8_t *out,uint8_t tag[8]) {
  return ccmClassified(encrypt,key,nonce,p,n,out,tag)==experimental::PkiBackend::CcmResult::Ok;
}
experimental::PkiBackend::CcmResult PsaPkiBackend::ccmClassified(bool encrypt,
                        const uint8_t key[32],const uint8_t nonce[13],
                        const uint8_t *p,size_t n,uint8_t *out,uint8_t tag[8]) {
  using CcmResult=experimental::PkiBackend::CcmResult;
  if(!healthy() || !key || !nonce || !p || !out || !tag || !n || n>227)return CcmResult::BackendFailure;
  psa_key_id_t id=0;
  if(!import(key,PSA_KEY_TYPE_AES,Ccm,encrypt?PSA_KEY_USAGE_ENCRYPT:PSA_KEY_USAGE_DECRYPT,id))return CcmResult::BackendFailure;
  Secret<235> input,output;size_t len=0;
  if(!encrypt){memcpy(input.bytes,p,n);memcpy(input.bytes+n,tag,8);}
  const psa_status_t status=api_.aead(encrypt,id,nonce,encrypt?p:input.bytes,
                               encrypt?n:n+8,output.bytes,sizeof(output.bytes),&len);
  const bool computed=status==PSA_SUCCESS&&len==(encrypt?n+8:n);
  const bool released=release(id);
  if(computed&&released) {
    memcpy(out,output.bytes,n);
    if(encrypt)memcpy(tag,output.bytes+n,8);
  }
  if(computed&&released)return CcmResult::Ok;
  if(!encrypt&&status==PSA_ERROR_INVALID_SIGNATURE&&len==0&&released)
    return CcmResult::AuthenticationFailed;
  return CcmResult::BackendFailure;
}
}
