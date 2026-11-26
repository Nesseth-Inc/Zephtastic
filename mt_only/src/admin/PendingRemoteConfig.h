// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Config.h"

namespace mt::admin {

// Caller-computed SHA-256 (or stronger fixed 32-byte) fingerprint of the
// canonical Config bytes. This type contains no pointer or dynamic storage.
struct ConfigFingerprint {
  uint8_t bytes[32]{};
};

// One owner-serialized response-gated core-Config candidate. The Config copy
// is volatile RAM only: power loss loads the verified journal candidate as the
// normal live Config. This class performs no storage, RF, or Config apply.
class PendingRemoteConfig {
public:
  static constexpr uint32_t ReplyLifetimeMs=30000u;
  enum class State:uint8_t {
    Empty=0, Persisted, AwaitingNativeTx, ReadyApply, NeedsApplyOrRecovery
  };

  struct Completion {
    uint32_t peer=0;
    uint32_t request=0;
    uint32_t responseWireId=0;
    uint32_t durableGeneration=0;
    uint32_t stagedAt=0;
    ConfigFingerprint fingerprint{};
  };

  PendingRemoteConfig()=default;
  PendingRemoteConfig(const PendingRemoteConfig &)=delete;
  PendingRemoteConfig &operator=(const PendingRemoteConfig &)=delete;
  ~PendingRemoteConfig(){wipePending();}

  State state()const{return state_;}
  bool pending()const{return state_!=State::Empty;}

  // Stage is accepted only after SettingsEdit reports a verified durable save.
  // The owner must call actualResponseQueued only after the correlated response
  // itself has been admitted to the native TX queue.
  bool stage(const Config &candidate,bool durableVerified,uint32_t generation,
      const ConfigFingerprint &fingerprint,uint32_t peer,uint32_t request,
      uint32_t now) {
    if(state_!=State::Empty||!durableVerified||!candidate.valid()||
       peer<=1||peer>=0xfffffffeu||!request)return false;
    candidate_=candidate;fingerprint_=fingerprint;peer_=peer;request_=request;
    durableGeneration_=generation;stagedAt_=now;state_=State::Persisted;
    return true;
  }

  // Bind the exact endpoint/native packet identifier once. A duplicate bind
  // of the same ID is idempotent and does not extend the deadline.
  bool actualResponseQueued(uint32_t wireId,uint32_t now) {
    if(expire(now)||!wireId)return false;
    if(state_==State::AwaitingNativeTx)return responseWireId_==wireId;
    if(state_!=State::Persisted)return false;
    responseWireId_=wireId;state_=State::AwaitingNativeTx;
    return true;
  }

  // An exact successful completion allows the first live apply. Failure does
  // not undo the already durable candidate: retain it as recovery work.
  bool actualNativeTxResult(uint32_t wireId,int result,uint32_t now) {
    expire(now);
    if(state_!=State::AwaitingNativeTx||!wireId||
       wireId!=responseWireId_)return false;
    if(result!=0){state_=State::NeedsApplyOrRecovery;return false;}
    state_=State::ReadyApply;
    return true;
  }

  // Copies the candidate after successful native TX, or while resolving held
  // recovery work. A generation/fingerprint mismatch remains held. The caller
  // must wipe `candidate` after apply or on apply failure. A successful apply
  // is not complete until confirmResolved() verifies the same durable value.
  bool takeReadyApply(uint32_t now,uint32_t currentGeneration,
      const ConfigFingerprint &currentFingerprint,Config &candidate,
      Completion &completion) {
    completion={};
    expire(now);
    if(state_!=State::ReadyApply&&state_!=State::NeedsApplyOrRecovery)return false;
    if(currentGeneration!=durableGeneration_||
       !equal(fingerprint_,currentFingerprint)) {
      state_=State::NeedsApplyOrRecovery;return false;
    }
    candidate=candidate_;
    completion.peer=peer_;completion.request=request_;
    completion.responseWireId=responseWireId_;
    completion.durableGeneration=durableGeneration_;
    completion.stagedAt=stagedAt_;completion.fingerprint=fingerprint_;
    return true;
  }

  // Call only after the owner confirms that this durable candidate is now the
  // live configuration, or that reboot/recovery loaded that exact generation.
  // A mismatch leaves the slot held and continues blocking other mutations.
  bool confirmResolved(uint32_t generation,const ConfigFingerprint &fingerprint) {
    if(state_==State::Empty||generation!=durableGeneration_||
       !equal(fingerprint_,fingerprint)) {
      if(state_!=State::Empty)state_=State::NeedsApplyOrRecovery;
      return false;
    }
    wipePending();
    return true;
  }

  // Called on every owner poll, including while RX is paused, to prevent a
  // late native event from reviving a stale response/config association.
  // Deadline means the committed candidate needs owner recovery; it never
  // frees the slot or discards the candidate.
  bool poll(uint32_t now){return expire(now);}

private:
  void wipePending(){
    wipeConfig(candidate_);wipe(&fingerprint_,sizeof(fingerprint_));
    peer_=request_=responseWireId_=durableGeneration_=stagedAt_=0;
    state_=State::Empty;
  }

public:
  static void wipeConfig(Config &config){wipe(config.bytes,sizeof(config.bytes));}

private:
  static void wipe(void *memory,size_t size) {
    volatile uint8_t *p=static_cast<volatile uint8_t *>(memory);
    while(size--)*p++=0;
  }
  static bool equal(const ConfigFingerprint &a,const ConfigFingerprint &b) {
    uint8_t difference=0;
    for(size_t i=0;i<sizeof(a.bytes);++i)
      difference=uint8_t(difference|uint8_t(a.bytes[i]^b.bytes[i]));
    return difference==0;
  }
  bool expire(uint32_t now) {
    if(state_!=State::Empty&&uint32_t(now-stagedAt_)>=ReplyLifetimeMs) {
      state_=State::NeedsApplyOrRecovery;return true;
    }
    return false;
  }

  Config candidate_{};
  ConfigFingerprint fingerprint_{};
  uint32_t peer_=0,request_=0,responseWireId_=0,durableGeneration_=0,stagedAt_=0;
  State state_=State::Empty;
};

}  // namespace mt::admin
