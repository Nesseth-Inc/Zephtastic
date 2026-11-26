// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "NodeCache.h"
#include "SettingsJournal.h"
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
#include "LargeNodeRecords.h"
#endif

namespace mt {
class NodeJournal {
public:
  static constexpr size_t HeaderSize=16,RecordSize=HeaderSize+NodeCache::LegacyCapacity*NodeCache::Entry::Size+4;
  static constexpr size_t RouteHintOffset=RecordSize-4,HintRecordSize=RecordSize+NodeCache::LegacyCapacity;
  static constexpr size_t EpochOffset=HintRecordSize-4,MaxRecordSize=HintRecordSize+4*NodeCache::LegacyCapacity;
  static constexpr size_t LargeRecordSize=HeaderSize+NodeCache::Capacity*(NodeCache::Entry::Size+5)+4;
  static constexpr uint32_t SettleMs=60000,IntervalMs=600000;
  enum class State:uint8_t {Unloaded,Fresh,Loaded,RecoveredFrozen,Invalid,IoError};
private:
#if defined(CONFIG_MT_PEER_DIRECTORY)
  int (*directoryFence_)(uint32_t,uint32_t)=nullptr;
#endif
  State state_=State::Unloaded;
  uint32_t generation_=0,self_=0,dirtyAt_=0,lastWrite_=0;
  // Zero is the BSS-safe construction value. While unloaded, expose the same
  // no-active-slot sentinel as the loaded journal uses internally (-1).
  int8_t active_=0;bool pending_=false,wrote_=false,trackEpochs_=false;
  // Per-journal scratch, never shared globally. All load/edit/flush callers
  // serialize on serviceLock; edit may call flush but must not be re-entered.
  // Keeping the candidate here avoids stacking it with the 1492-byte wire
  // record and the caller's protobuf/main-loop frames on the 8 KiB main stack.
  NodeCache candidate_;
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
  LargeNodeRecords external_;
  bool capacityBackpressure_=false;
  uint8_t buffer_[LargeRecordSize]{};
  void expand(NodeCache &c){c.expanded_=true;c.next_=0;}
#endif
  static void clearCache(NodeCache &c){
    for(unsigned i=0;i<c.count_;++i){c.entries_[i]={};c.replayEpoch_[i]=0;}
    c.count_=c.next_=0;c.dirty_=c.frozen_=false;
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    c.expanded_=false;
#endif
  }
  static unsigned slots(uint8_t version){return version==8?NodeCache::Capacity:NodeCache::LegacyCapacity;}
  static size_t hints(uint8_t v){return HeaderSize+slots(v)*NodeCache::Entry::Size;}
  static size_t epochs(uint8_t v){return hints(v)+slots(v);}
  static size_t length(uint8_t v){return v>=7?epochs(v)+4*slots(v)+4:v==6?HintRecordSize:RecordSize;}
  static const char *key(unsigned slot){return slot?"mtbase/nodes1":"mtbase/nodes0";}
  static uint32_t crc32(const uint8_t *p,size_t n){
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}
    return ~c;
  }
  static const uint8_t *entry(const uint8_t *raw,unsigned i){return raw+HeaderSize+i*NodeCache::Entry::Size;}
  static bool valid(const uint8_t *raw,int n,uint32_t self){
    using E=NodeCache::Entry;
    if(n<int(HeaderSize)||raw[0]!='Z'||raw[1]!='N'||raw[2]!='J'||raw[3]<1||
       raw[3]>(NodeCache::Capacity>16?8:7)||n!=int(length(raw[3]))||
       raw[8]>slots(raw[3])||raw[9]>=slots(raw[3])||(raw[8]<slots(raw[3])&&raw[9])||
       raw[10]!=E::Size||raw[11]||raw[12]||raw[13]||raw[14]||raw[15]||
       E::get32(raw+n-4)!=crc32(raw,size_t(n-4)))return false;
    unsigned protectedNodes=0;
    for(unsigned i=0;i<raw[8];++i){
      const auto *e=entry(raw,i);if(!E::valid(e,self))return false;
      if(raw[3]==1&&e[E::LocalFlags])return false;
      if(raw[3]==2&&(e[E::LocalFlags]&E::Muted))return false;
      if(raw[3]<4&&(e[E::LocalFlags]&E::ManualVerified))return false;
      // v1-v4 reserved peer metadata bit 2 and local bit 5. Reject before
      // migration so legacy bytes are never reinterpreted as licensed/signed.
      if(raw[3]<5&&((e[E::Flags]&E::LicensedUser)||(e[E::LocalFlags]&E::VerifiedSigner)))return false;
      if(e[E::LocalFlags]&E::Protected)++protectedNodes;
      for(unsigned j=0;j<i;++j)if(E::get32(e)==E::get32(entry(raw,j)))return false;
    }
    if(protectedNodes>(raw[3]==8?NodeCache::ProtectedLimit:NodeCache::LegacyCapacity-2))return false;
    if(raw[3]>=7){
      if(!E::get32(raw+4))return false;
      for(unsigned i=0;i<slots(raw[3]);++i){const auto n=E::get32(raw+epochs(raw[3])+4*i);
        if((i<raw[8])!=bool(n)||n>E::get32(raw+4))return false;}
    }
    if(raw[3]>=6)for(unsigned i=raw[8];i<slots(raw[3]);++i)if(raw[hints(raw[3])+i])return false;
    for(size_t i=HeaderSize+raw[8]*E::Size;i<hints(raw[3]);++i)if(raw[i])return false;
    return true;
  }
  static bool matches(const uint8_t *raw,const NodeCache &cache){
    if(raw[8]!=cache.count_||raw[9]!=cache.next_||(raw[3]==8)!=cache.expanded())return false;
    for(unsigned i=0;i<cache.count_;++i)if(memcmp(entry(raw,i),cache.entries_[i].bytes,NodeCache::Entry::Size)||
      (raw[3]>=6?raw[hints(raw[3])+i]:0)!=cache.entries_[i].routeHint||
      (raw[3]>=7&&NodeCache::Entry::get32(raw+epochs(raw[3])+4*i)!=(cache.replayEpoch_[i]?cache.replayEpoch_[i]:NodeCache::Entry::get32(raw+4))))return false;
    return true;
  }
  static void restore(const uint8_t *raw,NodeCache &cache){
    clearCache(cache);cache.count_=raw[8];cache.next_=raw[9];
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    cache.expanded_=raw[3]==8;
#endif
    for(unsigned i=0;i<cache.count_;++i){
      memcpy(cache.entries_[i].bytes,entry(raw,i),NodeCache::Entry::Size);
      if(raw[3]>=7)cache.replayEpoch_[i]=NodeCache::Entry::get32(raw+epochs(raw[3])+4*i);
      if(raw[3]>=6)cache.entries_[i].routeHint=raw[hints(raw[3])+i];
    }
    // RSSI/SNR/hops/heard-current have no durable meaning without trustworthy
    // time and radio-profile history. They remain unknown after restoration.
  }
  size_t encode(uint8_t *raw,const NodeCache &cache,uint32_t generation)const{
    memset(raw,0,cache.expanded()?LargeRecordSize:MaxRecordSize);raw[0]='Z';raw[1]='N';raw[2]='J';raw[3]=3;
    for(unsigned i=0;i<cache.count_;++i){
      if(cache.entries_[i].verifiedSigner()||cache.entries_[i].licensed()){raw[3]=5;break;}
      if(cache.entries_[i].manuallyVerified())raw[3]=4;
    }
    NodeCache::Entry::put32(raw+4,generation);raw[8]=cache.count_;raw[9]=cache.next_;raw[10]=NodeCache::Entry::Size;
    for(unsigned i=0;i<cache.count_;++i)memcpy(raw+HeaderSize+i*NodeCache::Entry::Size,cache.entries_[i].bytes,NodeCache::Entry::Size);
    if(cache.expanded())raw[3]=8;
    else if(trackEpochs_)raw[3]=7;
    for(unsigned i=0;i<cache.count_;++i){
      raw[hints(raw[3])+i]=cache.entries_[i].routeHint;
      if(raw[3]<6&&raw[hints(raw[3])+i])raw[3]=6;
    }
    if(raw[3]>=7){
      for(unsigned i=0;i<cache.count_;i++)NodeCache::Entry::put32(raw+epochs(raw[3])+4*i,
        cache.replayEpoch_[i]?cache.replayEpoch_[i]:generation);
    }
    const size_t length=NodeJournal::length(raw[3]);
    NodeCache::Entry::put32(raw+length-4,crc32(raw,length-4));return length;
  }
  int fail(NodeCache &cache,State state,int error){state_=state;cache.frozen_=true;return error;}
public:
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
  void files(LargeNodeFiles &files){external_.bind(files);}
  // A full inactive file was read back as absent or older. A partial key
  // installation or any uncertain metadata state must still hold the owner.
  bool verifiedCapacityRefusal(int rc,const NodeCache &cache)const{
    return rc==-ENOSPC&&capacityBackpressure_&&ready()&&cache.ready();
  }
#endif
  void enableReplayEpochs(NodeCache &cache){
    if(trackEpochs_||!ready()||!cache.ready())return;
    trackEpochs_=true;if(cache.count())cache.dirty_=true;
  }
  State state()const{return state_;}
  bool ready()const{return state_==State::Fresh||state_==State::Loaded;}
#if defined(CONFIG_MT_PEER_DIRECTORY)
  void directoryFence(int (*hook)(uint32_t,uint32_t)){directoryFence_=hook;}
#endif
  using BeforeEdit=int (*)(uint32_t,NodeCache::Edit);
  using BeforeOwnerContact=int (*)(uint32_t);
  uint32_t generation()const{return generation_;}
  int activeSlot()const{return state_==State::Unloaded?-1:active_;}
  // Explicit local NodeDB mutation: journal first, publish live policy second.
  // An uncertain commit freezes the prior live cache and requires recovery;
  // callers must keep RF off on failure rather than use an unverified policy.
  int edit(SettingsBackend &backend,NodeCache &cache,uint32_t id,NodeCache::Edit op,uint32_t now,bool quiescent,bool *committed=nullptr,BeforeEdit before=nullptr){
    if(committed)*committed=false;
    if(!ready()||!cache.ready())return -EACCES;
    if(!quiescent)return -EBUSY;
    candidate_=cache;
    int changed=candidate_.edit(id,self_,op);
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    if(changed==-ENOSPC&&!candidate_.expanded()&&external_.available()){
      expand(candidate_);changed=candidate_.edit(id,self_,op);
    }
#endif
    if(changed<0)return changed;
    // Remove and Ignore can retire archived metadata even for a hot-cache no-op.
    // The caller may withdraw it only after the edit passed all admission checks.
    if(before&&(changed||op==NodeCache::Edit::Remove||op==NodeCache::Edit::Ignore)){
      const int rc=before(id,op);if(rc)return rc;
    }
    // An evicted peer may still exist in the last durable generation. A
    // remove of its now-missing ID must persist pending cache changes first.
    if(!changed&&!(op==NodeCache::Edit::Remove&&candidate_.dirty()))return 0;
    return commitCandidate(backend,cache,now,committed);
  }
  // Bounded staging uses this journal's existing scratch; no second cache is
  // placed on the main stack. Callers serialize preview and commit.
  int previewOwnerContact(const NodeCache &cache,const NodeCache::Entry &incoming,bool ignore,
                          bool manual,NodeCache::Entry &resolved,bool autoFavorite=true,uint32_t heardNow=0) {
    if(!ready()||!cache.ready())return -EACCES;
    candidate_=cache;
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    const int existing=candidate_.find(incoming.id());
    if(!candidate_.expanded() && candidate_.protectedCount()>=NodeCache::LegacyCapacity-2 &&
       (ignore||manual||autoFavorite) && (existing<0||!candidate_.get(existing)->protectedNode()) && external_.available())expand(candidate_);
#endif
    const int slot=candidate_.ownerContact(incoming,self_,ignore,manual,autoFavorite,heardNow);
    if(slot<0)return slot;
    resolved=*candidate_.get(unsigned(slot));return 0;
  }
  // Same verified candidate-before-publish transaction as ordinary local edits.
  int ownerContact(SettingsBackend &backend,NodeCache &cache,const NodeCache::Entry &incoming,
                   bool ignore,bool manual,uint32_t now,bool quiescent,bool autoFavorite=true,uint32_t heardNow=0,BeforeOwnerContact before=nullptr) {
    if(!ready()||!cache.ready())return -EACCES;
    if(!quiescent)return -EBUSY;
    candidate_=cache;
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    const int existing=candidate_.find(incoming.id());
    if(!candidate_.expanded() && candidate_.protectedCount()>=NodeCache::LegacyCapacity-2 &&
       (ignore||manual||autoFavorite) && (existing<0||!candidate_.get(existing)->protectedNode()) && external_.available())expand(candidate_);
#endif
    const int slot=candidate_.ownerContact(incoming,self_,ignore,manual,autoFavorite,heardNow);
    if(slot<0)return slot;
    if(!candidate_.dirty())return 0;
    if(before){const int rc=before(incoming.id());if(rc)return rc;}
    return commitCandidate(backend,cache,now,nullptr);
  }
  int commitCandidate(SettingsBackend &backend,NodeCache &cache,uint32_t now,bool *committed){
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    if(candidate_.expanded()&&candidate_.count_<=NodeCache::LegacyCapacity &&
       candidate_.protectedCount()<=NodeCache::LegacyCapacity-2){
      candidate_.expanded_=false;candidate_.next_=0;
    }
#endif
    int rc=flush(backend,candidate_,now,true,true);
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    // Restore both old-format selector slots before acknowledging downgrade.
    if(rc==1&&!candidate_.expanded()&&external_.needed()){
      candidate_.dirty_=true;rc=flush(backend,candidate_,now,true,true);
      if(rc!=1)return fail(cache,State::IoError,rc?rc:-EIO);
    }
#endif
    if(rc!=1){
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
      if(rc!=-ENOSPC||!capacityBackpressure_||!ready())cache.frozen_=true;
#else
      cache.frozen_=true;
#endif
      return rc?rc:-EIO;
    }
    cache=candidate_;if(committed)*committed=true;return 0;
  }
  int load(SettingsBackend &backend,NodeCache &cache,uint32_t self){
    clearCache(cache);state_=State::Unloaded;generation_=0;self_=self;active_=-1;pending_=wrote_=trackEpochs_=false;dirtyAt_=lastWrite_=0;
    if(self<=1||self>=0xfffffffeu)return fail(cache,State::Invalid,-EINVAL);
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    auto &raw=buffer_;external_.reset();
#else
    uint8_t raw[MaxRecordSize];
#endif
    bool bad=false;
    for(unsigned i=0;i<2;++i){
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
      const int n=external_.read(backend,key(i),i,self,raw,sizeof(raw));
#else
      const int n=backend.read(key(i),raw,sizeof(raw));
#endif
      if(n==-ENOENT)continue;
      if(n<0&&n!=-E2BIG)return fail(cache,State::IoError,n);
      if(!valid(raw,n,self)){bad=true;continue;}
      const uint32_t generation=NodeCache::Entry::get32(raw+4);
      if(active_>=0){
        const uint32_t delta=generation-generation_;
        if((!delta&&((raw[3]>=7)!=trackEpochs_||!matches(raw,cache)))||delta==0x80000000u)return fail(cache,State::Invalid,-EINVAL);
        if(!delta||delta>0x80000000u)continue;
      }
      restore(raw,cache);trackEpochs_=raw[3]>=7;generation_=generation;active_=int8_t(i);
    }
    if(bad)return fail(cache,active_>=0?State::RecoveredFrozen:State::Invalid,-EILSEQ);
    state_=active_>=0?State::Loaded:State::Fresh;
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    if(!cache.expanded()&&external_.needed())cache.dirty_=true;
#endif
    return 0;
  }
  void observe(const NodeCache &cache,uint32_t now){
    if(ready()&&cache.ready()&&cache.dirty_&&!pending_){pending_=true;dirtyAt_=now;}
  }
  bool due(uint32_t now)const{
    return ready()&&pending_&&uint32_t(now-dirtyAt_)>=SettleMs&&(!wrote_||uint32_t(now-lastWrite_)>=IntervalMs);
  }
  // The caller must hold serviceLock and prove healthy RF fully off with no
  // active or queued TX before passing quiescent=true. In particular, cleared
  // receive flags after a failed radio stop are NOT proof of quiescence.
  // This method never stops/disarms a radio. Force is only for an already
  // quiesced explicit local NodeDB edit or shutdown/reset.
  // Returns 1 for verified commit, 0 for clean, -EAGAIN for deferral, or errno.
  int flush(SettingsBackend &backend,NodeCache &cache,uint32_t now,bool quiescent,bool force=false){
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    capacityBackpressure_=false;
#endif
    if(!ready()||!cache.ready())return -EACCES;
    if(!cache.dirty_)return 0;
    observe(cache,now);
    if(!quiescent||(!force&&!due(now)))return -EAGAIN;
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    auto &raw=buffer_;
#else
    uint8_t raw[MaxRecordSize];
#endif
    const uint32_t next=generation_+1;const unsigned slot=active_==0?1:0;
    if(trackEpochs_&&!next)return fail(cache,State::Invalid,-EOVERFLOW);
    const size_t length=encode(raw,cache,next);
    if(!valid(raw,int(length),self_))return fail(cache,State::Invalid,-EINVAL);
#if defined(CONFIG_MT_PEER_DIRECTORY)
    if(directoryFence_){const int fenced=directoryFence_(generation_,next);if(fenced)return fenced;}
#endif
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    const bool retiring=!cache.expanded()&&external_.selected(slot);
    bool fileFull=false;
    const int ret=external_.write(backend,key(slot),slot,self_,raw,length,cache.expanded(),fileFull);
    if(fileFull){
      // Even an out-of-space return can follow an uncertain rename. Resume
      // only if the durable inactive selector still resolves to absent/older.
      const int n=external_.read(backend,key(slot),slot,self_,raw,sizeof(raw));
      if(n==-ENOENT||(valid(raw,n,self_)&&NodeCache::Entry::get32(raw+4)!=next)){
        capacityBackpressure_=true;return -ENOSPC;
      }
    }
#else
    const int ret=backend.write(key(slot),raw,length);
#endif
    if(ret)return fail(cache,State::IoError,ret);
    // Reuse the bounded serialization buffer. RAM cache is stable under the
    // service lock, so compare every persisted field against it after reading.
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    const int n=external_.read(backend,key(slot),slot,self_,raw,sizeof(raw));
#else
    const int n=backend.read(key(slot),raw,sizeof(raw));
#endif
    if(n!=int(length)||!valid(raw,n,self_)||NodeCache::Entry::get32(raw+4)!=next||!matches(raw,cache))
      return fail(cache,State::IoError,n<0?n:-EIO);
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    if(retiring){const int erased=external_.retired(slot);if(erased)return fail(cache,State::IoError,erased);}
    if(cache.expanded())trackEpochs_=true;
#endif
    if(trackEpochs_)for(unsigned i=0;i<cache.count_;++i)cache.replayEpoch_[i]=NodeCache::Entry::get32(raw+epochs(raw[3])+4*i);
    generation_=next;active_=int8_t(slot);state_=State::Loaded;
    cache.dirty_=false;pending_=false;wrote_=true;lastWrite_=now;return 1;
  }
};
static_assert(NodeJournal::RecordSize==1492);
static_assert(sizeof(NodeJournal)<=sizeof(NodeCache)+32
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
              +NodeJournal::LargeRecordSize+sizeof(LargeNodeRecords)+8
#endif
#if defined(CONFIG_MT_PEER_DIRECTORY)
              +sizeof(int (*)(uint32_t,uint32_t))
#endif
              ,"Node journal scratch and bookkeeping RAM budget");
}
