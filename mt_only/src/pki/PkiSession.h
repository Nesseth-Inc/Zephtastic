// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiCrypto.h"
#include "PkiAck.h"
#include "PkiKeyProvider.h"
namespace mt::pki {
struct NonceAuthority {
  virtual ~NonceAuthority()=default;
  // Must irreversibly reserve a unique full CCM nonce in the stable-public-key/
  // local-node domain across sessions and restarts. Legacy providers fence IDs
  // permanently and supply a CSPRNG extra nonce. Explicit pair authorities may
  // reuse retired IDs only with a durable, never-repeated extra nonce. No fallback.
  // Failure may already have burned the reservation. Basic providers also use
  // the session's RAM attempt history; durable providers own that decision.
  // Runtime providers remain explicitly injected; no default entropy or credentials.
  virtual bool reserve(KeyRef,const uint8_t localPublic[32],uint32_t local,
                       uint32_t packetId,uint32_t &extra)=0;
  // Explicit trusted capability: reserve rejects every previously burned ID
  // durably, including across sessions/restarts. Basic providers retain the
  // conservative RAM history. This flag supplies no storage or fallback.
  virtual bool persistentIds()const{return false;}
  // Separate production capability: the authority durably prevents full CCM
  // nonce-pair reuse, even when a retired packet ID recurs. An active in-flight
  // packet ID still cannot recur. Legacy ID-fenced providers keep the flag off.
  virtual bool persistentNoncePairs()const{return false;}
};
struct PkiFrame {
  uint32_t from=0,to=0,id=0;
  size_t size=0;
  uint8_t bytes[239]{};
};
enum class SessionResult {
  Ok,NotReady,InvalidArgument,NoSlot,NonceReused,NonceExhausted,NonceUnavailable,
  KeyUnavailable,CryptoFailure,PeerMismatch,Replay,RxExhausted,NotPending,AuthenticationFailed
};
// Inert unless an owner supplies all providers/bindings. Single-thread caller;
// no transport, delivery receipt, peer discovery, persistence or default identity.
class PkiSession {
  enum class State { Empty,Ready,Closed };
  mutable experimental::PkiCrypto::SessionKey sharedKey_{};
  experimental::PkiCrypto crypto_;KeyProvider &keys_;NonceAuthority &nonces_;
  KeyRef key_{};uint32_t local_=0,peer_=0;
  uint8_t localPublic_[32]{},peerPublic_[32]{};
  State state_=State::Empty;
  struct Slot {bool active=false;PkiFrame frame{};};
  Slot slots_[4]{};
  uint32_t sent_[32]{},received_[32]{};size_t sentCount_=0,receivedCount_=0;
  bool load(uint8_t scalar[32]);
public:
  static constexpr size_t PendingLimit=4,NonceLimit=32,ReceiveLimit=32;
  PkiSession(experimental::PkiCrypto &crypto,KeyProvider &keys,NonceAuthority &nonces):
    crypto_(crypto,sharedKey_),keys_(keys),nonces_(nonces){}
  static void *operator new(size_t,void *storage)noexcept{return storage;}
  ~PkiSession(){close();}
  PkiSession(const PkiSession &)=delete;
  PkiSession &operator=(const PkiSession &)=delete;
  // Local node is trusted caller input, not derived/validated against key metadata.
  // Serialize all calls with provider mutations; callbacks must not reenter.
  // localPublic must match the explicitly supplied private handle. peerPublic
  // must ALREADY be bound authoritatively to peer; this class cannot prove it.
  bool open(KeyRef,uint32_t local,const uint8_t localPublic[32],
            uint32_t peer,const uint8_t trustedPeerPublic[32]);
  bool ready()const;
  void close(); // Terminal for this object, cannot reopen/reset nonce history.
  // Input is exact encoded Data, not plain text. IDs burn even on backend/key/
  // entropy failure. Malformed/full-queue attempts consume no reservation;
  // durable admission refusal before a write may leave the ID unused.
  SessionResult send(uint32_t id,const uint8_t *data,size_t n,PkiFrame &out);
  // Exact cached frame including original destination; no reencrypt/key/RNG call.
  SessionResult retry(uint32_t id,PkiFrame &out)const;
  bool retire(uint32_t id); // Frees queue slot, never frees a nonce reservation.
  // Marks replay only after successful authenticated decrypt. Never evicts replay
  // entries: limit fails closed. Cross-session replay persistence is caller work.
  SessionResult receive(const PkiFrame &,uint8_t *out,size_t cap,size_t &len);
  // Authentication only, for a durable replay owner. Does not admit/deliver or
  // mutate the in-memory replay list. Caller must commit replay before delivery.
  SessionResult authenticate(const PkiFrame &,uint8_t *out,size_t cap,size_t &len);
  bool attachProof(uint32_t request,const uint8_t *routing,size_t n,uint8_t *out,size_t cap,size_t &len);
  experimental::AckProofVerdict verifyProof(uint32_t from,uint32_t to,uint32_t request,
    const uint8_t *routing,size_t n,uint8_t *out,size_t cap,size_t &len);

};
}
