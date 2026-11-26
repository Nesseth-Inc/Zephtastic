// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "meshtastic/admin.pb.h"
#include "meshtastic/mesh.pb.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::pki::verification {
class KeyVerificationService;

// Stock Meshtastic manual PKI verification, with one bounded session and
// explicit I/O/identity adapters. This core never promotes a candidate key
// without local DO_VERIFY confirmation.
class KeyVerification {
public:
  enum class State:uint8_t {
    Idle,
    SenderStarted,
    SenderAwaitingNumber,
    SenderAwaitingConfirmation,
    ReceiverAwaitingHash1,
    ReceiverAwaitingConfirmation
  };

  struct Port {
    virtual ~Port()=default;
    virtual bool localIdentity(uint32_t &node,uint8_t publicKey[32])const {
      node=0;if(publicKey)memset(publicKey,0,32);return false;
    }
    // Only return an already pinned/known public key. Unknown-key discovery
    // remains a pending candidate and must not be reported here.
    virtual bool peerPublicKey(uint32_t node,uint8_t publicKey[32])const {
      (void)node;if(publicKey)memset(publicKey,0,32);return false;
    }
    virtual bool random(uint8_t *out,size_t length) {
      (void)out;(void)length;return false;
    }
    virtual bool sha256(const uint8_t *data,size_t length,uint8_t out[32]) {
      (void)data;(void)length;if(out)memset(out,0,32);return false;
    }
    virtual int stagePendingCandidate(uint32_t node,const uint8_t publicKey[32]) {
      (void)node;(void)publicKey;return -ENOTSUP;
    }
    virtual void clearPendingCandidate(uint32_t node,const uint8_t publicKey[32]) {
      (void)node;(void)publicKey;
    }
    virtual void releasePeerSession(uint32_t node,const uint8_t publicKey[32],bool pinned) {
      (void)node;(void)publicKey;(void)pinned;
    }
    // Called only after the matching final hash and explicit local approval.
    // The owner must recheck the current pin and persist the manually-verified
    // trust provenance atomically; no other path may call this.
    virtual int commitVerifiedPeer(uint32_t node,const uint8_t publicKey[32]) {
      (void)node;(void)publicKey;return -ENOTSUP;
    }
    // replyToId is nonzero only for the responder's M2 response. pki=true
    // requires explicitKey to match the exact peer key used by the protocol.
    virtual int send(uint32_t peer,uint32_t replyToId,
                     const meshtastic_KeyVerification &message,
                     bool pki,const uint8_t *explicitKey,size_t keyLength,
                     bool wantResponse,uint32_t now) {
      (void)peer;(void)replyToId;(void)message;(void)pki;(void)explicitKey;
      (void)keyLength;(void)wantResponse;(void)now;return -ENOTSUP;
    }
    virtual bool peerLongName(uint32_t node,char *out,size_t capacity)const {
      (void)node;if(out&&capacity)out[0]='\0';return false;
    }
    virtual void notify(const meshtastic_ClientNotification &event) {
      (void)event;
    }
  };

private:
  friend class KeyVerificationService;

  static constexpr uint32_t IdleTimeoutMs=60000;
  static constexpr uint32_t MaxSessionMs=180000;
  static constexpr uint32_t RemoteCooldownMs=60000;

  Port &port_;
  State state_=State::Idle;
  uint32_t peer_=0,localNode_=0,startMs_=0,lastProgressMs_=0,lastRemoteEndMs_=0;
  uint32_t securityNumber_=0;
  uint64_t nonce_=0;
  uint8_t localKey_[32]{},peerKey_[32]{},hash1_[32]{},hash2_[32]{};
  bool hasPeerKey_=false,peerKeyWasPinned_=false,remoteInitiated_=false,hasRemoteEnd_=false;

  static bool equal(const uint8_t *a,const uint8_t *b,size_t length);
  static void wipe(void *bytes,size_t length);
  bool snapshotLocal();
  bool resolvePeer(uint32_t peer,const uint8_t *candidate,bool candidatePresent);
  bool deriveHash1(uint32_t number,uint8_t out[32])const;
  bool deriveHash2(const uint8_t hash1[32],uint8_t out[32])const;
  void notifyNumberRequest();
  void notifyNumberInform();
  void notifyFinal(bool sender);
  void clearSession(bool recordRemoteEnd,uint32_t now);
  void update(uint32_t now);
  int sendInitial(uint32_t now);
  int acceptInitial(uint32_t from,uint32_t packetId,bool pki,
                    const uint8_t *envelopeKey,size_t envelopeKeyLength,
                    const meshtastic_KeyVerification &message,uint32_t now);
  int acceptResponse(uint32_t from,bool pki,const uint8_t *envelopeKey,
                     size_t envelopeKeyLength,
                     const meshtastic_KeyVerification &message,uint32_t now);
  int acceptFinal(uint32_t from,bool pki,const uint8_t *envelopeKey,
                  size_t envelopeKeyLength,
                  const meshtastic_KeyVerification &message,uint32_t now);
  int provideNumber(uint32_t number,uint32_t now);
  int approve(uint64_t nonce,uint32_t now);
  void cancel(uint32_t now);

public:
  explicit KeyVerification(Port &port):port_(port){}
  State state()const{return state_;}
  uint32_t peer()const{return peer_;}
  uint64_t nonce()const{return nonce_;}
  bool remoteInitiated()const{return remoteInitiated_;}

  // Called by local companion Admin adapter after outer-message canonical
  // admission. Returns zero only when the requested transition was accepted.
  int handleAdmin(const meshtastic_KeyVerificationAdmin &request,uint32_t now);

  // Call once from the radio owner after the ordinary RX admission/decode.
  // payload is the exact KeyVerification protobuf bytes; unsupported, stale,
  // unauthenticated or malformed messages have no state side effect.
  int receive(uint32_t from,uint32_t packetId,bool pki,
              const uint8_t *envelopeKey,size_t envelopeKeyLength,
              const uint8_t *payload,size_t payloadLength,uint32_t now);

  // Monotonic service-owner expiry; never performs radio work.
  void poll(uint32_t now);
};

} // namespace mt::pki::verification

