// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
#include "../modules/StoreForwardPacket.h"
#endif
#include "../Endpoint.h"
#include "MtNodeInfo.h"
#include "PrivateReplyDiagnostics.h"
#include "meshtastic/mesh.pb.h"
#include <errno.h>
namespace mt::pki {
// Read-only stock transport selection. UnknownNoKey is the only automatic
// channel-fallback disposition for an otherwise PKI-eligible unicast.
enum class DestinationDisposition:uint8_t {
  Unavailable,PinnedActive,UnknownNoKey,KnownUnpinned,Revoked,KeyMismatch
};
// Classification of one hash-zero private receive attempt. Unauthenticated is
// emitted only for a healthy unknown sender or an explicitly classified
// rejected authentication tag. Legacy boolean backends and uncertain cleanup,
// storage, key or replay authority remain Hold.
enum class ReceiveClassification:uint8_t {
  NotPki,Unauthenticated,AuthenticatedConsumed,Hold
};
// Borrowed, single Service owner under the existing service lock. Normal
// startup supplies nullptr. A configured-but-unready provider NEVER falls back
// to Public. Each RX event is authenticated/admitted once then fanout twice.
struct PrivateEvent {
  uint32_t from=0,to=0,id=0;uint8_t publicKey[32]{};
  uint8_t data[227]{};size_t size=0;
#if defined(CONFIG_MT_RF_ADMIN_GET)
  uint8_t adminOwners=0;
#endif
};
struct KeyVerificationReceive {
  uint32_t from=0,id=0;bool pki=false;uint8_t publicKey[32]{};
  uint8_t payload[meshtastic_KeyVerification_size]{};size_t size=0;
};
struct PrivateRuntime {
  virtual ~PrivateRuntime()=default;
  virtual uint32_t peer()const=0;
  virtual void hopLimit(uint8_t){}
  virtual void deviceRole(uint8_t){}
  virtual void ownerUnmessagable(bool){}
  virtual void rebroadcastMode(uint8_t){}
  virtual PrivateReplyDiagnostics replyDiagnostics()const{return {};}
  virtual bool ready(uint32_t local)const=0;
  // Public identity and its global TX nonce authority can remain usable while
  // the private contact set is empty. Existing providers keep their previous
  // semantics unless they explicitly support this split readiness state.
  virtual bool publicReady(uint32_t local)const{return ready(local);}
  virtual bool dormant(uint32_t)const{return false;}
  virtual bool publicIdentity(uint8_t out[32])const=0;
  // Snapshot only an already active operator-pinned PUBLIC peer identity.
  // No discovery trust, persistence or secret access; failure preserves out.
  virtual bool peerPublicIdentity(uint32_t,uint8_t [32])const{return false;}
  // Narrow manual-key-verification lane. Implementations must bind randomness
  // and SHA256 to the active crypto provider, use the shared durable nonce
  // authority for every PKI frame, and keep candidate sessions isolated from
  // ordinary application admission.
  virtual bool keyVerificationRandom(uint8_t *,size_t){return false;}
  virtual bool keyVerificationSha256(const uint8_t *,size_t,uint8_t [32]){return false;}
  virtual bool keyVerificationLocalIdentity(uint32_t &,uint8_t [32])const{return false;}
  virtual bool keyVerificationPeerKey(uint32_t,uint8_t [32])const{return false;}
  virtual int keyVerificationStageCandidate(uint32_t,const uint8_t [32]){return -ENOTSUP;}
  virtual void keyVerificationClearCandidate(uint32_t,const uint8_t [32]){}
  virtual void keyVerificationReleasePeer(uint32_t,const uint8_t [32]){}
  virtual bool keyVerificationCandidateMatches(uint32_t)const{return false;}
  // Dedicated correlated Routing-proof lane for the exact active candidate
  // session. It never falls back to ordinary peer/application admission.
  virtual bool keyVerificationProof(Endpoint &,const uint8_t *,size_t,
      const bench::RouteMetadata &,uint32_t){return false;}
  virtual int keyVerificationSend(Endpoint &,bench::MtCodec &,uint32_t,uint32_t,const uint8_t *,size_t,
      bool,const uint8_t *,size_t,unsigned,bool,uint32_t,uint32_t){return -ENOTSUP;}
  virtual bool takeKeyVerificationReceive(KeyVerificationReceive &out){out={};return false;}
  virtual bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t model,
    uint32_t to,uint32_t id,bool wantResponse,uint32_t request,bench::Frame &)=0;
  virtual bool broadcast(bench::MtCodec &,uint32_t,const uint8_t *,size_t,bench::Frame &){return false;}
  // Explicit licensed channel origination. Uses the existing signer/nonce
  // authority, but emits public plaintext and never a PKI ciphertext claim.
  virtual bool licensedUnicast(bench::MtCodec &,uint32_t,uint32_t,
    const uint8_t *,size_t,bench::Frame &){return false;}
  // Unsigned generic channel applications still share the identity's durable
  // outgoing-ID domain. Old providers fail closed; this is never PKI fallback.
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE)
  virtual int stockReply(Endpoint &,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,bool,uint32_t,uint32_t){return -ENOTSUP;}
#endif
  virtual bool encodePublicApplication(bench::MtCodec &,uint32_t,uint32_t,
    const uint8_t *,size_t,bench::Frame &){return false;}
  // Opt-in public control encoding under the owning Service's verified pause.
  // An older provider must refuse rather than bypass outgoing-ID reservation.
  virtual bool encodeChannelControl(bench::MtCodec &,uint32_t,uint32_t,
    const uint8_t *,size_t,bench::Frame &){return false;}
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  // Bounded generated shared-channel history only, never PKI or a signature.
  virtual bool encodeStoreForward(bench::MtCodec &,const modules::StoreForwardPacket &,
                                  uint32_t,bench::Frame &){return false;}
#endif
  virtual void freeze()=0;
  virtual bool acceptsPeerKey(uint32_t,const uint8_t *,size_t)const{return true;}
  virtual bool matchesPeer(uint32_t peer)const=0;
  virtual DestinationDisposition destinationDisposition(uint32_t,const uint8_t *,size_t)const {
    return DestinationDisposition::Unavailable;
  }
  virtual bool revoke(uint32_t peer)=0;
  virtual void clearEvent()=0;
  virtual void sweep(Endpoint &)=0;
  virtual int submit(Endpoint &,uint8_t owner,uint32_t to,uint32_t id,
    const uint8_t *data,size_t size,unsigned hop,bool reliable,
    const uint8_t *explicitKey,size_t keySize,bool explicitPki,uint32_t now)=0;
#if defined(CONFIG_MT_RF_ADMIN_GET)
  // Separate opt-in lane for canonical read-only Admin GET. Providers must
  // keep the ordinary application port policy closed.
  virtual int submitAdminGet(Endpoint &,uint8_t owner,uint32_t to,uint32_t id,
    const uint8_t *data,size_t size,unsigned hop,bool reliable,
    const uint8_t *explicitKey,size_t keySize,bool explicitPki,uint32_t now) {
    (void)owner;(void)to;(void)id;(void)data;(void)size;(void)hop;(void)reliable;
    (void)explicitKey;(void)keySize;(void)explicitPki;(void)now;return -ENOTSUP;
  }
  // Complete the deferred authenticated Admin SET only after the Service
  // owner has returned to RX, run its setter callback, and reacquired its
  // verified storage pause. Default providers fail closed.
  virtual bool respondPrivateAdminMutation(Endpoint &,uint32_t local,uint32_t peer,
    const uint8_t key[32],uint32_t request,uint8_t responseHop,bool reliable,
    uint32_t routingError,uint32_t now,uint32_t random) {
    (void)local;(void)peer;(void)key;(void)request;(void)responseHop;(void)reliable;
    (void)routingError;(void)now;(void)random;return false;
  }
#endif
  // Called only while the service's dedicated radio-storage pause is verified.
  virtual void receive(Endpoint &,bench::MtCodec &,const uint8_t *,size_t,
    uint32_t now,uint32_t random,bool armed)=0;
  // Existing void receive remains compatible. Default providers conservatively
  // consume/hold rather than authorizing shared-channel fallback.
  virtual ReceiveClassification receiveClassified(Endpoint &endpoint,bench::MtCodec &codec,
    const uint8_t *frame,size_t size,uint32_t now,uint32_t random,bool armed) {
    receive(endpoint,codec,frame,size,now,random,armed);
    return ReceiveClassification::Hold;
  }
  // One-shot authenticated/admitted peer observation produced by the most
  // recent receive(). Providers without this capability remain source-compatible.
  virtual bool takeAuthenticatedReceive(uint32_t &from){from=0;return false;}
  virtual void proof(Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t now)=0;
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE) || defined(CONFIG_MT_DROPZONE)
  virtual const PrivateEvent *moduleEvent(const uint8_t *,size_t){return nullptr;}
#endif
  virtual bool event(uint8_t owner,const uint8_t *frame,size_t n,PrivateEvent &)=0;
};
}
