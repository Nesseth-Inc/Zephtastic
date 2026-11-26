// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Protocol primitive used by opt-in, explicitly injected runtime.
// No default initialization, key generation/storage,
// peer trust, nonce generation, radio framing or authenticated delivery policy.
#include <stddef.h>
#include <stdint.h>

namespace mt::pki {class PkiSession;}
namespace mt::experimental {
struct PkiBackend {
  enum class CcmResult:uint8_t { Ok,AuthenticationFailed,BackendFailure };
  virtual ~PkiBackend()=default;
  // Stateless backends may keep this default. Stateful backends MUST override
  // it so shutdown or uncertain cleanup immediately removes runtime authority.
  // An ordinary authentication/tag rejection does not make a backend unhealthy.
  virtual bool healthy()const{return true;}
  virtual bool sha256(const uint8_t *,size_t,uint8_t out[32])=0;
  virtual bool hmac256(const uint8_t key[32],const uint8_t *,size_t,uint8_t out[32])=0;
  // CCM uses a 13-byte nonce, 8-byte tag, no AAD, and separate tag buffers.
  virtual bool ccm(bool encrypt,const uint8_t key[32],const uint8_t nonce[13],
                   const uint8_t *input,size_t n,uint8_t *output,uint8_t tag[8])=0;
  // Legacy providers remain conservative: a false result is an operation
  // failure unless a provider can prove it was only a rejected auth tag.
  virtual CcmResult ccmClassified(bool encrypt,const uint8_t key[32],const uint8_t nonce[13],
      const uint8_t *input,size_t n,uint8_t *output,uint8_t tag[8]) {
    return ccm(encrypt,key,nonce,input,n,output,tag)?CcmResult::Ok:CcmResult::BackendFailure;
  }
};
enum class DecryptResult:uint8_t { Ok,AuthenticationFailed,BackendFailure };
class PkiCrypto {
  friend class mt::pki::PkiSession;
  struct SessionKey {uint8_t bytes[32]{};bool valid=false;};
  PkiBackend &backend_;
  SessionKey *sessionKey_=nullptr;
  // Only a fixed-pair session can bind this private view, after independently
  // validating its generation, local scalar and enrolled peer on every use.
  PkiCrypto(PkiCrypto &owner,SessionKey &cache):backend_(owner.backend_),sessionKey_(&cache){}
  bool shared(const uint8_t secret[32],const uint8_t peer[32],uint8_t key[32]);
  static void nonce(uint32_t from,uint32_t id,uint32_t extra,uint8_t out[13]);
public:
  static constexpr size_t MaxPlaintext=227,Overhead=12;
  explicit PkiCrypto(PkiBackend &backend):backend_(backend){}
  PkiCrypto(const PkiCrypto &)=delete;
  PkiCrypto &operator=(const PkiCrypto &)=delete;
  // All secret keys here are explicit caller-owned, already-clamped X25519
  // scalars (as stock dh1 generates). Unclamped inputs fail. Nothing is retained.
  // The caller MUST prevent reuse of the complete (from,id,extraNonce) nonce
  // under the shared key. RFC 3610 sections 2.1/5 permits sequential nonces;
  // the production pair authority reserves durable counter ranges before use
  // and burns them on restart (RFC 5116 section 3.1). Legacy providers instead
  // fence every sent ID permanently and supply a CSPRNG extraNonce. No fallback
  // is generated here, and this primitive establishes no replay policy.
  bool encrypt(const uint8_t secret[32],const uint8_t peer[32],uint32_t from,
               uint32_t id,uint32_t extraNonce,const uint8_t *plain,size_t n,
               uint8_t *out,size_t cap,size_t &len);
  bool decrypt(const uint8_t secret[32],const uint8_t peer[32],uint32_t from,
               uint32_t id,const uint8_t *cipher,size_t n,uint8_t *out,size_t cap,size_t &len);
  DecryptResult decryptClassified(const uint8_t secret[32],const uint8_t peer[32],uint32_t from,
               uint32_t id,const uint8_t *cipher,size_t n,uint8_t *out,size_t cap,size_t &len);
  bool proof(const uint8_t secret[32],const uint8_t peer[32],uint32_t from,
             uint32_t to,uint32_t request,const uint8_t *routing,size_t n,uint8_t out[8]);
  bool verify(const uint8_t secret[32],const uint8_t peer[32],uint32_t from,
              uint32_t to,uint32_t request,const uint8_t *routing,size_t n,const uint8_t proof[8]);
};
}
