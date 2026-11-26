// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PrivateRuntime.h"
#include "PkiIdentity.h"
#include "PkiSession.h"
#include "PkiReplayAuthority.h"
#include "PkiSigner.h"
#include "PrivateDuplicateAck.hpp"
#include "../DeviceRolePolicy.h"
namespace mt::pki {
// Peer-independent public producers shared by a ready PkiRuntime child and
// the bound zero-peer MultiPeerRuntime. The caller proves the corresponding
// provider readiness first. These use the same identity, nonce authority and
// signer; they create no session and retain no state.
struct PkiPublicProducer {
  static bool canonicalPositionBroadcast(const uint8_t *,size_t);
  static bool bootstrap(KeyProvider &,KeyRef,const IdentityBinding &,NonceAuthority &,
    SigningEntropy &,bench::MtCodec &,const bench::MtNodeRecord &,uint8_t model,
    uint8_t role,bool ownerUnmessagable,uint32_t to,uint32_t id,bool wantResponse,
    uint32_t request,bench::Frame &);
  static bool broadcast(KeyProvider &,KeyRef,const IdentityBinding &,NonceAuthority &,
    SigningEntropy &,bench::MtCodec &,uint32_t id,const uint8_t *,size_t,bench::Frame &);
  static bool licensedUnicast(KeyProvider &,KeyRef,const IdentityBinding &,NonceAuthority &,
    SigningEntropy &,bench::MtCodec &,uint32_t to,uint32_t id,const uint8_t *,size_t,bench::Frame &);
  static bool encodePublicApplication(KeyProvider &,KeyRef,const IdentityBinding &,
    NonceAuthority &,bench::MtCodec &,uint32_t to,uint32_t id,const uint8_t *,size_t,bench::Frame &);
  static bool encodeChannelControl(KeyProvider &,KeyRef,const IdentityBinding &,
    NonceAuthority &,bench::MtCodec &,uint32_t to,uint32_t id,const uint8_t *,size_t,bench::Frame &);
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  static bool encodeStoreForward(KeyProvider &,KeyRef,const IdentityBinding &,
    NonceAuthority &,bench::MtCodec &,const modules::StoreForwardPacket &,uint32_t,
    bench::Frame &);
#endif
};
// One bounded candidate/replay mailbox shared by the MultiPeer coordinator's
// four hot sessions. Candidate material is never duplicated per pinned peer.
struct PkiVerificationContext {
  alignas(PkiSession) uint8_t sessionStorage[sizeof(PkiSession)]{};
  PkiSession *session=nullptr;
  uint32_t peer=0;uint8_t publicKey[32]{};bool peerPinned=false;
  PrivateDuplicateAck duplicateAck;
  KeyVerificationReceive receive{};bool receiveReady=false;
};
// One explicit pinned contact per coordinator, four pending ciphertexts. No
// startup instance or provisioning endpoint. CRC node binding is consistency,
// not collision-resistant peer authorization: the complete pinned key is checked.
class PkiRuntime final:public PrivateRuntime {
  experimental::PkiCrypto &crypto_;
  experimental::PkiBackend &backend_;
  IdentityJournal &identity_;PeerReplayAuthority &peers_;NonceAuthority &nonces_;SigningEntropy *signing_;
  PkiSession session_;IdentityBinding binding_{};KeyRef key_{};
  PkiVerificationContext *verification_=nullptr;
  bool verificationOwner_=false;
  uint32_t peer_=0;uint8_t peerPublic_[32]{};
  bool opened_=false,frozen_=false,ownerUnmessagable_=false;uint8_t deviceRole_=1,rebroadcastMode_=0,hopLimit_=3;
  struct Pending {uint32_t id=0;uint8_t digest[32]{};uint8_t hop=0;bool reliable=false;
#if defined(CONFIG_MT_RF_ADMIN_GET)
    bool adminGet=false;
#endif
  };
  Pending pending_[4]{};
  PrivateEvent event_{};bench::Frame eventFrame_{};uint8_t eventOwners_=0;
  PrivateDuplicateAck duplicateAck_;
  PrivateReplyDiagnostics replyDiagnostics_{};
  uint32_t authenticatedRxFrom_=0;
  ReceiveClassification receiveClassification_=ReceiveClassification::Hold;
  bool application(const uint8_t *,size_t)const;
  bool frame(uint32_t,const uint8_t *,size_t,unsigned,bool,bench::Frame &);
 #if defined(CONFIG_MT_RF_ADMIN_GET)
  int submitImpl(Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,bool,
    const uint8_t *,size_t,bool,uint32_t,bool);
#endif
  bool reply(Endpoint &,bench::MtCodec &,uint32_t,unsigned,uint32_t,uint32_t);
  bool reply(Endpoint &,bench::MtCodec &,PkiSession &,uint32_t,uint32_t,unsigned,uint32_t,uint32_t);
  void metricsReply(Endpoint &,uint32_t,unsigned,uint32_t,uint32_t,bool,bool localStats=false,bool environment=false,bool power=false,uint8_t extra=0);
  void positionReply(Endpoint &,uint32_t,unsigned,uint32_t,uint32_t,bool);
  void neighborReply(Endpoint &,uint32_t,unsigned,uint32_t,uint32_t,bool);
#if defined(CONFIG_MT_PAXCOUNTER)
  void paxReply(Endpoint &,uint32_t,unsigned,uint32_t,uint32_t,bool);
#endif
public:
  void hopLimit(uint8_t value)override{if(value<=7)hopLimit_=value;}
  void deviceRole(uint8_t role)override{if(DeviceRolePolicy::supportedRole(role))deviceRole_=role;}
  void ownerUnmessagable(bool value)override{ownerUnmessagable_=value;}
  void rebroadcastMode(uint8_t mode)override{rebroadcastMode_=mode;}
  // Explicit caller-owned aligned storage; Zephyr's minimal C++ library has
  // no placement-new declaration. This class-local overload allocates nothing.
  static void *operator new(size_t,void *storage)noexcept{return storage;}
  PkiRuntime(experimental::PkiCrypto &crypto,experimental::PkiBackend &backend,
    IdentityJournal &identity,NonceAuthority &nonces,PeerReplayAuthority &peers,SigningEntropy *signing=nullptr,
    PkiVerificationContext *verification=nullptr):
    crypto_(crypto),backend_(backend),identity_(identity),peers_(peers),nonces_(nonces),signing_(signing),
    session_(crypto,identity,nonces),verification_(verification){}
  ~PkiRuntime()override{freeze();}
  bool open(uint32_t local,uint32_t peer);
  // A dedicated owner for first-contact key verification. It binds the same
  // active local identity and shared nonce authority without fabricating a pin.
  bool openVerification(uint32_t local);
  uint32_t peer()const override{return peer_;}
  PrivateReplyDiagnostics replyDiagnostics()const override{return replyDiagnostics_;}
  bool ready(uint32_t local)const override;
  bool publicIdentity(uint8_t out[32])const override;
  bool peerPublicIdentity(uint32_t peer,uint8_t out[32])const override {
    if(!out||peer!=peer_||!ready(binding_.node))return false;
    memcpy(out,peerPublic_,32);return true;
  }
  bool keyVerificationRandom(uint8_t *,size_t)override;
  bool keyVerificationSha256(const uint8_t *,size_t,uint8_t [32])override;
  bool keyVerificationLocalIdentity(uint32_t &,uint8_t [32])const override;
  bool keyVerificationPeerKey(uint32_t,uint8_t [32])const override;
  int keyVerificationStageCandidate(uint32_t,const uint8_t [32])override;
  void keyVerificationClearCandidate(uint32_t,const uint8_t [32])override;
  void keyVerificationReleasePeer(uint32_t peer,const uint8_t [32])override;
  bool keyVerificationCandidateMatches(uint32_t peer)const override {
    return verification_&&verification_->session&&verification_->session->ready()&&verification_->peer==peer;
  }
  int keyVerificationSend(Endpoint &,bench::MtCodec &,uint32_t,uint32_t,const uint8_t *,size_t,
      bool,const uint8_t *,size_t,unsigned,bool,uint32_t,uint32_t)override;
  bool takeKeyVerificationReceive(KeyVerificationReceive &)override;
  // Candidate-only proof lane. Returns true only when the canonical proof
  // matches a live reliable Endpoint job to the exact staged candidate.
  bool keyVerificationProof(Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t)override;
  bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,
    uint32_t,uint32_t,bool,uint32_t,bench::Frame &)override;
  bool broadcast(bench::MtCodec &,uint32_t,const uint8_t *,size_t,bench::Frame &)override;
  bool licensedUnicast(bench::MtCodec &,uint32_t,uint32_t,const uint8_t *,size_t,bench::Frame &)override;
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE)
  int stockReply(Endpoint &,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,bool,uint32_t,uint32_t)override;
#endif
  bool encodePublicApplication(bench::MtCodec &,uint32_t,uint32_t,const uint8_t *,size_t,bench::Frame &)override;
  bool encodeChannelControl(bench::MtCodec &,uint32_t,uint32_t,const uint8_t *,size_t,bench::Frame &)override;
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  bool encodeStoreForward(bench::MtCodec &,const modules::StoreForwardPacket &,uint32_t,bench::Frame &)override;
#endif
  void freeze()override;
  bool acceptsPeerKey(uint32_t peer,const uint8_t *key,size_t n)const override {return peer!=peer_||!n||(n==32&&key&&!memcmp(key,peerPublic_,32));}
  bool matchesPeer(uint32_t peer)const override{return opened_&&peer==peer_;}
  DestinationDisposition destinationDisposition(uint32_t node,const uint8_t *hint,
      size_t hintSize)const override {
    if(node<=1||node>=0xfffffffeu||node==binding_.node||(hintSize&&(!hint||hintSize!=32)))
      return DestinationDisposition::KeyMismatch;
    if(!publicReady(binding_.node))return DestinationDisposition::Unavailable;
    if(node==peer_) {
      if(hintSize&&memcmp(hint,peerPublic_,32))return DestinationDisposition::KeyMismatch;
      if(peers_.revoked(node))return DestinationDisposition::Revoked;
      return ready(binding_.node)&&peers_.active(node,peerPublic_)?
        DestinationDisposition::PinnedActive:DestinationDisposition::Unavailable;
    }
    if(peers_.revoked(node))return DestinationDisposition::Revoked;
    uint8_t known[32]{};const bool found=peers_.lookup(node,known);
    const bool mismatch=found&&hintSize&&memcmp(hint,known,sizeof(known));
    memset(known,0,sizeof(known));
    if(mismatch)return DestinationDisposition::KeyMismatch;
    return found?DestinationDisposition::KnownUnpinned:DestinationDisposition::UnknownNoKey;
  }
  bool revoke(uint32_t peer)override;
  void clearEvent()override;
  void sweep(Endpoint &)override;
  int submit(Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,bool,
    const uint8_t *,size_t,bool,uint32_t)override;
#if defined(CONFIG_MT_RF_ADMIN_GET)
  int submitAdminGet(Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,bool,
    const uint8_t *,size_t,bool,uint32_t)override;
  bool respondPrivateAdminMutation(Endpoint &,uint32_t,uint32_t,const uint8_t[32],uint32_t,
    uint8_t,bool,uint32_t,uint32_t,uint32_t)override;
#endif
  void receive(Endpoint &,bench::MtCodec &,const uint8_t *,size_t,uint32_t,uint32_t,bool)override;
  ReceiveClassification receiveClassified(Endpoint &,bench::MtCodec &,const uint8_t *,size_t,
      uint32_t,uint32_t,bool)override;
  bool takeAuthenticatedReceive(uint32_t &from)override {from=authenticatedRxFrom_;authenticatedRxFrom_=0;return from!=0;}
  void proof(Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t)override;
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE) || defined(CONFIG_MT_DROPZONE)
  const PrivateEvent *moduleEvent(const uint8_t *,size_t)override;
#endif
  bool event(uint8_t,const uint8_t *,size_t,PrivateEvent &)override;
};
}
