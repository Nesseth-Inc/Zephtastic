// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include "PeerLastHeard.h"
#include "PeerDeviceMetrics.h"
#include "PeerPosition.h"
#include "PeerStatus.h"

namespace mt {
class NodeJournal;
// Received peer identities, volatile heard/Position metadata and local favorite/ignore policy.
// No local identity, MAC, local position, secrets,
// messaging history, or remote configuration. Manual verification is local owner policy, not RF proof.
class NodeCache {
public:
  static constexpr unsigned LegacyCapacity=16;
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
  // Stock MAX_NUM_NODES includes self. ZT stores remote identities here.
  static constexpr unsigned Capacity=199,ProtectedLimit=198;
#else
  static constexpr unsigned Capacity=LegacyCapacity,ProtectedLimit=Capacity-2;
#endif
  enum class Edit:uint8_t {Favorite,Unfavorite,Ignore,Unignore,ToggleMute,Remove};
  struct Entry {
    static constexpr unsigned Size=92,Name=12,Short=52,KeyLength=57,Key=58,Flags=90,LocalFlags=91;
    static constexpr uint8_t Favorite=1,Ignored=2,Placeholder=4,Muted=8,ManualVerified=16,VerifiedSigner=32;
    static constexpr uint8_t LicensedUser=4;
    static constexpr uint8_t Protected=Favorite|Ignored|ManualVerified,DurablePolicy=Favorite|Ignored|Muted|ManualVerified|VerifiedSigner;
    uint8_t bytes[Size]{};
    // Volatile receive flags share one storage unit to preserve the entry RAM cap.
    int8_t snr=0;uint8_t hops:3;bool heard:1;bool viaMqtt:1;bool replayPosition:1,replayDevice:1,replayEnvironment:1;
    uint8_t routeHint=0; // Last learned next hop; journal v6 stores one byte per peer.
    // RAM only: never part of bytes[92] or the NodeJournal wire record.
    uint8_t lastHeardBytes[4]{};
    PeerDeviceMetrics deviceMetrics; // RAM only; never serialized by NodeJournal.
    PeerPosition position; // RAM only; never serialized by NodeJournal.
    PeerStatus status; // RAM only, including explicit known-empty values.
    Entry():bytes{},snr(0),hops(0),heard(false),viaMqtt(false),replayPosition(false),replayDevice(false),replayEnvironment(false),lastHeardBytes{}{}
    static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
    static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
    uint32_t id()const{return get32(bytes);}
    uint32_t model()const{return get32(bytes+4);}
    uint32_t role()const{return get32(bytes+8);}
    uint32_t lastHeard()const{return get32(lastHeardBytes);}
    bool favorite()const{return bytes[LocalFlags]&Favorite;}
    bool ignored()const{return bytes[LocalFlags]&Ignored;}
    bool muted()const{return bytes[LocalFlags]&Muted;}
    bool manuallyVerified()const{return bytes[LocalFlags]&ManualVerified;}
    bool verifiedSigner()const{return bytes[LocalFlags]&VerifiedSigner;}
    bool licensed()const{return bytes[Flags]&LicensedUser;}
    bool placeholder()const{return bytes[LocalFlags]&Placeholder;}
    bool protectedNode()const{return bytes[LocalFlags]&Protected;}
    uint8_t sampleMask()const{
      return uint8_t(position.present()|(deviceMetrics.present()<<1)|(replayEnvironment<<2)|(status.present()<<3));
    }
    void clearSamples(uint8_t mask=15){
      if(mask&1){position={};replayPosition=false;}
      if(mask&2){deviceMetrics={};replayDevice=false;}
      if(mask&4)replayEnvironment=false;
      if(mask&8)status={};
    }
    void identity(uint32_t id,uint32_t model,uint32_t role){put32(bytes,id);put32(bytes+4,model);put32(bytes+8,role);}
    static bool text(uint8_t *out,size_t capacity,const char *in,size_t bound){
      const void *end=memchr(in,0,bound);if(!end)return false;
      const size_t n=static_cast<const char *>(end)-in;if(n>=capacity)return false;
      memset(out,0,capacity);memcpy(out,in,n);return true;
    }
    static bool canonicalText(const uint8_t *p,size_t n){
      bool ended=false;for(size_t i=0;i<n;++i){if(!p[i])ended=true;else if(ended)return false;}return ended;
    }
    static bool valid(const uint8_t *p,uint32_t self){
      const uint32_t id=get32(p);
      if(id<=1||id>=0xfffffffeu||id==self||get32(p+4)>INT32_MAX||get32(p+8)>INT32_MAX||
         p[KeyLength]>32||(p[Flags]&~7u)||(p[LocalFlags]&~63u)||!canonicalText(p+Name,40)||!canonicalText(p+Short,5))return false;
      for(unsigned i=p[KeyLength];i<32;++i)if(p[Key+i])return false;
      if(p[LocalFlags]&Placeholder)for(unsigned i=4;i<LocalFlags;++i)if(p[i])return false;
      return true;
    }
  };
private:
  friend class NodeJournal;
  Entry entries_[Capacity]{};
  uint32_t replayEpoch_[Capacity]{};
  uint8_t count_=0,next_=0;bool dirty_=false,frozen_=false;
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
  bool expanded_=false;
#endif
  int allocate(){
    const unsigned limit=workingLimit();
    if(count_<limit)return count_++;
    for(unsigned i=0;i<limit;++i){
      const unsigned slot=(next_+i)%limit;
      if(!entries_[slot].protectedNode()){next_=uint8_t((slot+1)%limit);return int(slot);}
    }
    return -ENOSPC;
  }
public:
  bool expanded()const{
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    return expanded_;
#else
    return false;
#endif
  }
  unsigned workingLimit()const{return expanded()?Capacity:LegacyCapacity;}
  unsigned protectionLimit()const{return expanded()?ProtectedLimit:LegacyCapacity-2;}
  unsigned count()const{return count_;}
  uint32_t replayEpoch(unsigned slot)const{return slot<count_?replayEpoch_[slot]:0;}
  bool dirty()const{return dirty_;}
  bool ready()const{return !frozen_;}
  const Entry *get(unsigned i)const{return i<count_?&entries_[i]:nullptr;}
  int find(uint32_t id)const{for(unsigned i=0;i<count_;++i)if(entries_[i].id()==id)return int(i);return -1;}
  uint8_t routeHint(uint32_t id)const{
    const int slot=find(id);
    return ready()&&slot>=0&&!entries_[slot].ignored()?entries_[slot].routeHint:0;
  }
  void setRouteHint(uint32_t id,uint8_t hop){
    const int slot=find(id);
    if(ready()&&slot>=0&&!entries_[slot].ignored()&&entries_[slot].routeHint!=hop){
      entries_[slot].routeHint=hop;dirty_=true;
    }
  }
  bool ignored(uint32_t id)const{const int slot=find(id);return slot>=0&&entries_[slot].ignored();}
  bool muted(uint32_t id)const{const int slot=find(id);return slot>=0&&entries_[slot].muted();}
  bool verifiedSigner(uint32_t id)const{const int slot=find(id);return slot>=0&&entries_[slot].verifiedSigner();}
  // Persist anti-downgrade evidence only after real signature verification.
  // It stays attached to the peer entry and may be forgotten only when the
  // ordinary bounded NodeDB evicts/removes that whole unprotected entry.
  bool rememberVerifiedSigner(uint32_t id,uint32_t self){
    if(frozen_||id<=1||id>=0xfffffffeu||id==self)return false;
    int slot=find(id);
    if(slot<0){
      slot=allocate();if(slot<0)return false;
      entries_[unsigned(slot)]={};replayEpoch_[slot]=0;entries_[unsigned(slot)].identity(id,0,0);
      entries_[unsigned(slot)].bytes[Entry::LocalFlags]=Entry::Placeholder;
    }
    auto &flags=entries_[unsigned(slot)].bytes[Entry::LocalFlags];
    if(flags&Entry::VerifiedSigner)return true;
    flags|=Entry::VerifiedSigner;dirty_=true;return true;
  }
  unsigned protectedCount()const{unsigned n=0;for(unsigned i=0;i<count_;++i)n+=entries_[i].protectedNode();return n;}
  // Admission belongs to the receive caller. This only annotates an existing
  // identity; it never allocates, changes durable policy, or dirties the cache.
  // Zero is unknown, while a known UTC correction may move backwards.
  bool observeLastHeard(uint32_t id,uint32_t capturedUtc){
    if(frozen_||id<=1||id>=0xfffffffeu)return false;
    const int slot=find(id);if(slot<0)return false;
    auto &entry=entries_[slot];if(entry.ignored()||entry.placeholder())return false;
    Entry::put32(entry.lastHeardBytes,receivedLastHeard(entry.lastHeard(),capturedUtc));
    return true;
  }
  // Receive admission belongs to the caller. Update timestamp and packet-origin
  // metadata together only for an existing, non-ignored identity. This does
  // not allocate, change durable bytes/policy, or dirty the NodeJournal.
  bool observeReceivedPacket(uint32_t id,uint32_t capturedUtc,bool viaMqtt){
    if(frozen_||id<=1||id>=0xfffffffeu)return false;
    const int slot=find(id);if(slot<0)return false;
    auto &entry=entries_[slot];if(entry.ignored()||entry.placeholder())return false;
    Entry::put32(entry.lastHeardBytes,receivedLastHeard(entry.lastHeard(),capturedUtc));
    entry.viaMqtt=viaMqtt;return true;
  }
  // Receive admission is owned by the caller. Only annotate an already known
  // identity; no allocation, durable bytes/policy changes or journal dirtiness.
  bool observeDeviceMetrics(uint32_t id,const PeerDeviceMetrics &metrics,bool replay=false){
    if(frozen_||id<=1||id>=0xfffffffeu||!metrics.present())return false;
    const int slot=find(id);if(slot<0)return false;
    auto &entry=entries_[slot];if(entry.ignored()||entry.placeholder())return false;
    entry.deviceMetrics=metrics;entry.replayDevice=replay;return true;
  }
  bool observePosition(uint32_t id,PeerPosition position,bool replay=false,bool sameChannel=false){
    if(frozen_||id<=1||id>=0xfffffffeu||!position.present())return false;
    const int slot=find(id);if(slot<0)return false;
    auto &entry=entries_[slot];if(entry.ignored()||entry.placeholder())return false;
    if(replay&&entry.replayPosition&&sameChannel)position.retainFixTime(entry.position);
    entry.position=position;entry.replayPosition=replay;return true;
  }
  void clearReplay(const uint8_t *mask=nullptr){
    for(unsigned i=0;i<count_;++i)entries_[i].clearSamples(mask?mask[i]:15);
  }
  void environmentReplay(uint32_t node,bool valid){
    const int slot=find(node);if(ready()&&slot>=0)entries_[slot].replayEnvironment=valid;
  }
  bool observeStatus(uint32_t id,const PeerStatus &status,bool replay=true){
    if(frozen_||id<=1||id>=0xfffffffeu||!status.present())return false;
    const int slot=find(id);if(slot<0)return false;
    auto &entry=entries_[slot];if(entry.ignored()||entry.placeholder())return false;
    entry.status=status;entry.status.allowReplay(replay);return true;
  }
  // Local admin only. Returns 1 changed, 0 unchanged, or errno. Persistence
  // callers mutate a candidate copy and publish only after verified storage.
  int edit(uint32_t id,uint32_t self,Edit op){
    if(frozen_)return -EACCES;
    if(id<=1||id>=0xfffffffeu||id==self)return -EINVAL;
    int slot=find(id);
    if(op==Edit::Remove){
      if(slot<0)return 0;
      for(unsigned i=unsigned(slot);i+1<count_;++i){entries_[i]=entries_[i+1];replayEpoch_[i]=replayEpoch_[i+1];}
      entries_[--count_]={};replayEpoch_[count_]=0;next_=0;dirty_=true;return 1;
    }
    if(op==Edit::ToggleMute){
      if(slot<0)return -ENOENT;
      entries_[slot].bytes[Entry::LocalFlags]^=Entry::Muted;dirty_=true;return 1;
    }
    if(op!=Edit::Favorite&&op!=Edit::Unfavorite&&op!=Edit::Ignore&&op!=Edit::Unignore)return -EINVAL;
    const bool set=op==Edit::Favorite||op==Edit::Ignore;
    const uint8_t bit=op==Edit::Favorite||op==Edit::Unfavorite?Entry::Favorite:Entry::Ignored;
    if(slot<0){
      // Match stock local policy: no entry means no favorite state to change.
      if(op==Edit::Favorite||op==Edit::Unfavorite)return 0;
      if(op!=Edit::Ignore)return -ENOENT;
      if(protectedCount()>=protectionLimit())return -ENOSPC;
      slot=allocate();if(slot<0)return slot;
      entries_[slot]={};replayEpoch_[slot]=0;entries_[slot].identity(id,0,0);entries_[slot].bytes[Entry::LocalFlags]=Entry::Placeholder;
    }
    auto &e=entries_[slot];auto &flags=e.bytes[Entry::LocalFlags];
    if(bool(flags&bit)==set)return 0;
    if(set&&!e.protectedNode()&&protectedCount()>=protectionLimit())return -ENOSPC;
    if(op==Edit::Ignore||op==Edit::Unignore)replayEpoch_[slot]=0;
    if(op==Edit::Ignore)e.clearSamples();
    flags=set?uint8_t(flags|bit):uint8_t(flags&~bit);dirty_=true;return 1;
  }
  // Explicit local-owner contact update. Never call from RF admission. A
  // keyless contact preserves a saved key; false ignore does not unignore.
#if defined(__GNUC__) && !defined(__clang__)
  __attribute__((noinline,noclone))
#else
  [[gnu::noinline]]
#endif
  int ownerContact(const Entry &incoming,uint32_t self,bool ignore,bool manual,
                   bool autoFavorite=true,uint32_t heardNow=0) {
    if(frozen_)return -EACCES;
    if(!Entry::valid(incoming.bytes,self)||incoming.bytes[Entry::LocalFlags])return -EINVAL;
    const int found=find(incoming.id());
    const Entry *old=found<0?nullptr:&entries_[unsigned(found)];
    Entry staged=incoming;
    if(old&&old->bytes[Entry::KeyLength]==32&&incoming.bytes[Entry::KeyLength]!=32) {
      staged.bytes[Entry::KeyLength]=32;memcpy(staged.bytes+Entry::Key,old->bytes+Entry::Key,32);
    }
    // Omission preserves a key before manual policy checks. Only an explicit
    // different supplied key requires an explicit manual replacement request.
    if(old&&old->manuallyVerified()&&!manual&&
       (staged.bytes[Entry::KeyLength]!=old->bytes[Entry::KeyLength]||
        memcmp(staged.bytes+Entry::Key,old->bytes+Entry::Key,old->bytes[Entry::KeyLength])))return -EACCES;
    uint8_t flags=old?old->bytes[Entry::LocalFlags]&Entry::DurablePolicy:0;
    // A legitimate owner-authorized key replacement starts a new identity
    // binding for downgrade purposes; a removed entry clears it entirely.
    const bool changedKey=old&&(staged.bytes[Entry::KeyLength]!=old->bytes[Entry::KeyLength]||
       memcmp(staged.bytes+Entry::Key,old->bytes+Entry::Key,old->bytes[Entry::KeyLength]));
    if(changedKey)flags=uint8_t(flags&~Entry::VerifiedSigner);
    const unsigned protectedWithoutOld=protectedCount()-(old&&old->protectedNode()?1u:0u);
    const bool canProtect=(flags&Entry::Protected)||protectedWithoutOld<protectionLimit();
    if(ignore) {
      if(!canProtect)return -ENOSPC;
      flags=uint8_t((flags|Entry::Ignored)&~Entry::Favorite);
    } else {
      if(autoFavorite&&canProtect)flags|=Entry::Favorite;
      if(manual&&canProtect)flags|=Entry::ManualVerified;
    }
    staged.bytes[Entry::LocalFlags]=flags;
    int slot=found;
    if(slot<0){slot=allocate();if(slot<0)return slot;entries_[unsigned(slot)]={};}
    auto &entry=entries_[unsigned(slot)];
    if(found<0||changedKey||entry.placeholder()||(ignore&&!entry.ignored()))replayEpoch_[slot]=0;
    if(found<0||memcmp(entry.bytes,staged.bytes,Entry::Size)) {
      memcpy(entry.bytes,staged.bytes,Entry::Size);dirty_=true;
    }
    if(ignore||changedKey){entry.clearSamples();entry.heard=false;}
    if(!ignore&&(!autoFavorite||!canProtect))Entry::put32(entry.lastHeardBytes,heardNow);
    return slot;
  }
  // Non-mutating admission is shared by endpoint response processing. Actual
  // cache updates stay behind the companion's received-packet duplicate guard.
  int permits(const Entry &incoming,uint32_t self,uint8_t hops)const {
    if(frozen_)return -EACCES;
    if(!Entry::valid(incoming.bytes,self)||incoming.bytes[Entry::LocalFlags]||hops>7)return -EINVAL;
    const int found=find(incoming.id());
    if(found>=0&&entries_[found].ignored())return -EACCES;
    if(found>=0&&entries_[found].bytes[Entry::KeyLength]==32&&
       (incoming.bytes[Entry::KeyLength]!=32||memcmp(incoming.bytes+Entry::Key,entries_[found].bytes+Entry::Key,32)))return -EACCES;
    return 0;
  }
  // Returns the accepted slot even for an unchanged observation. Both USB and
  // BLE must still emit their own NodeInfo event for the same received frame.
  int accept(const Entry &incoming,uint32_t self,int8_t snr,uint8_t hops){
    const int allowed=permits(incoming,self,hops);if(allowed)return allowed;
    const int found=find(incoming.id());unsigned slot;
    if(found>=0)slot=unsigned(found);
    else {const int available=allocate();if(available<0)return available;slot=unsigned(available);}
    auto &entry=entries_[slot];
    if(found<0||entry.placeholder()||entry.bytes[Entry::KeyLength]!=incoming.bytes[Entry::KeyLength]||
       memcmp(entry.bytes+Entry::Key,incoming.bytes+Entry::Key,incoming.bytes[Entry::KeyLength])){
      replayEpoch_[slot]=0;entry.clearSamples();
    }
    if(found<0)entry={}; // A reused slot must not inherit another peer's UTC.
    const uint8_t local=found>=0?entry.bytes[Entry::LocalFlags]&Entry::DurablePolicy:0;
    if(found<0||memcmp(entry.bytes,incoming.bytes,Entry::LocalFlags)||entry.placeholder()){
      memcpy(entry.bytes,incoming.bytes,Entry::Size);entry.bytes[Entry::LocalFlags]=local;dirty_=true;
    }
    entry.snr=snr;entry.hops=hops;entry.heard=true;return int(slot);
  }
};
// The two-hour census uses a radio/civil timestamp. An unavailable clock
// or timestamp cannot establish recency. A future stamp tolerates clock corrections.
inline bool onlineAt(uint32_t heard,uint32_t utc){return heard&&utc&&(heard>=utc||utc-heard<7200);}
inline uint16_t hotOnline(const NodeCache &hot,uint32_t utc){
    if(!utc)return 0;
    uint16_t count=1;
    for(unsigned i=0;i<hot.count();++i)if(onlineAt(hot.get(i)->lastHeard(),utc))++count;
    return count;
}
static_assert(sizeof(NodeCache::Entry)<=288,"Node cache entry RAM budget");
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
static_assert(sizeof(NodeCache)<=NodeCache::Capacity*(sizeof(NodeCache::Entry)+4)+20,"PSRAM node cache budget");
#else
static_assert(sizeof(NodeCache)<=4676,"Shared node cache RAM budget");
#endif
}
