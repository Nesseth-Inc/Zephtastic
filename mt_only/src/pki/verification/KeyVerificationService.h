// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "KeyVerification.h"
#include "../../Service.h"

namespace mt::pki::verification {

// Concrete production owner over the single serialized Service and its
// PrivateRuntime. The only externally bound operation is durable owner
// approval/pin persistence; missing binding fails closed.
class KeyVerificationService {
public:
  using CommitPeerFn=int(*)(void *,uint32_t,const uint8_t[32]);
private:
  struct PendingSend {
    uint8_t data[meshtastic_Data_size]{};
    uint8_t explicitKey[32]{};
    size_t length=0,keyLength=0;
    uint8_t owner=Endpoint::LocalUiOwner;
    uint32_t peer=0,replyTo=0,hop=0,randomSeed=0,queuedAt=0,lastAttempt=0;
    uint64_t nonce=0;
    bool pki=false,active=false,afterSessionApproval=false;
  } pending_{};
  static constexpr uint32_t PendingRetryMs=250;
  static constexpr uint32_t ApprovedSendLimitMs=10000;
  class Adapter final:public KeyVerification::Port {
    KeyVerificationService &owner_;
  public:
    explicit Adapter(KeyVerificationService &owner):owner_(owner){}
    bool localIdentity(uint32_t &,uint8_t [32])const override;
    bool peerPublicKey(uint32_t,uint8_t [32])const override;
    bool random(uint8_t *,size_t)override;
    bool sha256(const uint8_t *,size_t,uint8_t [32])override;
    int stagePendingCandidate(uint32_t,const uint8_t [32])override;
    void clearPendingCandidate(uint32_t,const uint8_t [32])override;
    void releasePeerSession(uint32_t,const uint8_t [32],bool)override;
    int commitVerifiedPeer(uint32_t,const uint8_t [32])override;
    int send(uint32_t,uint32_t,const meshtastic_KeyVerification &,bool,
             const uint8_t *,size_t,bool,uint32_t)override;
    bool peerLongName(uint32_t,char *,size_t)const override;
    void notify(const meshtastic_ClientNotification &)override;
  } adapter_;
  KeyVerification core_;
  Service &service_;
  CommitPeerFn commit_=nullptr;void *commitContext_=nullptr;
  uint8_t owner_=Endpoint::LocalUiOwner;
  meshtastic_ClientNotification notifications_[2]{};
  uint8_t notificationRead_=0,notificationCount_=0;
  int transmit(const PendingSend &,uint32_t now);
  bool defer(const uint8_t *data,size_t length,uint32_t peer,uint32_t replyTo,
             bool pki,const uint8_t *explicitKey,size_t keyLength,uint32_t hop,
             uint32_t randomSeed,uint64_t nonce,uint32_t now);
  void clearPending();
  void notifyPendingFailure();
  void retryPending(uint32_t now);
public:
  explicit KeyVerificationService(Service &service):adapter_(*this),core_(adapter_),service_(service){}
  void bindCommit(CommitPeerFn fn,void *context){commit_=fn;commitContext_=context;}
  int admin(uint8_t owner,const meshtastic_KeyVerificationAdmin &,uint32_t now);
  int receive(const KeyVerificationReceive &,uint32_t now);
  void poll(uint32_t now);
  void disconnect(uint8_t owner,uint32_t now);
  bool takeNotification(meshtastic_ClientNotification &out);
  KeyVerification::State state()const{return core_.state();}
  uint32_t peer()const{return core_.peer();}
  uint64_t nonce()const{return core_.nonce();}
};

} // namespace mt::pki::verification
