// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiCrypto.h"
#include <psa/crypto.h>

namespace mt::pki {
// Injectable API boundary for fault tests; the default implementation calls the
// pinned PSA library directly. No persistent-key or key-generation operation.
struct PsaApi {
  virtual ~PsaApi()=default;
  virtual psa_status_t init();
  virtual psa_status_t hash(const uint8_t *,size_t,uint8_t *,size_t,size_t *);
  virtual psa_status_t importKey(const psa_key_attributes_t *,const uint8_t *,size_t,psa_key_id_t *);
  virtual psa_status_t destroy(psa_key_id_t);
  virtual psa_status_t mac(psa_key_id_t,const uint8_t *,size_t,uint8_t *,size_t,size_t *);
  virtual psa_status_t aead(bool,psa_key_id_t,const uint8_t *,const uint8_t *,size_t,uint8_t *,size_t,size_t *);
};
class PsaPkiBackend final:public experimental::PkiBackend {
  PsaApi systemApi_;
  PsaApi &api_;
  bool initialized_=false,poisoned_=false;
  psa_key_id_t quarantined_=0;
  bool release(psa_key_id_t key);
  bool import(const uint8_t key[32],psa_key_type_t type,psa_algorithm_t algorithm,
              psa_key_usage_t usage,psa_key_id_t &id);
public:
  static constexpr size_t MaxInput=256;
  PsaPkiBackend();
  explicit PsaPkiBackend(PsaApi &api):api_(api){}
  ~PsaPkiBackend()override;
  PsaPkiBackend(const PsaPkiBackend &)=delete;
  PsaPkiBackend &operator=(const PsaPkiBackend &)=delete;
  bool begin();
  bool healthy()const override{return initialized_&&!poisoned_;}
  bool poisoned()const{return poisoned_;}
  bool hasQuarantinedKey()const{return quarantined_!=0;}
  // Terminal shutdown. Reports unresolved cleanup; never retries an uncertain
  // volatile ID (it may have been reused), and never unpoisons the backend.
  bool close();
  bool sha256(const uint8_t *,size_t,uint8_t out[32])override;
  bool hmac256(const uint8_t key[32],const uint8_t *,size_t,uint8_t out[32])override;
  bool ccm(bool,const uint8_t key[32],const uint8_t nonce[13],
           const uint8_t *,size_t,uint8_t *,uint8_t tag[8])override;
  experimental::PkiBackend::CcmResult ccmClassified(bool,const uint8_t key[32],
      const uint8_t nonce[13],const uint8_t *,size_t,uint8_t *,uint8_t tag[8])override;
};
}
