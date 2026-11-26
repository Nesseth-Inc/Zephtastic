// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiStorage.h"
#include "MultiPeerRuntime.h"
#include "../Service.h"

namespace mt::pki {
// Boot composition plus one serialized live-pin publication bridge. Caller
// holds serviceLock throughout and supplies objects with static lifetime. No
// key generation or heap use; live adoption requires an already committed pin.
class PkiStartup {
public:
  enum class State:uint8_t {New,Empty,Prepared,Active,Dormant,Held};
private:
  PkiStorage &storage_;experimental::PkiCrypto &crypto_;
  experimental::PkiBackend &backend_;SigningEntropy &signing_;
  State state_=State::New;IdentityBinding binding_{};
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  RuntimePinDirectory peers_;
  ExpandedContactFlash *progress_=nullptr;
#else
  PkiPeerSet peers_;
#endif
  alignas(MultiPeerRuntime) uint8_t memory_[sizeof(MultiPeerRuntime)]{};
  MultiPeerRuntime *runtime_=nullptr;
public:
  PkiStartup(PkiStorage &storage,experimental::PkiCrypto &crypto,
    experimental::PkiBackend &backend,SigningEntropy &signing):
    storage_(storage),crypto_(crypto),backend_(backend),signing_(signing){}
  PkiStartup(const PkiStartup &)=delete;PkiStartup &operator=(const PkiStartup &)=delete;
  ~PkiStartup(){if(runtime_)runtime_->~MultiPeerRuntime();}
  State state()const{return state_;}
  // A verified empty store retains the hardware identity on the public XIAO
  // companion. The T096 stock PKI profile requires a verified local binding.
  bool permitsCompanionRadioBoot(bool allowEmptyPublic)const {
    return state_==State::Active||state_==State::Dormant||
      (allowEmptyPublic&&state_==State::Empty);
  }
  // Initial boot snapshot only; this metric intentionally does not track later
  // live contact adoption through the owner-loop bridge below.
  size_t loadedPeers()const{return state_==State::Active?peers_.count():0;}
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  void checkpoint(ExpandedContactFlash &flash){progress_=&flash;}
#endif
  bool prepare(uint32_t &node) {
    if(state_!=State::New)return false;
    const int result=storage_.prepare();
    if(result==-ENOENT&&storage_.state()==PkiStorage::State::Empty){state_=State::Empty;return true;}
    if(result||!storage_.preparedBinding(binding_)
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
       ||!storage_.preparedRuntimePeers(peers_)
#else
       ||!storage_.preparedPeers(peers_)
#endif
      ) {
      state_=State::Held;(void)storage_.close();return false;
    }
    node=binding_.node;state_=State::Prepared;return true;
  }
  bool hold(Service &service) {
    if(runtime_)runtime_->freeze();
    (void)storage_.close();state_=State::Held;service.holdPrivateStartup();return false;
  }
  // Readiness is a non-consuming boot snapshot. Never fold transient entropy
  // starvation/lock contention into the running runtime's identity authority.
  bool activate(Service &service,bool entropyReady) {
    if(state_==State::Empty)return true;
    if(state_!=State::Prepared||!entropyReady||!backend_.healthy()||
       service.route.identity()!=binding_.node||!service.nodes.ready()||
       !service.storageQuiescent()||storage_.activate(binding_.node))return hold(service);
    auto *identity=storage_.runtimeIdentity();if(!identity)return hold(service);
    runtime_=new(memory_) MultiPeerRuntime(crypto_,backend_,*identity,storage_,storage_,&signing_);
    if(!runtime_->open(binding_.node,peers_
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
                      ,progress_
#endif
                      )||!service.bindPrivateRuntime(*runtime_))return hold(service);
    state_=peers_.count()?State::Active:State::Dormant;return true;
  }
  // Owner-loop bridge after durable storage commit. This neither reads nor
  // writes storage and never recreates the already-running runtime/provider.
  bool adoptPersistedPeer(const PkiPeerPin &pin) {
    if((state_!=State::Active&&state_!=State::Dormant)||!runtime_)return false;
    if(!runtime_->adoptPersistedPeer(pin))return false;
    state_=State::Active;return true;
  }
};
}
