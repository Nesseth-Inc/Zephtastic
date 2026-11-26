// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiRuntime.h"
#include "PkiPeerSet.h"
#include "PkiCapacity.h"
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
#include "ExpandedContacts.h"
#endif
#include "PrivateApplication.h"
#include "monocypher.h"
#include "../DeviceRolePolicy.h"

namespace mt::pki {
// One serialized Service owner; contact count is independent of the four hot
// session slots. Owner mutations require the caller to drain accepted Endpoint
// work first; directory changes never alter durable nonce/replay history.
class MultiPeerRuntime final:public PrivateRuntime {
  experimental::PkiCrypto &crypto_;experimental::PkiBackend &backend_;
  IdentityJournal &identity_;PeerReplayAuthority &peers_;NonceAuthority &nonces_;SigningEntropy *signing_;
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  using Pins=RuntimePinDirectory;
  using ContactIndex=int16_t;
#else
  using Pins=PkiPeerSet;
  using ContactIndex=int8_t;
#endif
  IdentityBinding binding_{};KeyRef key_{};Pins pins_;
  PkiVerificationContext verification_{};
  alignas(PkiRuntime) uint8_t verificationMemory_[sizeof(PkiRuntime)]{};
  PkiRuntime *verificationRuntime_=nullptr;
  alignas(PkiRuntime) uint8_t memory_[RuntimeSlots][sizeof(PkiRuntime)]{};
  PkiRuntime *children_[RuntimeSlots]{};
  ContactIndex contacts_[RuntimeSlots]{-1,-1,-1,-1};
  bool active_[Pins::Limit]{},revoked_[Pins::Limit]{};
  PrivateReplyDiagnostics retiredDiagnostics_{};
  bool opened_=false,frozen_=false,cancelled_=false;
  uint32_t local_=0;int selected_=-1;uint8_t nextVictim_=0;uint32_t authenticatedRxFrom_=0;
  ReceiveClassification receiveClassification_=ReceiveClassification::Hold;
  uint8_t role_=1,rebroadcast_=0,hop_=3;bool ownerUnmessagable_=false;
  static bool validNodeId(uint32_t node){return node>=2&&node<=0xfffffffdu;}
  int index(uint32_t node)const {
    for(size_t i=0;i<pins_.count();++i)if(pins_.at(i)->node==node)return int(i);
    return -1;
  }
  int firstContact()const {
    for(size_t i=0;i<pins_.count();++i)if(active_[i])return int(i);
    return -1;
  }
  int firstChild()const {
    for(size_t i=0;i<RuntimeSlots;++i)if(children_[i]&&contacts_[i]>=0&&active_[unsigned(contacts_[i])])return int(i);
    return -1;
  }
  int cached(unsigned contact)const {
    for(size_t i=0;i<RuntimeSlots;++i)if(children_[i]&&contacts_[i]==int(contact))return int(i);
    return -1;
  }
  bool candidateShape(const PkiPeerPin &pin,int replacement)const {
    if(!validNodeId(pin.node)||pin.node==local_||
       !memcmp(pin.publicKey,binding_.publicKey,32))return false;
    for(size_t i=0;i<pins_.count();++i){
      const auto *old=pins_.at(i);
      if(old&&int(i)!=replacement&&!memcmp(old->publicKey,pin.publicKey,32))return false;
    }
    uint8_t ed[32]{};
    const bool valid=bench::MtSignature::admissiblePublicKey(pin.publicKey,ed);
    crypto_wipe(ed,sizeof(ed));return valid;
  }
  bool persistedPeerMatches(const PkiPeerPin &pin)const {
    if(peers_.revoked(pin.node))return false;
    uint8_t durable[32]{};
    const bool found=peers_.lookup(pin.node,durable);
    const bool match=found&&!memcmp(durable,pin.publicKey,sizeof(durable));
    const bool active=match&&peers_.active(pin.node,durable);
    crypto_wipe(durable,sizeof(durable));
    return active&&!peers_.revoked(pin.node);
  }
  // The owner-loop storage barrier must have paused RX and drained accepted
  // work before trusted-key replacement or removal. This check deliberately
  // ignores the one target child whose durable key may already have changed;
  // every unaffected child must remain usable and is never rebuilt here.
  bool childrenReadyExcept(int excludedContact,bool &hasReadyChild)const {
    hasReadyChild=false;
    if(!opened_||frozen_||!sharedReady(local_))return false;
    for(size_t i=0;i<RuntimeSlots;++i){
      if(!children_[i]){
        if(contacts_[i]>=0)return false;
        continue;
      }
      const int contact=int(contacts_[i]);
      if(contact<0||contact>=int(pins_.count()))return false;
      if(contact==excludedContact)continue;
      if(!active_[unsigned(contact)]||revoked_[unsigned(contact)]||
         !children_[i]->ready(local_))return false;
      hasReadyChild=true;
    }
    return true;
  }
  bool stagePeerChild(unsigned slot,unsigned contact,const PkiPeerPin &pin) {
    destroy(slot);
    auto *candidate=new(memory_[slot]) PkiRuntime(crypto_,backend_,identity_,nonces_,peers_,signing_,&verification_);
    candidate->deviceRole(role_);candidate->ownerUnmessagable(ownerUnmessagable_);
    candidate->rebroadcastMode(rebroadcast_);candidate->hopLimit(hop_);
    const bool valid=candidate->open(local_,pin.node)&&
      candidate->acceptsPeerKey(pin.node,pin.publicKey,32)&&candidate->ready(local_)&&
      sharedReady(local_)&&persistedPeerMatches(pin);
    if(!valid){
      candidate->~PkiRuntime();crypto_wipe(memory_[slot],sizeof(memory_[slot]));
      return false;
    }
    children_[slot]=candidate;contacts_[slot]=ContactIndex(contact);return true;
  }
  bool sharedReady(uint32_t local)const {
    IdentityBinding current;KeyRef key;
    return local==local_&&backend_.healthy()&&identity_.binding(current,key)&&identity_.active(key_)&&
      key.slot==key_.slot&&key.generation==key_.generation&&current.node==local_&&
      current.version==binding_.version&&current.algorithm==binding_.algorithm&&
      current.generation==binding_.generation&&!memcmp(current.publicKey,binding_.publicKey,32)&&
      peers_.boundTo(binding_.publicKey,local_);
  }
  bool ensure(Endpoint &endpoint) {
    if(ready(local_))return true;
    if(dormant(local_))return false;
    freeze();
    if(!cancelled_){endpoint.cancelPrivate();cancelled_=true;}
    return false;
  }
  void destroy(unsigned slot) {
    if(!children_[slot])return;
    retiredDiagnostics_.add(children_[slot]->replyDiagnostics());
    children_[slot]->~PkiRuntime();children_[slot]=nullptr;contacts_[slot]=-1;
    crypto_wipe(memory_[slot],sizeof(memory_[slot]));
    if(selected_==int(slot))selected_=-1;
  }
  bool construct(unsigned slot,unsigned contact) {
    destroy(slot);
    children_[slot]=new(memory_[slot]) PkiRuntime(crypto_,backend_,identity_,nonces_,peers_,signing_,&verification_);
    auto &child=*children_[slot];child.deviceRole(role_);child.ownerUnmessagable(ownerUnmessagable_);child.rebroadcastMode(rebroadcast_);child.hopLimit(hop_);
    const auto *pin=pins_.at(contact);
    if(!pin||!active_[contact]||!child.open(local_,pin->node)||!child.acceptsPeerKey(pin->node,pin->publicKey,32))return false;
    contacts_[slot]=ContactIndex(contact);return true;
  }
  int load(unsigned contact,Endpoint &endpoint) {
    const int existing=cached(contact);if(existing>=0)return existing;
    int slot=-1;
    for(size_t i=0;i<RuntimeSlots;++i)if(!children_[i]){slot=int(i);break;}
    // Basic nonce providers rely on the session RAM attempted-ID history.
    // Replacing such a session would discard that additional reuse guard.
    if(slot<0&&!nonces_.persistentIds()&&!nonces_.persistentNoncePairs())return -1;
    if(slot<0)for(size_t k=0;k<RuntimeSlots;++k){
      const unsigned candidate=(nextVictim_+k)%RuntimeSlots;
      const auto *pin=pins_.at(unsigned(contacts_[candidate]));
      if(pin&&!endpoint.hasPrivatePeerWork(pin->node)){slot=int(candidate);break;}
    }
    if(slot<0)return -1; // Busy slots are retained, never reset to admit work.
    if(!construct(unsigned(slot),contact)){freeze();return -1;}
    nextVictim_=uint8_t((unsigned(slot)+1)%RuntimeSlots);return slot;
  }
  bool seedIfNeeded() {
    if(firstChild()>=0||firstContact()<0)return true;
    return construct(0,unsigned(firstContact()));
  }
public:
  static_assert(RuntimeSlots==4&&Pins::Limit<=(sizeof(ContactIndex)==1?127:32767),"Fixed four slots and bounded contact index");
  static void *operator new(size_t,void *storage)noexcept{return storage;}
  MultiPeerRuntime(experimental::PkiCrypto &crypto,experimental::PkiBackend &backend,
    IdentityJournal &identity,NonceAuthority &nonces,PeerReplayAuthority &peers,SigningEntropy *signing=nullptr):
    crypto_(crypto),backend_(backend),identity_(identity),peers_(peers),nonces_(nonces),signing_(signing){}
  MultiPeerRuntime(const MultiPeerRuntime &)=delete;
  MultiPeerRuntime &operator=(const MultiPeerRuntime &)=delete;
  ~MultiPeerRuntime()override{freeze();for(size_t i=0;i<RuntimeSlots;++i)destroy(unsigned(i));
    if(verificationRuntime_){verificationRuntime_->~PkiRuntime();verificationRuntime_=nullptr;crypto_wipe(verificationMemory_,sizeof(verificationMemory_));}}
  bool open(uint32_t local,const Pins &set
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
            ,ExpandedContactFlash *progress=nullptr
#endif
            ) {
    if(opened_||frozen_)return false;
    opened_=true;local_=local;PkiPeerSet checkedLocal;
    if(set.count()>Pins::Limit||(set.count()>RuntimeSlots&&!nonces_.persistentIds()&&!nonces_.persistentNoncePairs())||
       !backend_.healthy()||!identity_.binding(binding_,key_)||
       !checkedLocal.assignEmpty(binding_)||!sharedReady(local)){freeze();return false;}
    pins_=set;
    for(size_t i=0;i<pins_.count();++i)active_[i]=true;
    verificationRuntime_=new(verificationMemory_) PkiRuntime(crypto_,backend_,identity_,nonces_,peers_,signing_,&verification_);
    if(!verificationRuntime_->openVerification(local)){freeze();return false;}
    // Validate every pinned session with the existing cryptographic open, even
    // when it will start cold. Only four constructed objects remain resident.
    for(size_t i=0;i<pins_.count();++i){
      const unsigned slot=unsigned(i<RuntimeSlots?i:RuntimeSlots-1);
      if(!construct(slot,unsigned(i))){freeze();return false;}
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
      if(progress)progress->progress();
#endif
    }
    if(!ready(local)&&!dormant(local)){freeze();return false;}
    return true;
  }
  // Publish a pin only after its exact identity-bound trust record has already
  // been durably committed. This updates the bounded RAM directory without
  // rebuilding the provider or disturbing unrelated child sessions. The
  // serialized owner must pause RX and drain accepted Endpoint work first.
#if defined(__GNUC__) && !defined(__clang__)
  __attribute__((noinline,noclone))
#else
  [[gnu::noinline]]
#endif
  bool adoptPersistedPeer(const PkiPeerPin &pin) {
    if(!opened_||frozen_||!sharedReady(local_))return false;
    const int existing=index(pin.node);
    if(!candidateShape(pin,existing))return false;
    const size_t oldCount=pins_.count();
    if(existing<0&&oldCount>=Pins::Limit)return false;
    const size_t nextCount=existing<0?oldCount+1:oldCount;
    if(nextCount>RuntimeSlots&&!nonces_.persistentIds()&&!nonces_.persistentNoncePairs())return false;
    if(!persistedPeerMatches(pin))return false;
    if(existing>=0&&(!active_[unsigned(existing)]||revoked_[unsigned(existing)]))return false;
    if(existing>=0&&!memcmp(pins_.at(unsigned(existing))->publicKey,pin.publicKey,32)){
      return false; // Preserve the existing duplicate-adoption refusal.
    }

    // A reconstructed child loses its RAM attempted-ID history. Keep the
    // existing nonpersistent-provider eviction prohibition for replacement.
    if(existing>=0&&!nonces_.persistentIds()&&!nonces_.persistentNoncePairs())return false;
    bool hasReadyChild=false;
    if(!childrenReadyExcept(existing,hasReadyChild))return false;

#if !defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
    PkiPeerPin staged[Pins::Limit]{};
    for(size_t i=0;i<oldCount;++i)staged[i]=*pins_.at(i);
    if(existing<0)staged[nextCount-1]=pin;else staged[unsigned(existing)]=pin;
    PkiPeerSet candidateSet;
    const bool validSet=candidateSet.assignTrusted(binding_,staged,nextCount);
    crypto_wipe(staged,sizeof(staged));
    if(!validSet)return false;
#endif

    int stageSlot=-1;
    const int oldSlot=existing<0?-1:cached(unsigned(existing));
    const bool needChild=(nextCount>0&&!hasReadyChild);
    const bool preserveSelection=oldSlot>=0&&selected_==oldSlot;
    if(oldSlot>=0)stageSlot=oldSlot;
    else if(needChild){
      for(size_t i=0;i<RuntimeSlots;++i)if(!children_[i]){stageSlot=int(i);break;}
      if(stageSlot<0)return false;
    }
    if(needChild||oldSlot>=0){
      if(stageSlot<0||!stagePeerChild(unsigned(stageSlot),unsigned(existing<0?nextCount-1:existing),pin))return false;
    }
    // No fallible operation follows publication. Unaffected child pointers,
    // selected slots, Endpoint work and per-session nonce/replay state remain
    // in place. The owner-loop storage barrier drains work before this call.
    const unsigned published=unsigned(existing<0?nextCount-1:existing);
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
    pins_.publish(published,&pin);
#else
    pins_=candidateSet;
#endif
    active_[published]=true;revoked_[published]=false;
    if(preserveSelection)selected_=stageSlot;
    return true;
  }
  void hopLimit(uint8_t value)override{if(value<=7){hop_=value;for(auto *child:children_)if(child)child->hopLimit(value);}}
  void deviceRole(uint8_t role)override{if(DeviceRolePolicy::supportedRole(role)){role_=role;for(auto *child:children_)if(child)child->deviceRole(role);}}
  void ownerUnmessagable(bool value)override{ownerUnmessagable_=value;for(auto *child:children_)if(child)child->ownerUnmessagable(value);}
  void rebroadcastMode(uint8_t mode)override{rebroadcast_=mode;for(auto *child:children_)if(child)child->rebroadcastMode(mode);}
  uint32_t peer()const override {const int i=firstContact();return i<0?0:pins_.at(unsigned(i))->node;}
  PrivateReplyDiagnostics replyDiagnostics()const override {
    auto totals=retiredDiagnostics_;for(const auto *child:children_)if(child)totals.add(child->replyDiagnostics());return totals;
  }
  bool ready(uint32_t local)const override {
    if(!opened_||frozen_||local!=local_||firstContact()<0||firstChild()<0||!sharedReady(local))return false;
    for(size_t i=0;i<RuntimeSlots;++i)if(children_[i]&&
      (contacts_[i]<0||!active_[unsigned(contacts_[i])]||!children_[i]->ready(local)))return false;
    return true;
  }
  bool dormant(uint32_t local)const override {return opened_&&!frozen_&&local==local_&&firstContact()<0&&sharedReady(local);}
  bool publicReady(uint32_t local)const override {
    return (ready(local)||(signing_&&dormant(local)))&&sharedReady(local)&&PkiSigner::ready(identity_,key_,binding_);
  }
  bool publicIdentity(uint8_t out[32])const override {
    if(!out||!signing_||!publicReady(local_))return false;
    memcpy(out,binding_.publicKey,32);return true;
  }
  bool peerPublicIdentity(uint32_t node,uint8_t out[32])const override {
    const int i=index(node);const auto *pin=i<0?nullptr:pins_.at(unsigned(i));
    if(!out||!ready(local_)||!pin||!active_[unsigned(i)]||!peers_.active(node,pin->publicKey))return false;
    memcpy(out,pin->publicKey,32);return true;
  }
  bool keyVerificationRandom(uint8_t *out,size_t n)override {
    return verificationRuntime_&&publicReady(local_)&&verificationRuntime_->keyVerificationRandom(out,n);
  }
  bool keyVerificationSha256(const uint8_t *data,size_t n,uint8_t out[32])override {
    return verificationRuntime_&&publicReady(local_)&&verificationRuntime_->keyVerificationSha256(data,n,out);
  }
  bool keyVerificationLocalIdentity(uint32_t &node,uint8_t out[32])const override {
    node=0;return verificationRuntime_&&publicReady(local_)&&verificationRuntime_->keyVerificationLocalIdentity(node,out);
  }
  bool keyVerificationPeerKey(uint32_t node,uint8_t out[32])const override {
    return verificationRuntime_&&verificationRuntime_->keyVerificationPeerKey(node,out);
  }
  int keyVerificationStageCandidate(uint32_t node,const uint8_t key[32])override {
    return verificationRuntime_&&publicReady(local_)?verificationRuntime_->keyVerificationStageCandidate(node,key):-EPERM;
  }
  void keyVerificationClearCandidate(uint32_t node,const uint8_t key[32])override {
    if(verificationRuntime_)verificationRuntime_->keyVerificationClearCandidate(node,key);
  }
  void keyVerificationReleasePeer(uint32_t node,const uint8_t key[32])override {
    if(verificationRuntime_)verificationRuntime_->keyVerificationReleasePeer(node,key);
  }
  bool keyVerificationCandidateMatches(uint32_t node)const override {
    return verificationRuntime_&&verificationRuntime_->keyVerificationCandidateMatches(node);
  }
  int keyVerificationSend(Endpoint &endpoint,bench::MtCodec &codec,uint32_t peer,uint32_t reply,
    const uint8_t *data,size_t n,bool pki,const uint8_t *key,size_t keySize,unsigned hop,
    bool reliable,uint32_t now,uint32_t random)override {
    return verificationRuntime_&&publicReady(local_)?verificationRuntime_->keyVerificationSend(
      endpoint,codec,peer,reply,data,n,pki,key,keySize,hop,reliable,now,random):-EPERM;
  }
  bool takeKeyVerificationReceive(KeyVerificationReceive &out)override {
    out={};return verificationRuntime_&&verificationRuntime_->takeKeyVerificationReceive(out);
  }
  bool bootstrap(bench::MtCodec &codec,const bench::MtNodeRecord &record,uint8_t model,
    uint32_t to,uint32_t id,bool wantResponse,uint32_t request,bench::Frame &out)override {
    if(!publicReady(local_)||!signing_)return false;
    bench::Frame candidate{};struct Wipe{bench::Frame &f;~Wipe(){crypto_wipe(&f,sizeof(f));}}wipe{candidate};
    const bool result=PkiPublicProducer::bootstrap(identity_,key_,binding_,nonces_,*signing_,codec,
      record,model,role_,ownerUnmessagable_,to,id,wantResponse,request,candidate);
    if(!publicReady(local_)){freeze();return false;}if(!result)return false;out=candidate;return true;
  }
  bool broadcast(bench::MtCodec &codec,uint32_t id,const uint8_t *data,size_t n,bench::Frame &out)override {
    if(!publicReady(local_)||!signing_)return false;
    bench::Frame candidate{};struct Wipe{bench::Frame &f;~Wipe(){crypto_wipe(&f,sizeof(f));}}wipe{candidate};
    const bool result=PkiPublicProducer::broadcast(identity_,key_,binding_,nonces_,*signing_,codec,
      id,data,n,candidate);
    if(!publicReady(local_)){freeze();return false;}if(!result)return false;out=candidate;return true;
  }
  bool licensedUnicast(bench::MtCodec &codec,uint32_t to,uint32_t id,const uint8_t *data,size_t n,bench::Frame &out)override {
    if(!publicReady(local_)||!signing_||!codec.licensedPlaintext())return false;
    bench::Frame candidate{};struct Wipe{bench::Frame &f;~Wipe(){crypto_wipe(&f,sizeof(f));}}wipe{candidate};
    const bool result=PkiPublicProducer::licensedUnicast(identity_,key_,binding_,nonces_,*signing_,codec,to,id,data,n,candidate);
    if(!publicReady(local_)){freeze();return false;}if(!result)return false;out=candidate;return true;
  }
  bool encodeChannelControl(bench::MtCodec &codec,uint32_t to,uint32_t id,const uint8_t *data,size_t n,bench::Frame &out)override {
    if(codec.licensedPlaintext()&&to!=0xffffffffu)return licensedUnicast(codec,to,id,data,n,out);
    if(to!=UINT32_MAX&&codec.signsPublic()&&data&&n>=2&&data[0]==8&&data[1]==3)
      return signing_&&publicReady(local_)&&PkiPublicProducer::licensedUnicast(identity_,key_,binding_,nonces_,*signing_,
        codec,to,id,data,n,out)&&publicReady(local_);
    if(to==0xffffffffu&&signing_&&PkiPublicProducer::canonicalPositionBroadcast(data,n))
      return broadcast(codec,id,data,n,out);
    if(!publicReady(local_))return false;
    bench::Frame candidate{};struct Wipe{bench::Frame &f;~Wipe(){crypto_wipe(&f,sizeof(f));}}wipe{candidate};
    const bool result=PkiPublicProducer::encodeChannelControl(identity_,key_,binding_,nonces_,codec,to,id,data,n,candidate);
    if(!publicReady(local_)){freeze();return false;}if(!result)return false;out=candidate;return true;
  }
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE)
  int stockReply(Endpoint &ep,uint32_t peer,uint32_t id,const uint8_t *wire,size_t size,
                 unsigned hop,bool reliable,uint32_t request,uint32_t now)override {
    if(!ensure(ep))return -EPERM;
    const int i=index(peer);if(i<0||!active_[i])return -EPERM;
    const int slot=load(unsigned(i),ep);if(slot<0)return -ENOBUFS;
    const int result=children_[slot]->stockReply(ep,peer,id,wire,size,hop,reliable,request,now);
    return ensure(ep)?result:-EPERM;
  }
#endif
  bool encodePublicApplication(bench::MtCodec &codec,uint32_t to,uint32_t id,const uint8_t *data,size_t n,bench::Frame &out)override {
    if(codec.licensedPlaintext()&&to!=0xffffffffu)return licensedUnicast(codec,to,id,data,n,out);
    if(to==0xffffffffu)return broadcast(codec,id,data,n,out);
    if(!publicReady(local_))return false;
    bench::Frame candidate{};struct Wipe{bench::Frame &f;~Wipe(){crypto_wipe(&f,sizeof(f));}}wipe{candidate};
    const bool result=PkiPublicProducer::encodePublicApplication(identity_,key_,binding_,nonces_,codec,to,id,data,n,candidate);
    if(!publicReady(local_)){freeze();return false;}if(!result)return false;out=candidate;return true;
  }
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  bool encodeStoreForward(bench::MtCodec &codec,const modules::StoreForwardPacket &packet,
                          uint32_t id,bench::Frame &out)override {
    if(!publicReady(local_))return false;
    bench::Frame candidate{};struct Wipe{bench::Frame &f;~Wipe(){crypto_wipe(&f,sizeof(f));}}wipe{candidate};
    const bool result=PkiPublicProducer::encodeStoreForward(identity_,key_,binding_,nonces_,codec,packet,id,candidate);
    if(!publicReady(local_)){freeze();return false;}if(!result)return false;out=candidate;return true;
  }
#endif
  void clearEvent()override {authenticatedRxFrom_=0;for(auto *child:children_)if(child)child->clearEvent();selected_=-1;}
  void freeze()override {frozen_=true;clearEvent();for(auto *child:children_)if(child)child->freeze();if(verificationRuntime_)verificationRuntime_->freeze();}
  bool acceptsPeerKey(uint32_t node,const uint8_t *key,size_t n)const override {
    const auto *pin=pins_.find(node);
    if(pin)return !n||(n==32&&key&&!memcmp(key,pin->publicKey,32));
    if(!n)return true;
    if(n!=32||!key)return false;
    uint8_t historical[32]{};
    const bool known=peers_.lookup(node,historical);
    const bool matches=!known||!memcmp(historical,key,sizeof(historical));
    const bool tombstoneWithoutKey=!known&&peers_.revoked(node);
    crypto_wipe(historical,sizeof(historical));
    return matches&&!tombstoneWithoutKey;
  }
  bool matchesPeer(uint32_t node)const override {
    if(!opened_)return false;
    uint8_t key[32]{};return pins_.find(node)||peers_.revoked(node)||peers_.lookup(node,key);
  }
  DestinationDisposition destinationDisposition(uint32_t node,const uint8_t *hint,
      size_t hintSize)const override {
    if(!validNodeId(node)||node==local_||(hintSize&&(!hint||hintSize!=32)))
      return DestinationDisposition::KeyMismatch;
    if(!publicReady(local_))return DestinationDisposition::Unavailable;
    const auto *pin=pins_.find(node);
    if(pin) {
      if(hintSize&&memcmp(hint,pin->publicKey,32))return DestinationDisposition::KeyMismatch;
      const int contact=index(node);
      if(contact<0||revoked_[unsigned(contact)]||peers_.revoked(node))
        return DestinationDisposition::Revoked;
      return active_[unsigned(contact)]&&persistedPeerMatches(*pin)?
        DestinationDisposition::PinnedActive:DestinationDisposition::Unavailable;
    }
    if(peers_.revoked(node))return DestinationDisposition::Revoked;
    uint8_t knownKey[32]{};
    const bool known=peers_.lookup(node,knownKey);
    const bool hintMismatch=known&&hintSize&&memcmp(hint,knownKey,sizeof(knownKey));
    crypto_wipe(knownKey,sizeof(knownKey));
    if(hintMismatch)return DestinationDisposition::KeyMismatch;
    return known?DestinationDisposition::KnownUnpinned:DestinationDisposition::UnknownNoKey;
  }
  // Owner-loop callers must pause RX and drain accepted work before invoking
  // this durable mutation. Surviving session objects are never relocated.
  bool revoke(uint32_t node)override {
    const int contact=index(node);
    if(contact>=0&&revoked_[unsigned(contact)])return true;
    if(!opened_||frozen_||!sharedReady(local_))return false;
    if(contact<0){
      if(peers_.revoked(node))return true;
      uint8_t key[32]{};if(!peers_.lookup(node,key))return true;
      const bool removed=peers_.revokeTrustedPeer(node,key);
      const bool tombstone=peers_.revoked(node);
      uint8_t after[32]{};const bool stillFound=peers_.lookup(node,after);
      const bool stillActive=stillFound&&peers_.active(node,key);
      crypto_wipe(after,sizeof(after));
      crypto_wipe(key,sizeof(key));
      if(!removed||(!tombstone&&stillActive)){freeze();return false;}
      return true;
    }
    const auto *pin=pins_.at(unsigned(contact));
    if(!pin||!active_[unsigned(contact)]||revoked_[unsigned(contact)])return false;
    if(!nonces_.persistentIds()&&!nonces_.persistentNoncePairs())return false;
    bool survivorChild=false;
    if(!childrenReadyExcept(contact,survivorChild))return false;
    const size_t oldCount=pins_.count();
    const size_t nextCount=oldCount-1;
#if !defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
    PkiPeerPin staged[Pins::Limit]{};
    for(size_t i=0,j=0;i<oldCount;++i)if(int(i)!=contact)staged[j++]=*pins_.at(i);
    PkiPeerSet candidateSet;
    const bool validSet=nextCount?candidateSet.assignTrusted(binding_,staged,nextCount):candidateSet.assignEmpty(binding_);
    crypto_wipe(staged,sizeof(staged));
    if(!validSet)return false;
#endif
    int stagedSurvivorSlot=-1;
    if(nextCount&&!survivorChild){
      for(size_t i=0;i<RuntimeSlots;++i)if(!children_[i]){stagedSurvivorSlot=int(i);break;}
      if(stagedSurvivorSlot<0)return false;
      unsigned survivor=0;
      while(survivor<oldCount&&int(survivor)==contact)++survivor;
      if(survivor>=oldCount||!stagePeerChild(unsigned(stagedSurvivorSlot),survivor,*pins_.at(survivor)))return false;
    }
    uint8_t durable[32]{};
    const bool exact=peers_.lookup(node,durable)&&!memcmp(durable,pin->publicKey,sizeof(durable))&&
      peers_.active(node,durable)&&!peers_.revoked(node);
    crypto_wipe(durable,sizeof(durable));
    if(!exact){if(stagedSurvivorSlot>=0)destroy(unsigned(stagedSurvivorSlot));return false;}
    const bool removed=peers_.revokeTrustedPeer(node,pin->publicKey);
    const bool tombstone=peers_.revoked(node);
    uint8_t after[32]{};const bool stillFound=peers_.lookup(node,after);
    const bool stillActive=stillFound&&peers_.active(node,pin->publicKey);
    crypto_wipe(after,sizeof(after));
    if(!removed||(!tombstone&&stillActive)){
      if(stagedSurvivorSlot>=0)destroy(unsigned(stagedSurvivorSlot));
      freeze();return false;
    }
    const int slot=cached(unsigned(contact));if(slot>=0)destroy(unsigned(slot));
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
    pins_.publish(unsigned(contact),nullptr);
#else
    pins_=candidateSet;
#endif
    for(size_t i=0;i<Pins::Limit;++i){active_[i]=i<nextCount;revoked_[i]=false;}
    for(size_t i=0;i<RuntimeSlots;++i)if(children_[i]&&contacts_[i]>contact)--contacts_[i];
    return true;
  }
  void sweep(Endpoint &endpoint)override {
    if(!ensure(endpoint))return;
    for(auto *child:children_)if(child){child->sweep(endpoint);if(!ensure(endpoint))return;}
  }
#if defined(CONFIG_MT_RF_ADMIN_GET)
  int submit(Endpoint &endpoint,uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
    unsigned hop,bool reliable,const uint8_t *key,size_t keySize,bool explicitPki,uint32_t now)override {
    return submitImpl(endpoint,owner,to,id,data,n,hop,reliable,key,keySize,explicitPki,now,false);
  }
  int submitAdminGet(Endpoint &endpoint,uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
    unsigned hop,bool reliable,const uint8_t *key,size_t keySize,bool explicitPki,uint32_t now)override {
    return submitImpl(endpoint,owner,to,id,data,n,hop,reliable,key,keySize,explicitPki,now,true);
  }
  bool respondPrivateAdminMutation(Endpoint &endpoint,uint32_t local,uint32_t peer,
      const uint8_t key[32],uint32_t request,uint8_t responseHop,bool reliable,
      uint32_t routingError,uint32_t now,uint32_t random)override {
    if(!ensure(endpoint)||local!=local_||!key||!request||responseHop>HopPolicy::Maximum)return false;
    const int contact=index(peer);
    const auto *pin=contact<0?nullptr:pins_.at(unsigned(contact));
    if(!pin||!active_[unsigned(contact)]||memcmp(pin->publicKey,key,32)||
       !peers_.active(peer,key))return false;
    const int slot=cached(unsigned(contact));
    if(slot<0||!children_[slot]||!children_[slot]->ready(local_))return false;
    const bool result=children_[slot]->respondPrivateAdminMutation(endpoint,local,peer,key,
        request,responseHop,reliable,routingError,now,random);
    return ensure(endpoint)&&result;
  }
private:
  // Keep one target body for the two virtual entrypoints under GCC LTO.
  // The canonical GET and generic application gates remain distinct below.
#if defined(__GNUC__) && !defined(__clang__)
  __attribute__((noinline,noclone))
#else
  [[gnu::noinline]]
#endif
  int submitImpl(Endpoint &endpoint,uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
    unsigned hop,bool reliable,const uint8_t *key,size_t keySize,bool explicitPki,uint32_t now,bool adminGet) {
    if(!ensure(endpoint))return -EPERM;
    const int contact=index(to);const auto *pin=contact<0?nullptr:pins_.at(unsigned(contact));
    if(!pin||!active_[unsigned(contact)]||(owner>=LocalClientCount&&owner!=Endpoint::LocalUiOwner)||
       !id||hop>HopPolicy::Maximum||
       (adminGet?(!explicitPki||!key||keySize!=32||owner>=LocalClientCount):!PrivateApplication::allowed(data,n))||
       (keySize&&(keySize!=32||!key||memcmp(key,pin->publicKey,32))))return -EINVAL;
    if(owner==Endpoint::LocalUiOwner&&!endpoint.localUiSubmissionAllowed(id))return -EBUSY;
    sweep(endpoint);if(!ensure(endpoint))return -EPERM;
    const int slot=load(unsigned(contact),endpoint);if(slot<0)return ensure(endpoint)?-ENOBUFS:-EPERM;
    const int result=adminGet?
      children_[slot]->submitAdminGet(endpoint,owner,to,id,data,n,hop,reliable,key,keySize,explicitPki,now):
      children_[slot]->submit(endpoint,owner,to,id,data,n,hop,reliable,key,keySize,explicitPki,now);
    return ensure(endpoint)?result:-EPERM;
  }
public:
#else
  int submit(Endpoint &endpoint,uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
    unsigned hop,bool reliable,const uint8_t *key,size_t keySize,bool explicitPki,uint32_t now)override {
    if(!ensure(endpoint))return -EPERM;
    const int contact=index(to);const auto *pin=contact<0?nullptr:pins_.at(unsigned(contact));
    if(!pin||!active_[unsigned(contact)]||(owner>=LocalClientCount&&owner!=Endpoint::LocalUiOwner)||!id||hop>HopPolicy::Maximum||!PrivateApplication::allowed(data,n)||
       (keySize&&(keySize!=32||!key||memcmp(key,pin->publicKey,32))))return -EINVAL;
    if(owner==Endpoint::LocalUiOwner&&!endpoint.localUiSubmissionAllowed(id))return -EBUSY;
    sweep(endpoint);if(!ensure(endpoint))return -EPERM;
    const int slot=load(unsigned(contact),endpoint);if(slot<0)return ensure(endpoint)?-ENOBUFS:-EPERM;
    const int result=children_[slot]->submit(endpoint,owner,to,id,data,n,hop,reliable,key,keySize,explicitPki,now);
    return ensure(endpoint)?result:-EPERM;
  }
#endif
  void receive(Endpoint &endpoint,bench::MtCodec &codec,const uint8_t *p,size_t n,
    uint32_t now,uint32_t random,bool armed)override {
    (void)receiveClassified(endpoint,codec,p,n,now,random,armed);
  }
  ReceiveClassification receiveClassified(Endpoint &endpoint,bench::MtCodec &codec,
      const uint8_t *p,size_t n,uint32_t now,uint32_t random,bool armed)override {
    receiveClassification_=ReceiveClassification::Hold;
    clearEvent();authenticatedRxFrom_=0;
    if(!p||n<29||n>255) {
      receiveClassification_=ReceiveClassification::NotPki;return receiveClassification_;
    }
    if(bench::le32(p)!=local_||p[13]) {
      receiveClassification_=ReceiveClassification::NotPki;return receiveClassification_;
    }
    if(!bench::le32(p+8)||(p[12]&7)>((p[12]>>5)&7)) {
      receiveClassification_=ReceiveClassification::NotPki;return receiveClassification_;
    }
    if(!sharedReady(local_))return receiveClassification_;
    // One shared temporary session receives first-contact PKI verification
    // responses, independent of whether this node has any persisted pins.
    // Its PkiSession admits only the dedicated verifier payload; ordinary
    // messages from an unpinned source never enter peer/session delivery.
    if(verificationRuntime_&&publicReady(local_)&&verification_.peer&&bench::le32(p+4)==verification_.peer) {
      const auto verificationResult=verificationRuntime_->receiveClassified(endpoint,codec,p,n,now,random,armed);
      if(verificationResult==ReceiveClassification::AuthenticatedConsumed||
         verificationResult==ReceiveClassification::Hold)return receiveClassification_=verificationResult;
    }
    const uint32_t from=bench::le32(p+4);
    const int contact=index(from);
    if(contact<0) {
      const auto disposition=destinationDisposition(from,nullptr,0);
      if(disposition==DestinationDisposition::UnknownNoKey)
        receiveClassification_=ReceiveClassification::Unauthenticated;
      return receiveClassification_;
    }
    if(!active_[unsigned(contact)]||revoked_[unsigned(contact)])return receiveClassification_;
    if(!ensure(endpoint))return receiveClassification_;
    const int slot=load(unsigned(contact),endpoint);
    if(slot<0){(void)ensure(endpoint);return receiveClassification_;}
    selected_=slot;
    receiveClassification_=children_[slot]->receiveClassified(endpoint,codec,p,n,now,random,armed);
    uint32_t authenticatedFrom=0;
    if(children_[slot]->takeAuthenticatedReceive(authenticatedFrom)&&authenticatedFrom==bench::le32(p+4))
      authenticatedRxFrom_=authenticatedFrom;
    if(!ensure(endpoint))return receiveClassification_=ReceiveClassification::Hold;
    return receiveClassification_;
  }
  bool takeAuthenticatedReceive(uint32_t &from)override {from=authenticatedRxFrom_;authenticatedRxFrom_=0;return from!=0;}
  bool keyVerificationProof(Endpoint &endpoint,const uint8_t *p,size_t n,
      const bench::RouteMetadata &metadata,uint32_t now)override {
    return verificationRuntime_&&verification_.peer&&p&&n>=8&&
      bench::le32(p+4)==verification_.peer&&
      verificationRuntime_->keyVerificationProof(endpoint,p,n,metadata,now);
  }
  void proof(Endpoint &endpoint,const uint8_t *p,size_t n,const bench::RouteMetadata &metadata,uint32_t now)override {
    clearEvent();if(!p||n<8)return;
    // A candidate KVA send owns a separate temporary session from ordinary
    // pinned DM. Verify its ACK proof only against that exact peer/Endpoint
    // job first; if it does not match, preserve normal pinned-peer correlation.
    if(verificationRuntime_&&verification_.peer&&
       bench::le32(p+4)==verification_.peer&&
       verificationRuntime_->keyVerificationProof(endpoint,p,n,metadata,now))return;
    if(!ensure(endpoint))return;
    const int contact=index(bench::le32(p+4));if(contact<0||!active_[unsigned(contact)])return;
    const int slot=cached(unsigned(contact));if(slot<0)return; // A proof never creates a new session.
    children_[slot]->proof(endpoint,p,n,metadata,now);(void)ensure(endpoint);
  }
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE) || defined(CONFIG_MT_DROPZONE)
  const PrivateEvent *moduleEvent(const uint8_t *frame,size_t size)override{
    if(!ready(local_) || selected_<0 || selected_>=int(RuntimeSlots) || !children_[selected_] ||
       contacts_[selected_]<0 || !active_[unsigned(contacts_[selected_])])return nullptr;
    return children_[selected_]->moduleEvent(frame,size);
  }
#endif
  bool event(uint8_t owner,const uint8_t *p,size_t n,PrivateEvent &out)override {
    if(!ready(local_)||selected_<0||selected_>=int(RuntimeSlots)||!children_[selected_]||
       contacts_[selected_]<0||!active_[unsigned(contacts_[selected_])]){clearEvent();return false;}
    return children_[selected_]->event(owner,p,n,out);
  }
};
}
