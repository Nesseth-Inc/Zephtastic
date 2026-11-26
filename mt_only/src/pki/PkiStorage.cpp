// SPDX-License-Identifier: GPL-3.0-only
#include "PkiStorage.h"
#include "PkiAppendHistory.h"
#include "provisioning/FreshDomain.h"
#include <string.h>
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
#include "ImportedNoncePermutation.h"
#endif
namespace mt::pki {
namespace {
constexpr const char *names[]={"mtidentity/state0","mtidentity/state1","mtpki/ledger0","mtpki/ledger1","mtpki/nonces0","mtpki/nonces1","mtpki/contacts0","mtpki/contacts1",PkiFreshProvisioning::RootKey,PkiFreshIntent::Key};
int index(const char *key){
  if(!key)return -1;
  for(unsigned i=0;i<10;++i)if(!strcmp(key,names[i]))return int(i);
  return -1;
}
const char *counterKey(unsigned slot){return slot==0?"mtpki/nonces0":slot==1?"mtpki/nonces1":nullptr;}
size_t recordSize(int i){return i<2?IdentityJournal::RecordSize:i<4?PkiLedger::RecordSize:i<6?PkiNonceCounter::RecordBytes:i<8?PkiContactSnapshot::RecordBytes:i==8?PkiFreshRoot::RecordBytes:PkiFreshIntent::RecordBytes;}
bool ledgerSize(size_t n){return n==PkiLedger::RecordSize||n==PkiLedger::LegacyRecordSize||
  n==PkiContactMigrationMarker::RecordBytes||n==PkiContactSnapshot::RecordBytes;}
}
PkiStorage::Lease::Lease(const PkiStorage &o):owner_(o){
  if(!owner_.context_.tryEnter())return;
  if(owner_.busy_){owner_.context_.leave();return;}
  owner_.busy_=true;held_=true;
}
PkiStorage::Lease::~Lease(){if(held_){owner_.busy_=false;owner_.context_.leave();}}
PkiStorage::PkiStorage(BootSettings &boot,SettingsBackend &runtime,PkiStorageContext &context,EntropySource &entropy,PkiAppendHistory *append):
  boot_(boot),runtimeBackend_(runtime),context_(context),
#if !defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  entropy_(entropy),
#endif
  append_(append),backend_(*this),
  contacts_(backend_,ledger_),counterStore_(*this),counter_(counterStore_),nonce_(identity_,ledger_,backend_,entropy,context),replay_(identity_,ledger_,backend_,context){}
PkiStorage::~PkiStorage(){
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  eraseIdentityBytes(importPermutation_,sizeof(importPermutation_));
#endif
  if(append_)append_->close();identity_.close();ledger_.close();}
PkiStorage::State PkiStorage::state()const{Lease hold(*this);return hold?state_:State::Busy;}
int PkiStorage::lastError()const{Lease hold(*this);return hold?error_:-EBUSY;}
int PkiStorage::fail(State state,int error){
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  imported_=false;eraseIdentityBytes(importPermutation_,sizeof(importPermutation_));
#endif
  counter_.close();window_.clear();bounded_=false;nextWindow_=0;if(append_)append_->close();identity_.close();ledger_.close();phase_=Phase::None;state_=state;error_=error;return error;}
bool PkiStorage::usable()const{
  IdentityBinding b;KeyRef ref;
  if(freshImplementation())return freshDomain_&&state_==State::Active&&counter_.active()&&freshReady()&&
    identity_.binding(b,ref)&&freshRoot_.matches(b);
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  return state_==State::Active&&extended_&&append_&&append_->ready()&&bounded_&&
    counter_.active()&&contacts_.ready()&&identity_.binding(b,ref)&&ledger_.boundTo(b.publicKey,b.node);
#else
  return state_==State::Active&&identity_.binding(b,ref)&&ledger_.boundTo(b.publicKey,b.node)&&
    (!extended_||(append_&&append_->ready()))&&(!bounded_||(counter_.active()&&
      contacts_.state()!=PkiInplaceContacts::State::New&&contacts_.state()!=PkiInplaceContacts::State::Frozen));
#endif
}
void PkiStorage::observeFailure(){
  if(!usable())fail(State::Frozen,-EIO);
}
int PkiStorage::Backend::read(const char *name,uint8_t *out,size_t cap){
  const int i=index(name);const size_t expected=recordSize(i);
  if(i<0||!out||cap!=expected||!owner_.busy_)return -EACCES;
  int n=-EACCES;
  if(owner_.phase_==Phase::Boot)n=owner_.boot_.read(name,out,cap);
  else if(owner_.phase_==Phase::Runtime&&owner_.context_.quiescent()){
    n=owner_.runtimeBackend_.read(name,out,cap);
    if(!owner_.context_.quiescent())n=-EACCES;
  }
  const bool supported=n==int(expected)||(i>=2&&i<4&&n>0&&ledgerSize(size_t(n)));
  if(!supported){eraseIdentityBytes(out,cap);return n<0?n:-EILSEQ;}
  return n;
}
int PkiStorage::Backend::write(const char *name,const uint8_t *data,size_t length){
  const int i=index(name);
  if(i<2||!data||!(i<4?ledgerSize(length):length==recordSize(i))||!owner_.busy_||
     owner_.phase_!=Phase::Runtime||owner_.state_!=State::Active||!owner_.context_.quiescent())return -EACCES;
  return owner_.runtimeBackend_.write(name,data,length);
}
int PkiStorage::CounterStore::read(unsigned slot,uint8_t *out,size_t size){
  return slot<2?owner_.backend_.read(counterKey(slot),out,size):-EINVAL;
}
int PkiStorage::CounterStore::write(unsigned slot,const uint8_t *data,size_t size){
  return slot<2?owner_.backend_.write(counterKey(slot),data,size):-EINVAL;
}
bool PkiStorage::counterBinding(PkiNonceCounterBinding &out)const{
  IdentityBinding local;KeyRef ref;AppendAnchor anchor;PkiLedger::Status status;
  if(!extended_||!append_||!identity_.binding(local,ref)||
     !ledger_.appendAnchor(anchor)||!append_->status(status))return false;
  out={};out.node=local.node;memcpy(out.publicKey,local.publicKey,32);
  out.legacyGeneration=status.generation;out.legacyCrc=anchor.rootCrc;return true;
}
int PkiStorage::windowIndex(uint32_t node,const uint8_t *pub,uint32_t id)const{
  for(size_t i=0;i<PkiReplayWindow::PeerLimit;++i)
    if(window_.contains(i,node,pub,id)!=PkiReplayWindow::Result::Unknown)return int(i);
  return -1;
}
bool PkiStorage::legacyPins(PkiPeerPin *out,size_t capacity,size_t &count)const{
  uint32_t nodes[PkiPeerSet::Limit]{};size_t n=0;
  if(!out||capacity<PkiPeerSet::Limit||!append_||!append_->activePeers(nodes,PkiPeerSet::Limit,n))return false;
  for(size_t i=0;i<n;++i){out[i].node=nodes[i];if(!append_->lookup(nodes[i],out[i].publicKey))return false;}
  count=n;return true;
}
int PkiStorage::loadContacts(){
  IdentityBinding local;KeyRef ref;AppendAnchor root;PkiPeerPin pins[PkiPeerSet::Limit]{};size_t count=0;
  if(!identity_.binding(local,ref)||!ledger_.appendAnchor(root)||!legacyPins(pins,PkiPeerSet::Limit,count))return -EILSEQ;
  return contacts_.load(local,root,pins,count);
}
bool PkiStorage::contactPin(uint32_t node,uint8_t *out)const{
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  if(expandedActive()){
    const auto *p=expanded_->pins().find(node);if(!p)return false;
    if(out)memcpy(out,p->publicKey,32);return true;
  }
#endif
  if(freshImplementation()){PkiContactSnapshot::Value value;if(!freshContacts_.snapshot(value))return false;
    for(size_t i=0;i<value.count;++i)if(value.contacts[i].node==node){if(out)memcpy(out,value.contacts[i].publicKey,32);return true;}return false;}
  PkiContactSnapshot::Value value;if(!contacts_.snapshot(value))return false;
  for(size_t i=0;i<value.count;++i)if(value.contacts[i].node==node){
    if(out)memcpy(out,value.contacts[i].publicKey,32);return true;
  }
  return false;
}
ReplayCheck PkiStorage::boundedLegacy(uint32_t node,const uint8_t *pub,uint32_t id)const{
  if(contacts_.ready()){
    uint8_t current[32];if(!pub||!id)return ReplayCheck::Invalid;
    if(!contactPin(node,current))return ReplayCheck::UnknownPeer;
    if(memcmp(current,pub,32))return ReplayCheck::WrongKey;
  }
  const auto result=contacts_.ready()?append_->historicalRecorded(node,pub,id):append_->recorded(node,pub,id);
  // The current contact snapshot supplies trust. A newly pinned/re-added
  // contact has no new permanent replay domain; old matching legacy IDs and
  // the bounded RAM window remain fenced, with the same global TX counter.
  return contacts_.ready()&&(result==ReplayCheck::UnknownPeer||result==ReplayCheck::WrongKey)?ReplayCheck::Fresh:result;
}
#if defined(CONFIG_MT_PKI_FRESH_DOMAIN_ONLY)
// Fresh-only firmware can use neither a legacy nonce authority nor an append
// ledger. Any surviving row blocks admission, including an unreadable row.
int PkiStorage::requireAbsent(unsigned end){
  uint8_t scratch[PkiLedger::RecordSize];
  static_assert(sizeof(scratch)>=PkiNonceCounter::RecordBytes&&sizeof(scratch)>=PkiContactSnapshot::RecordBytes);
  for(unsigned row=2;row<end;++row){
    const size_t size=recordSize(row);
    const int read=backend_.read(names[row],scratch,size);
    eraseIdentityBytes(scratch,size);
    if(read!=-ENOENT)return read<0?read:-EEXIST;
  }
  return 0;
}
#endif
int PkiStorage::prepare(){
  Lease hold(*this);if(!hold)return -EBUSY;
  if(state_!=State::New)return -EACCES;
  if(!context_.bootQuiescent())return -EAGAIN;
  const int start=boot_.begin();if(start)return start==-EBUSY?start:fail(start==-EILSEQ?State::Corrupt:State::Frozen,start);
  struct End {BootSettings &boot;~End(){boot.end();}} end{boot_};
  phase_=Phase::Boot;
  int rc=identity_.load(backend_,deriver_);
  if(rc)return fail(rc==-EILSEQ?State::Corrupt:State::Frozen,rc);
  const bool haveIdentity=identity_.state()==IdentityJournal::State::Ready;
  IdentityBinding b;KeyRef ref;
  if(haveIdentity&&!identity_.binding(b,ref))return fail(State::Mismatch,-EXDEV);
  uint8_t rootBytes[PkiFreshRoot::RecordBytes]{};
  rc=backend_.read(PkiFreshProvisioning::RootKey,rootBytes,sizeof(rootBytes));
  if(rc==int(sizeof(rootBytes))){
    PkiFreshRoot candidate;
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    const bool imported=!memcmp(rootBytes,"ZTI1",4);
    if(imported){
      if(!haveIdentity)return fail(State::Partial,-ENODATA);
      if(!ImportedDomain::matchesRoot(rootBytes,sizeof(rootBytes),b,candidate))
        return fail(State::Mismatch,-EXDEV);
    }else
#endif
    if(!PkiFreshRoot::decode(rootBytes,sizeof(rootBytes),candidate))return fail(State::Corrupt,-EILSEQ);
    if(!haveIdentity)return fail(State::Partial,-ENODATA);
    if(!identity_.binding(b,ref)||!candidate.matches(b))return fail(State::Mismatch,-EXDEV);
#if defined(CONFIG_MT_PKI_FRESH_DOMAIN_ONLY)
    rc=requireAbsent(4);
    if(rc)return fail(rc==-EILSEQ?State::Corrupt:rc==-EEXIST?State::Partial:State::Frozen,rc);
#else
    rc=ledger_.load(backend_);
    if(rc||ledger_.state()!=PkiLedger::State::Empty)return fail(State::Partial,rc?rc:-EEXIST);
#endif
    FreshSecret<PkiFreshIntent::RecordBytes> pending;FreshSecret<32> pendingScalar;IdentityBinding pendingBinding{};
    rc=backend_.read(PkiFreshIntent::Key,pending.bytes,sizeof(pending.bytes));
    if(rc!=int(sizeof(pending.bytes)))return fail(State::Corrupt,rc<0?rc:-EILSEQ);
    PkiNonceCounterBinding freshBinding{};freshBinding.node=b.node;memcpy(freshBinding.publicKey,b.publicKey,32);
    freshBinding.legacyGeneration=candidate.generation;freshBinding.legacyCrc=candidate.crc;
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    if(imported){
      uint8_t salt[ImportedDomain::SaltSize]{};
      if(!ImportedDomain::readIntent(pending.bytes,b,salt)||memcmp(salt,rootBytes+4,sizeof(salt)))
        return fail(State::Corrupt,-EILSEQ);
      ImportedDomain::keys(b,pending.bytes+16,salt,freshBinding,importPermutation_);imported_=true;
    }else
#endif
    if(!PkiFreshIntent::decode(pending.bytes,sizeof(pending.bytes),pendingBinding,pendingScalar.bytes,deriver_)||
       pendingBinding.generation!=b.generation||pendingBinding.node!=b.node||memcmp(pendingBinding.publicKey,b.publicKey,32))
      return fail(State::Corrupt,-EILSEQ);
    rc=counter_.open(freshBinding,true);if(rc)return fail(State::Frozen,rc);
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
    if(expanded_){rc=expanded_->load(backend_,b,candidate.crc);if(rc)return fail(State::Corrupt,rc);}
    if(!expandedActive())
#endif
    rc=freshContacts_.load(backend_,b,candidate.generation,candidate.crc);
    if(rc||!freshReady())return fail(State::Corrupt,rc?rc:-EILSEQ);
    freshRoot_=candidate;freshDomain_=true;bounded_=true;phase_=Phase::None;
    if(!context_.bootQuiescent())return fail(State::Frozen,-EIO);
    state_=State::Prepared;error_=0;return 0;
  }
  if(rc!=-ENOENT)return fail(rc==-EILSEQ?State::Corrupt:State::Frozen,rc<0?rc:-EILSEQ);
  uint8_t pendingIntent[PkiFreshIntent::RecordBytes]{};
  rc=backend_.read(PkiFreshIntent::Key,pendingIntent,sizeof(pendingIntent));eraseIdentityBytes(pendingIntent,sizeof(pendingIntent));
  if(rc!=-ENOENT)return fail(State::Partial,rc<0?rc:-EILSEQ);
#if defined(CONFIG_MT_PKI_FRESH_DOMAIN_ONLY)
  // Only the complete ZTFD branch above can activate an identity in this
  // layout. Never reinterpret an old key/ledger as a fresh counter domain.
  if(haveIdentity)return fail(State::Partial,-ENOTSUP);
  rc=requireAbsent(8);
  if(rc)return fail(rc==-EILSEQ?State::Corrupt:rc==-EEXIST?State::Partial:State::Frozen,rc);
  return fail(State::Empty,-ENOENT);
#else
  bool reconstructedBounded=false;
  if(haveIdentity){
    // Check nonce-slot presence before loading redundant NVS ledger values. A
    // selected bounded counter may bootstrap only from the immutable replayed
    // root, never from an older/partially migrated NVS contact ledger.
    uint8_t nonceRecord[PkiNonceCounter::RecordBytes]{};bool anyNonce=false;
    for(unsigned slot=0;slot<2;++slot){
      const int n=counterStore_.read(slot,nonceRecord,sizeof(nonceRecord));
      if(n==-ENOENT)continue;
      if(n!=int(sizeof(nonceRecord))){
        eraseIdentityBytes(nonceRecord,sizeof(nonceRecord));
        return fail(State::Partial,n<0?n:-EILSEQ);
      }
      anyNonce=true;
    }
    eraseIdentityBytes(nonceRecord,sizeof(nonceRecord));
    if(anyNonce&&append_){
      AppendAnchor reconstructedAnchor;
      rc=append_->reconstructLedgerBaseline(ledger_,b,reconstructedAnchor);
      if(rc)return fail(rc==-EILSEQ?State::Corrupt:State::Frozen,rc);
      rc=append_->open(ledger_,reconstructedAnchor);
      if(rc)return fail(rc==-EILSEQ?State::Corrupt:State::Frozen,rc);
      extended_=true;

      PkiLedger::Status replayed;
      if(!append_->counterMode()||append_->remaining()!=0||!append_->status(replayed)||
         replayed.format!=3||replayed.generation!=reconstructedAnchor.generation+PkiAppendHistory::EventLimit)
        return fail(State::Partial,-EILSEQ);

      IdentityBinding afterReplay;KeyRef afterReplayRef;
      if(!identity_.binding(afterReplay,afterReplayRef)||!identity_.active(afterReplayRef)||
         afterReplayRef.slot!=ref.slot||afterReplayRef.generation!=ref.generation||
         afterReplay.version!=b.version||afterReplay.algorithm!=b.algorithm||
         afterReplay.generation!=b.generation||afterReplay.node!=b.node||
         memcmp(afterReplay.publicKey,b.publicKey,sizeof(b.publicKey)))return fail(State::Mismatch,-EXDEV);

      PkiNonceCounterBinding nonceBinding;
      if(!counterBinding(nonceBinding))return fail(State::Corrupt,-EILSEQ);
      rc=counter_.open(nonceBinding,true); // Only the complete replayed BMIT root proves activation.
      if(rc)return fail(State::Frozen,rc);
      IdentityBinding afterNonce;KeyRef afterNonceRef;
      if(!identity_.binding(afterNonce,afterNonceRef)||!identity_.active(afterNonceRef)||
         afterNonceRef.slot!=ref.slot||afterNonceRef.generation!=ref.generation||
         afterNonce.version!=b.version||afterNonce.algorithm!=b.algorithm||
         afterNonce.generation!=b.generation||afterNonce.node!=b.node||
         memcmp(afterNonce.publicKey,b.publicKey,sizeof(b.publicKey))||!counter_.active())
        return fail(State::Mismatch,-EXDEV);
      bounded_=true;reconstructedBounded=true;
    }
  }
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  // The normal compact profile has no legacy/unbounded fallback. A prepared
  // runtime must come from the complete identity-bound append replay and its
  // selected bounded counter, never from a merely readable legacy ledger.
  if(!reconstructedBounded){
    rc=ledger_.load(backend_);if(rc)return fail(rc==-EILSEQ?State::Corrupt:State::Frozen,rc);
    const bool haveLedger=ledger_.state()==PkiLedger::State::Ready;
    if(!haveIdentity&&!haveLedger){
      uint8_t record[PkiNonceCounter::RecordBytes]{};
      for(unsigned slot=0;slot<2;++slot){const int n=counterStore_.read(slot,record,sizeof(record));
        if(n!=-ENOENT){eraseIdentityBytes(record,sizeof(record));return fail(State::Partial,n<0?n:-EILSEQ);}}
      uint8_t contact[PkiContactStore::RecordBytes]{};
      for(unsigned slot=0;slot<2;++slot){const int n=backend_.read(slot?"mtpki/contacts1":"mtpki/contacts0",contact,sizeof(contact));
        if(n!=-ENOENT){eraseIdentityBytes(record,sizeof(record));eraseIdentityBytes(contact,sizeof(contact));return fail(State::Partial,n<0?n:-EILSEQ);}}
      eraseIdentityBytes(record,sizeof(record));eraseIdentityBytes(contact,sizeof(contact));return fail(State::Empty,-ENOENT);
    }
    return fail(State::Partial,-ENODATA);
  }
#else
  if(!reconstructedBounded){
    rc=ledger_.load(backend_);
    if(rc)return fail(rc==-EILSEQ?State::Corrupt:State::Frozen,rc);
    const bool haveLedger=ledger_.state()==PkiLedger::State::Ready;
    if(!haveIdentity&&!haveLedger){
      uint8_t orphan[PkiNonceCounter::RecordBytes]{};int orphanRc=0;
      for(unsigned slot=0;slot<2;++slot){orphanRc=counterStore_.read(slot,orphan,sizeof(orphan));if(orphanRc!=-ENOENT)return fail(State::Partial,orphanRc<0?orphanRc:-EILSEQ);}
      uint8_t contact[PkiContactStore::RecordBytes]{};
      for(unsigned slot=0;slot<2;++slot){orphanRc=backend_.read(slot?"mtpki/contacts1":"mtpki/contacts0",contact,sizeof(contact));if(orphanRc!=-ENOENT)return fail(State::Partial,orphanRc<0?orphanRc:-EILSEQ);}
      return fail(State::Empty,-ENOENT);
    }
    if(haveIdentity!=haveLedger)return fail(State::Partial,-ENODATA);
    if(!identity_.binding(b,ref)||!ledger_.boundTo(b.publicKey,b.node))return fail(State::Mismatch,-EXDEV);
  // A legacy unanchored ledger is compatible only with the original nonce-free
  // format. Any surviving counter slot means the append selector may have been
  // lost or activation interrupted; never fall back to unbounded legacy IDs.
  const auto requireNonceSlotsAbsent=[this]() {
    uint8_t record[PkiNonceCounter::RecordBytes]{};
    int result=0;
    for(unsigned slot=0;slot<2;++slot){
      const int read=counterStore_.read(slot,record,sizeof(record));
      if(read!=-ENOENT){result=read<0?read:-EILSEQ;break;}
    }
    eraseIdentityBytes(record,sizeof(record));
    return result;
  };
  AppendAnchor anchor;
  if(ledger_.appendAnchor(anchor)) {
    if(!append_)return fail(State::Frozen,-ENOTSUP);
    rc=append_->open(ledger_,anchor);
    if(rc)return fail(rc==-EILSEQ?State::Corrupt:State::Frozen,rc);
    extended_=true;
    PkiNonceCounterBinding counterBindingValue;
    if(!counterBinding(counterBindingValue))return fail(State::Corrupt,-EILSEQ);
    if(append_->counterMode()){
      rc=counter_.open(counterBindingValue,true);
      if(rc)return fail(State::Frozen,rc);
      bounded_=true;
    }else{
      // Existing nonce records with an old selector mean activation may have
      // been attempted. Never program that flash word for a third time.
      rc=requireNonceSlotsAbsent();
      if(rc)return fail(State::Partial,rc);
    }
  }else{
    // Selector loss must not downgrade a persistent M148 identity/counter to
    // the unbounded legacy nonce path. Preserve compatibility only when both
    // bounded-counter slots are positively absent.
    rc=requireNonceSlotsAbsent();
    if(rc)return fail(State::Partial,rc);
  }
  }
#endif
  if(bounded_){rc=loadContacts();if(rc)return fail(State::Corrupt,rc);}
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  if(!haveIdentity||!reconstructedBounded||!extended_||!append_||!bounded_||
     !append_->counterMode()||!counter_.active()||!contacts_.ready())
    return fail(State::Partial,-ENODATA);
#endif
  if(!context_.bootQuiescent())return fail(State::Frozen,-EIO);
  phase_=Phase::None;state_=State::Prepared;return 0;
#endif
}
bool PkiStorage::preparedBinding(IdentityBinding &out)const{
  Lease hold(*this);if(!hold||(state_!=State::Prepared&&state_!=State::Active))return false;
  IdentityBinding candidate;KeyRef ref;
  if(!identity_.binding(candidate,ref)||(freshImplementation()?!freshRoot_.matches(candidate):!ledger_.boundTo(candidate.publicKey,candidate.node)))return false;
  out=candidate;return true;
}
bool PkiStorage::preparedPeer(uint32_t &out)const{
  Lease hold(*this);if(!hold||(state_!=State::Prepared&&state_!=State::Active))return false;
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  if(expandedActive()){if(expanded_->pins().count()!=1)return false;out=expanded_->pins().at(0)->node;return true;}
#endif
  if(freshImplementation()){PkiContactSnapshot::Value value;if(!freshContacts_.snapshot(value)||value.count!=1)return false;out=value.contacts[0].node;return true;}
  if(contacts_.ready()){PkiContactSnapshot::Value value;
    if(!contacts_.snapshot(value)||value.count!=1)return false;out=value.contacts[0].node;return true;}
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  return false;
#else
  if(!extended_)return ledger_.singleActivePeer(out);
  uint32_t nodes[4]{};size_t count=0;
  if(!append_->activePeers(nodes,4,count)||count!=1)return false;
  out=nodes[0];return true;
#endif
}
bool PkiStorage::preparedPeers(PkiPeerSet &out)const{
  Lease hold(*this);if(!hold||(state_!=State::Prepared&&state_!=State::Active))return false;
  IdentityBinding binding;KeyRef ref;
  if(!identity_.binding(binding,ref)||(freshImplementation()?!freshRoot_.matches(binding):!ledger_.boundTo(binding.publicKey,binding.node)))return false;
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  if(expandedActive()){
    const auto &pins=expanded_->pins();if(pins.count()>PkiPeerSet::Limit)return false;
    if(!pins.count())return out.assignEmpty(binding);
    return out.assignTrusted(binding,pins.at(0),pins.count());
  }
#endif
  if(freshImplementation()){PkiContactSnapshot::Value value;if(!freshContacts_.snapshot(value))return false;
    PkiPeerSet staged;if(value.count?!staged.assignTrusted(binding,value.contacts,value.count):!staged.assignEmpty(binding))return false;
    out=staged;return true;}
  if(contacts_.ready()){
    PkiContactSnapshot::Value value;if(!contacts_.snapshot(value))return false;
    PkiPeerSet staged;if(value.count?!staged.assignTrusted(binding,value.contacts,value.count):!staged.assignEmpty(binding))return false;
    out=staged;return true;
  }
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  return false;
#else
  uint32_t ids[PkiPeerSet::Limit]{};size_t count=0;
  if(extended_?!append_->activePeers(ids,PkiPeerSet::Limit,count):!ledger_.activePeers(ids,PkiPeerSet::Limit,count))return false;
  PkiPeerPin pins[PkiPeerSet::Limit]{};
  for(size_t i=0;i<count;++i){pins[i].node=ids[i];
    if(extended_?!append_->lookup(ids[i],pins[i].publicKey):!ledger_.lookup(ids[i],pins[i].publicKey))return false;}
  PkiPeerSet staged;if(count?!staged.assign(binding,pins,count):!staged.assignEmpty(binding))return false;
  out=staged;return true;
#endif
}
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
bool PkiStorage::preparedRuntimePeers(RuntimePinDirectory &out)const{
  if(expandedActive()){
    Lease hold(*this);if(!hold||(state_!=State::Prepared&&state_!=State::Active))return false;
    out=expanded_->pins();return true;
  }
  PkiPeerSet small;if(!preparedPeers(small))return false;out.import(small);return true;
}
int PkiStorage::enableExpanded(const IdentityBinding &expected){
  Lease hold(*this);if(!hold)return -EBUSY;
  if(!expanded_||!usable()||!freshDomain_||!context_.quiescent())return -EACCES;
  if(!freshRoot_.matches(expected))return -EXDEV;
  phase_=Phase::Runtime;
  const int rc=expanded_->enable(backend_,freshContacts_);
  phase_=Phase::None;
  if(expanded_->state()==ExpandedContacts::State::Frozen||!context_.quiescent())return fail(State::Frozen,rc?rc:-EIO);
  return rc;
}
#endif
bool PkiStorage::contactRevision(uint32_t &out)const{
  Lease hold(*this);if(!hold||(state_!=State::Prepared&&state_!=State::Active))return false;
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  if(expandedActive()){out=expanded_->generation();return true;}
#endif
  if(freshImplementation()){if(!freshDomain_||!freshReady())return false;out=freshContacts_.generation();return true;}
  if(contacts_.ready()){out=contacts_.generation();return true;}
  // Legacy journals without a separate contact snapshot also advance on
  // traffic. Retaining a private waypoint requires the exact same generation.
#if !defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  PkiLedger::Status status{};
  if(extended_?append_&&append_->status(status):ledger_.status(status)){out=status.generation;return true;}
#endif
  return false;
}
bool PkiStorage::ledgerStatus(PkiLedger::Status &out)const{
  if constexpr(FreshOnly)return false; // This API measures only legacy append/ledger history.
  Lease hold(*this);if(!hold||!usable())return false;
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  return extended_&&append_&&bounded_&&counter_.active()&&contacts_.ready()&&append_->status(out);
#else
  return extended_?append_->status(out):ledger_.status(out);
#endif
}
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
int PkiStorage::legacyProof(PkiLedger::Status &out,uint32_t &crc)const{
  PkiLedger::Status status;AppendAnchor anchor;
  if(!usable()||extended_||ledger_.appendAnchor(anchor)||!ledger_.status(status)||
     (status.format!=1&&status.format!=2)||!status.generation)return -EACCES;
  uint8_t seal[4]{};
  if(!ledger_.snapshotRead(PkiLedger::RecordSize-4,seal,4))return -EIO;
  const uint32_t value=uint32_t(seal[0])|(uint32_t(seal[1])<<8)|(uint32_t(seal[2])<<16)|(uint32_t(seal[3])<<24);
  eraseIdentityBytes(seal,sizeof(seal));out=status;crc=value;return 0;
}
int PkiStorage::probeLegacyTail(AppendStore &store,TailProbeResult &out,uint32_t &generation,uint32_t &snapshotCrc)const{
  Lease hold(*this);if(!hold)return -EBUSY;
  PkiLedger::Status before,after;uint32_t seal=0,check=0;
  const int admission=legacyProof(before,seal);if(admission)return admission;
  struct Scratch {
    TailProbeResult result;
    ~Scratch(){eraseIdentityBytes(this,sizeof(*this));}
  } scratch;
  const int rc=LegacyTailProbe::inspect(store,scratch.result);if(rc)return rc;
  if(legacyProof(after,check)||after.format!=before.format||after.generation!=before.generation||seal!=check)return -EAGAIN;
  out=scratch.result;generation=before.generation;snapshotCrc=seal;return 0;
}
int PkiStorage::prepareFormerCode(AppendStore &store,HistoryPageEraser &eraser,PreparationProgress &progress,
                                const IdentityBinding &expected,uint32_t generation,uint32_t crc,TailPreparationResult &out){
  out={};Lease hold(*this);if(!hold)return -EBUSY;
  if(formerCodeAttempted_)return -EALREADY; // Preserve the prior uncertainty receipt.
  // Measured on retained T096 by the reviewed read-only diagnostic. This is
  // necessary but not sufficient: the external one-use package also binds the
  // positive first-M70/former-code chronology, image and retained identity.
  constexpr uint8_t approved[32]={0xb9,0x13,0x6f,0xc2,0x1f,0x63,0xce,0x70,0x63,0xa8,0xf4,0x28,0x26,0x43,0x2f,0x05,
    0xf6,0x98,0x41,0x7c,0x51,0x62,0x62,0xff,0xe4,0x38,0xed,0x74,0x16,0x3b,0x48,0xee};
  struct Proof final:PreparationProgress {
    PkiStorage &owner;PreparationProgress &outer;const IdentityBinding &expected;uint32_t generation,crc;
    Proof(PkiStorage &o,PreparationProgress &p,const IdentityBinding &e,uint32_t g,uint32_t c):owner(o),outer(p),expected(e),generation(g),crc(c){}
    int checkpoint()override{
      const int rc=outer.checkpoint();if(rc)return rc<0?rc:-EIO;
      if(!owner.context_.quiescent())return -EACCES;
      IdentityBinding current;KeyRef ref;PkiLedger::Status status;uint32_t seal=0;
      if(!owner.identity_.binding(current,ref)||current.version!=expected.version||current.algorithm!=expected.algorithm||
         current.generation!=expected.generation||current.node!=expected.node||memcmp(current.publicKey,expected.publicKey,32))return -EINVAL;
      const int proof=owner.legacyProof(status,seal);if(proof)return proof;
      return status.generation==generation&&seal==crc?0:-EAGAIN;
    }
  } proof(*this,progress,expected,generation,crc);
  const int rc=FormerCodePreparation::run(store,eraser,proof,approved,formerCodeAttempted_,out);
  preparationResult_=out;preparationError_=rc;preparationObserved_=true;
  if(rc&&out.attempted)return fail(State::Frozen,rc);
  return rc;
}
bool PkiStorage::preparationReport(TailPreparationResult &out,int &error,bool &observed)const{
  Lease hold(*this);if(!hold)return false;
  out=preparationResult_;error=preparationError_;observed=preparationObserved_;return true;
}
void PkiStorage::preparationRestoreFailed(int error){
  Lease hold(*this);if(!hold||!preparationObserved_||error>=0)return;
  preparationResult_.complete=false;preparationError_=error;
}
#endif
bool PkiStorage::extendedStorage()const{Lease hold(*this);return hold&&usable()&&extended_;}
uint16_t PkiStorage::appendRemaining()const{Lease hold(*this);return hold&&usable()&&extended_?append_->remaining():0;}
int PkiStorage::enableLongStorage(const IdentityBinding &expected){
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  (void)expected;return -ENOTSUP;
#else
  Lease hold(*this);if(!hold)return -EBUSY;
  if(freshDomain_)return -EALREADY;
  if(!usable()||!context_.quiescent())return -EACCES;
  if(!append_)return -ENOTSUP;
  if(extended_)return -EALREADY;
  IdentityBinding current;KeyRef ref;
  if(!identity_.binding(current,ref)||current.version!=expected.version||
     current.algorithm!=expected.algorithm||current.generation!=expected.generation||
     current.node!=expected.node||memcmp(current.publicKey,expected.publicKey,32))return -EINVAL;
  AppendAnchor anchor;
  int rc=append_->initialize(ledger_,context_.quiescent(),anchor);
  if(rc) {
    // Nonblank refusal performs no program operation and leaves the original
    // legacy authority usable. Uncertain initialization holds all authority.
    if((rc!=-ENOTEMPTY&&rc!=-EAGAIN)||append_->lastError()!=0)return fail(State::Frozen,rc);
    return rc;
  }
  phase_=Phase::Runtime;rc=ledger_.installAppendAnchor(backend_,anchor,context_.quiescent());phase_=Phase::None;
  if(rc||!context_.quiescent())return fail(State::Frozen,rc?rc:-EIO);
  rc=append_->open(ledger_,anchor);
  if(rc||!context_.quiescent())return fail(State::Frozen,rc?rc:-EIO);
  extended_=true;return 0;
#endif
}
int PkiStorage::activate(uint32_t node){
  Lease hold(*this);if(!hold)return -EBUSY;
  if(state_!=State::Prepared)return -EACCES;
  if(!context_.bootQuiescent())return -EAGAIN;
  IdentityBinding b;KeyRef ref;
  if(!identity_.binding(b,ref))return fail(State::Frozen,-EIO);
  if(freshImplementation()){if(!freshDomain_||!freshRoot_.matches(b)||!counter_.active()||!freshReady())return fail(State::Frozen,-EIO);
    if(b.node!=node)return -EINVAL;state_=State::Active;return 0;}
  if(!ledger_.boundTo(b.publicKey,b.node))return fail(State::Frozen,-EIO);
  if(b.node!=node)return -EINVAL;
  state_=State::Active;return 0;
}
int PkiStorage::enableBoundedHistory(const IdentityBinding &expected){
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  (void)expected;return -ENOTSUP;
#else
  Lease hold(*this);if(!hold)return -EBUSY;
  if(!usable()||!extended_||!append_||!context_.quiescent())return -EACCES;
  if(bounded_||append_->counterMode())return -EALREADY;
  if(append_->remaining()!=0)return -EINVAL;
  PkiNonceCounterBinding binding;
  if(!counterBinding(binding)||binding.node!=expected.node||
     memcmp(binding.publicKey,expected.publicKey,32))return -EINVAL;
  phase_=Phase::Runtime;
  uint8_t record[PkiNonceCounter::RecordBytes];
  for(unsigned slot=0;slot<2;++slot){
    const int n=counterStore_.read(slot,record,sizeof(record));
    if(n!=-ENOENT){phase_=Phase::None;return fail(State::Partial,n<0?n:-EILSEQ);}
  }
  int rc=counter_.initialize(binding,true);
  if(rc)return fail(State::Frozen,rc);
  if(!context_.quiescent())return fail(State::Frozen,-EIO);
  rc=append_->activateCounterMode(true);
  if(rc)return fail(State::Frozen,rc);
  rc=counter_.open(binding,append_->counterMode());
  if(rc||!context_.quiescent())return fail(State::Frozen,rc?rc:-EIO);
  rc=loadContacts();if(rc)return fail(State::Corrupt,rc);
  window_.clear();nextWindow_=0;bounded_=true;phase_=Phase::None;return 0;
#endif
}
int PkiStorage::enableCompactContacts(const IdentityBinding &expected){
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  (void)expected;return -ENOTSUP;
#else
  Lease hold(*this);if(!hold)return -EBUSY;
  if(freshDomain_)return -EALREADY;
  if(!usable()||!bounded_||!context_.quiescent())return -EACCES;
  IdentityBinding current;KeyRef ref;
  if(!identity_.binding(current,ref)||current.version!=expected.version||current.algorithm!=expected.algorithm||
     current.generation!=expected.generation||current.node!=expected.node||memcmp(current.publicKey,expected.publicKey,32))return -EINVAL;
  if(contacts_.ready())return -EALREADY;
  phase_=Phase::Runtime;const int rc=contacts_.migrate();phase_=Phase::None;
  if(!context_.quiescent())return fail(State::Frozen,-EIO);
  observeFailure();return state_==State::Active?rc:-EIO;
#endif
}
bool PkiStorage::compactContacts()const{Lease hold(*this);return hold&&usable()&&(freshImplementation()?freshReady():contacts_.ready());}
uint32_t PkiStorage::contactGeneration()const{Lease hold(*this);
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  if(hold&&usable()&&expandedActive())return expanded_->generation();
#endif
  return hold&&usable()?(freshImplementation()?freshContacts_.generation():contacts_.generation()):0;}
bool PkiStorage::boundedHistory()const{Lease hold(*this);return hold&&usable()&&bounded_;}
uint32_t PkiStorage::nonceReservedThrough()const{Lease hold(*this);return hold&&usable()&&bounded_?counter_.reservedThrough():0;}
IdentityJournal *PkiStorage::runtimeIdentity(){Lease hold(*this);return hold&&usable()?&identity_:nullptr;}
bool PkiStorage::close(){Lease hold(*this);if(!hold)return false;fail(State::Closed,0);return true;}
bool PkiStorage::reserve(KeyRef ref,const uint8_t pub[32],uint32_t local,uint32_t id,uint32_t &extra){
  Lease hold(*this);if(!hold||!id||!usable()||!context_.quiescent())return false;
  uint32_t staged=0;
  phase_=Phase::Runtime;bool ok=false;
  if(freshImplementation()){IdentityBinding binding;KeyRef current;
    ok=pub&&counter_.active()&&freshReady()&&identity_.binding(binding,current)&&
      current.slot==ref.slot&&current.generation==ref.generation&&binding.node==local&&
      !memcmp(binding.publicKey,pub,32)&&freshRoot_.matches(binding)&&identity_.active(ref)&&
      counter_.reserve(staged)==0&&identity_.active(ref)&&counter_.active();
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    if(ok&&imported_){
      uint32_t permuted=0;
      ok=ImportedNoncePermutation::apply(importPermutation_,staged,permuted);
      // Zero is a valid stock wire extra. The bool result distinguishes
      // failure, so every output of the migration permutation is admissible.
      if(!ok){phase_=Phase::None;fail(State::Frozen,-EIO);return false;}
      staged=permuted;
    }
#endif
    phase_=Phase::None;if(!context_.quiescent()){eraseIdentityBytes(&staged,sizeof(staged));fail(State::Frozen,-EIO);return false;}
    observeFailure();if(ok)extra=staged;eraseIdentityBytes(&staged,sizeof(staged));return ok;
  }
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  IdentityBinding binding;KeyRef current;
  const bool match=extended_&&append_&&bounded_&&counter_.active()&&contacts_.ready()&&pub&&
    identity_.binding(binding,current)&&current.slot==ref.slot&&current.generation==ref.generation&&
    binding.node==local&&!memcmp(binding.publicKey,pub,32)&&ledger_.boundTo(pub,local)&&
    identity_.active(ref)&&append_->ready();
  // The immutable journal continues to fence old IDs; every new TX value is
  // reserved durably from the identity-bound bounded counter before publish.
  if(match&&!append_->sent(id)&&counter_.reserve(staged)==0&&
     identity_.active(ref)&&append_->ready()&&contacts_.ready())ok=true;
#else
  if(!extended_)ok=nonce_.reserve(ref,pub,local,id,staged);
  else {
    IdentityBinding binding;KeyRef current;
    const bool match=pub&&identity_.binding(binding,current)&&current.slot==ref.slot&&current.generation==ref.generation&&
      binding.node==local&&!memcmp(binding.publicKey,pub,32)&&ledger_.boundTo(pub,local);
    if(match&&bounded_){
      // Legacy IDs remain forbidden. A future extra nonce is issued only from
      // the identity-bound range committed before use; ciphertext retries are
      // handled by PkiSession without reserving another value.
      ok=!append_->sent(id)&&counter_.reserve(staged)==0&&
        identity_.active(ref)&&append_->ready();
    }else if(match&&!append_->reserve(id,context_.quiescent())) {
      uint8_t random[4]{};
      if(entropy_.fill(random,4)&&identity_.active(ref)&&append_->ready()) {
        staged=uint32_t(random[0])|(uint32_t(random[1])<<8)|(uint32_t(random[2])<<16)|(uint32_t(random[3])<<24);ok=true;
      }
      eraseIdentityBytes(random,sizeof(random));
    }
  }
#endif
  phase_=Phase::None;
  if(!context_.quiescent()){eraseIdentityBytes(&staged,sizeof(staged));fail(State::Frozen,-EIO);return false;}
  observeFailure();if(ok)extra=staged;eraseIdentityBytes(&staged,sizeof(staged));return ok;
}
int PkiStorage::addTrustedPeer(const IdentityBinding &expected,const PkiPeerPin &pin){
  Lease hold(*this);if(!hold)return -EBUSY;
  if(!usable()||!context_.quiescent())return -EACCES;
  if(contacts_.state()==PkiInplaceContacts::State::Pending)return -EAGAIN;
  IdentityBinding current;KeyRef ref;PkiPeerSet check;
  if(!identity_.binding(current,ref)||current.version!=expected.version||
     current.algorithm!=expected.algorithm||current.generation!=expected.generation||
     current.node!=expected.node||memcmp(current.publicKey,expected.publicKey,32)||
     !check.assign(current,&pin,1))return -EINVAL;
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  if(expandedActive())return -ENOTSUP; // New pins require explicit local OwnerContact.
#endif
  phase_=Phase::Runtime;
  int result=0;
  if(freshImplementation()){PkiContactSnapshot::Value value;
    if(!freshReady()||!freshRoot_.matches(current)||!freshContacts_.snapshot(value))result=-EIO;
    else if(contactPin(pin.node,nullptr))result=-EEXIST;
    else if(value.count==PkiPeerSet::Limit)result=-ENOSPC;
    else{value.contacts[value.count++]=pin;result=freshContacts_.replace(backend_,value.contacts,value.count);}
    phase_=Phase::None;if(!context_.quiescent()){fail(State::Frozen,-EIO);return -EIO;}observeFailure();return state_==State::Active?result:-EIO;
  }
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  if(!extended_||!append_||!bounded_||!counter_.active()||!contacts_.ready())result=-ENOTSUP;
  else {
    PkiContactSnapshot::Value value;
    if(append_->revoked(pin.node))result=-EEXIST;
    else if(!contacts_.snapshot(value))result=-EIO;
    else if(contactPin(pin.node,nullptr))result=-EEXIST;
    else if(value.count==PkiPeerSet::Limit)result=-ENOSPC;
    else {value.contacts[value.count++]=pin;result=contacts_.replace(value.contacts,value.count);}
  }
#else
  if(contacts_.ready()){
    PkiContactSnapshot::Value value;
    if(append_&&append_->revoked(pin.node))result=-EEXIST;
    else if(!contacts_.snapshot(value))result=-EIO;
    else if(contactPin(pin.node,nullptr))result=-EEXIST;
    else if(value.count==PkiPeerSet::Limit)result=-ENOSPC;
    else {value.contacts[value.count++]=pin;result=contacts_.replace(value.contacts,value.count);}
  }else result=extended_?append_->addTrustedPeer(pin.node,pin.publicKey,context_.quiescent()):
    ledger_.addTrustedPeer(backend_,pin.node,pin.publicKey,context_.quiescent());
#endif
  phase_=Phase::None;
  if(!context_.quiescent()){fail(State::Frozen,-EIO);return -EIO;}
  observeFailure();
  return state_==State::Active?result:-EIO;
}
int PkiStorage::putOwnerPeer(const IdentityBinding &expected,const PkiPeerPin &pin,BeforeOwnerPin before){
  Lease hold(*this);if(!hold)return -EBUSY;
  if(!usable()||!context_.quiescent())return -EACCES;
  if(!freshImplementation()&&!contacts_.ready())return -ENOTSUP;
  IdentityBinding current;KeyRef ref;
  if(!identity_.binding(current,ref)||current.version!=expected.version||
     current.algorithm!=expected.algorithm||current.generation!=expected.generation||
     current.node!=expected.node||memcmp(current.publicKey,expected.publicKey,32))return -EINVAL;
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  if(expandedActive()){
    phase_=Phase::Runtime;const int rc=expanded_->put(backend_,pin,before);phase_=Phase::None;
    if(!expanded_->ready()||!context_.quiescent())return fail(State::Frozen,rc?rc:-EIO);
    return rc;
  }
#endif
  if(freshImplementation()){
    if(!freshReady())return -ENOTSUP;
    PkiContactSnapshot::Value value;if(!freshContacts_.snapshot(value)||value.count>PkiPeerSet::Limit)return -EIO;
    size_t existing=value.count;for(size_t i=0;i<value.count;++i){
      if(value.contacts[i].node==pin.node)existing=i;
      else if(!memcmp(value.contacts[i].publicKey,pin.publicKey,32))return -EEXIST;
    }
    if(existing==value.count){if(value.count==PkiPeerSet::Limit)return -ENOSPC;value.contacts[value.count++]=pin;}
    else value.contacts[existing]=pin;
    PkiPeerSet checked;if(!checked.assignTrusted(current,value.contacts,value.count))return -EINVAL;
    if(value.generation==UINT32_MAX)return -EOVERFLOW;
    if(before){const int rc=before(pin.node);if(rc)return rc;}
    phase_=Phase::Runtime;const int result=freshContacts_.replace(backend_,value.contacts,value.count);phase_=Phase::None;
    if(!context_.quiescent()){fail(State::Frozen,-EIO);return -EIO;}observeFailure();return state_==State::Active?result:-EIO;
  }
  if(!append_)return -ENOTSUP;

  PkiContactSnapshot::Value value;
  if(!contacts_.snapshot(value)||value.count>PkiPeerSet::Limit)return -EIO;
  // Block only immutable history tombstones. A still-active legacy lookup is
  // not a revocation after compact contacts take ownership of membership.
  if(append_->revoked(pin.node))return -EEXIST;

  size_t existing=value.count;bool changed=true;int retiredWindow=-1;
  for(size_t i=0;i<value.count;++i){
    if(value.contacts[i].node==pin.node)existing=i;
    else if(!memcmp(value.contacts[i].publicKey,pin.publicKey,32))return -EEXIST;
  }
  if(existing==value.count){
    if(value.count==PkiPeerSet::Limit)return -ENOSPC;
    value.contacts[value.count++]=pin;
  }else if(!memcmp(value.contacts[existing].publicKey,pin.publicKey,32)){
    // Re-applying the same owner pin is a no-op: do not advance the snapshot
    // generation or spend another NVS write.
    changed=false;
  }else {
    retiredWindow=windowIndex(value.contacts[existing].node,value.contacts[existing].publicKey,1);
    value.contacts[existing]=pin;
  }

  PkiPeerSet checked;if(!checked.assignTrusted(current,value.contacts,value.count))return -EINVAL;
  if(changed&&value.generation==UINT32_MAX)return -EOVERFLOW;
  if(before){const int rc=before(pin.node);if(rc)return rc;}
  int result=0;
  if(changed){
    phase_=Phase::Runtime;
    result=contacts_.replace(value.contacts,value.count);
    phase_=Phase::None;
  }
  if(!context_.quiescent()){fail(State::Frozen,-EIO);return -EIO;}
  observeFailure();
  if(state_==State::Active&&result==0&&retiredWindow>=0)
    (void)window_.unbind(unsigned(retiredWindow));
  return state_==State::Active?result:-EIO;
}
bool PkiStorage::boundTo(const uint8_t pub[32],uint32_t node)const{
  Lease hold(*this);if(!hold||!usable())return false;
  if(freshImplementation()){IdentityBinding b;KeyRef ref;return pub&&counter_.active()&&freshReady()&&
    identity_.binding(b,ref)&&freshRoot_.matches(b)&&b.node==node&&!memcmp(b.publicKey,pub,32);}
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  return pub&&extended_&&append_&&bounded_&&counter_.active()&&contacts_.ready()&&ledger_.boundTo(pub,node);
#else
  return replay_.boundTo(pub,node);
#endif
}
bool PkiStorage::lookup(uint32_t node,uint8_t pub[32])const{
  Lease hold(*this);if(!hold||!usable()||!pub)return false;
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  if(expandedActive())return contactPin(node,pub);
#endif
  if(freshImplementation()){PkiContactSnapshot::Value value;if(!freshContacts_.snapshot(value))return false;
    for(size_t i=0;i<value.count;++i)if(value.contacts[i].node==node){memcpy(pub,value.contacts[i].publicKey,32);return true;}return false;}
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  return bounded_&&counter_.active()&&contacts_.ready()&&contactPin(node,pub);
#else
  return contacts_.ready()?contactPin(node,pub):extended_?append_->lookup(node,pub):replay_.lookup(node,pub);
#endif
}
bool PkiStorage::active(uint32_t node,const uint8_t pub[32])const{
  Lease hold(*this);uint8_t current[32];if(!hold||!usable()||!pub)return false;
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  if(expandedActive())return contactPin(node,current)&&!memcmp(current,pub,32);
#endif
  if(freshImplementation()){PkiContactSnapshot::Value value;if(!freshContacts_.snapshot(value))return false;
    for(size_t i=0;i<value.count;++i)if(value.contacts[i].node==node)return !memcmp(value.contacts[i].publicKey,pub,32);return false;}
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  return bounded_&&counter_.active()&&contacts_.ready()&&contactPin(node,current)&&!memcmp(current,pub,32);
#else
  return contacts_.ready()?(contactPin(node,current)&&!memcmp(current,pub,32)):extended_?append_->active(node,pub):replay_.active(node,pub);
#endif
}
bool PkiStorage::revoked(uint32_t node)const{
  Lease hold(*this);if(!hold||!usable())return false;
  if(freshImplementation())return false;
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  return bounded_&&counter_.active()&&contacts_.ready()&&append_&&append_->revoked(node);
#else
  if(!contacts_.ready())return extended_?append_->revoked(node):replay_.revoked(node);
  if(append_&&append_->revoked(node))return true;
  if(contactPin(node,nullptr))return false;
  // Unknown public signed senders are not private revoked contacts. Preserve
  // tombstone knowledge from the immutable legacy history. A normal compact
  // removal only forgets the contact and does not create a permanent ban.
  return false;
#endif
}
ReplayCheck PkiStorage::seen(uint32_t node,const uint8_t pub[32],uint32_t id)const{
  Lease hold(*this);if(!hold||!usable())return ReplayCheck::NotReady;
  if(freshImplementation()){uint8_t trusted[32];if(!pub||!id)return ReplayCheck::Invalid;if(!contactPin(node,trusted)||memcmp(trusted,pub,32))return ReplayCheck::UnknownPeer;
    const int slot=windowIndex(node,pub,id);return slot>=0&&window_.contains(unsigned(slot),node,pub,id)==PkiReplayWindow::Result::Seen?
      ReplayCheck::Seen:ReplayCheck::Fresh;}
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  if(!bounded_||!counter_.active()||!contacts_.ready()||!append_)return ReplayCheck::NotReady;
#else
  if(!bounded_)return extended_?append_->seen(node,pub,id):replay_.seen(node,pub,id);
#endif
  const ReplayCheck legacy=boundedLegacy(node,pub,id);
  if(legacy!=ReplayCheck::Fresh)return legacy;
  const int slot=windowIndex(node,pub,id);
  return slot>=0&&window_.contains(unsigned(slot),node,pub,id)==PkiReplayWindow::Result::Seen?
    ReplayCheck::Seen:ReplayCheck::Fresh;
}
bool PkiStorage::commitAuthenticated(uint32_t node,const uint8_t pub[32],uint32_t id){
  Lease hold(*this);if(!hold||!usable()||!context_.quiescent())return false;
  if(freshImplementation()){uint8_t trusted[32];if(!pub||!id||!contactPin(node,trusted)||memcmp(trusted,pub,32))return false;int slot=windowIndex(node,pub,id);
    if(slot<0){slot=int(nextWindow_++%PkiReplayWindow::PeerLimit);(void)window_.unbind(unsigned(slot));
      IdentityBinding local;KeyRef ref;if(!identity_.binding(local,ref)||!window_.bindTrusted(unsigned(slot),node,pub,local.node))return false;}
    return window_.commitAuthenticated(unsigned(slot),node,pub,id)==PkiReplayWindow::Result::Fresh;}
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  if(!bounded_||!counter_.active()||!contacts_.ready()||!append_)return false;
#endif
  phase_=Phase::Runtime;bool ok=false;
  if(bounded_){
    if(boundedLegacy(node,pub,id)==ReplayCheck::Fresh){
      int slot=windowIndex(node,pub,id);
      if(slot<0){
        slot=int(nextWindow_++%PkiReplayWindow::PeerLimit);
        (void)window_.unbind(unsigned(slot));IdentityBinding local;KeyRef ref;
        bool bound=false;
        if(identity_.binding(local,ref)){
          if(contacts_.ready()){
            uint8_t trusted[32];
            if(pub&&contactPin(node,trusted)&&!memcmp(trusted,pub,32))
              bound=window_.bindTrusted(unsigned(slot),node,pub,local.node);
          }else bound=window_.bind(unsigned(slot),node,pub,local.node);
        }
        if(!bound)slot=-1;
      }
      ok=slot>=0&&window_.commitAuthenticated(unsigned(slot),node,pub,id)==PkiReplayWindow::Result::Fresh;
    }
  }
#if !defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  else ok=extended_?append_->commitAuthenticated(node,pub,id,context_.quiescent())==0:replay_.commitAuthenticated(node,pub,id);
#endif
  phase_=Phase::None;
  if(!context_.quiescent()){fail(State::Frozen,-EIO);return false;}
  observeFailure();return ok;
}
bool PkiStorage::revokeTrustedPeer(uint32_t node,const uint8_t pub[32]){
  Lease hold(*this);if(!hold||!usable()||!context_.quiescent())return false;
  if(contacts_.state()==PkiInplaceContacts::State::Pending)return false;
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  if(expandedActive()){
    phase_=Phase::Runtime;const int rc=expanded_->remove(backend_,node,pub);phase_=Phase::None;
    if(!expanded_->ready()||!context_.quiescent()){fail(State::Frozen,rc?rc:-EIO);return false;}
    return rc==0;
  }
#endif
  if(freshImplementation()){
    PkiContactSnapshot::Value value;uint8_t current[32];
    if(!pub||!contactPin(node,current)||memcmp(current,pub,32)||!freshContacts_.snapshot(value))return false;
    for(size_t i=0;i<value.count;++i){
      if(value.contacts[i].node!=node)continue;
      for(size_t j=i+1;j<value.count;++j)value.contacts[j-1]=value.contacts[j];
      --value.count;phase_=Phase::Runtime;
      const int rc=freshContacts_.replace(backend_,value.contacts,value.count);phase_=Phase::None;
      if(!context_.quiescent()){fail(State::Frozen,-EIO);return false;}
      observeFailure();return state_==State::Active&&rc==0;
    }
    return false;
  }
  phase_=Phase::Runtime;bool ok=false;
#if defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  if(contacts_.ready()){
    PkiContactSnapshot::Value value;uint8_t current[32];
    if(pub&&contactPin(node,current)&&!memcmp(current,pub,32)&&contacts_.snapshot(value)){
      for(size_t i=0;i<value.count;++i)if(value.contacts[i].node==node){
        for(size_t j=i+1;j<value.count;++j)value.contacts[j-1]=value.contacts[j];
        --value.count;ok=contacts_.replace(value.contacts,value.count)==0;break;
      }
    }
  }
#else
  if(contacts_.ready()){
    PkiContactSnapshot::Value value;uint8_t current[32];
    if(pub&&contactPin(node,current)&&!memcmp(current,pub,32)&&contacts_.snapshot(value)){
      for(size_t i=0;i<value.count;++i)if(value.contacts[i].node==node){
        for(size_t j=i+1;j<value.count;++j)value.contacts[j-1]=value.contacts[j];
        --value.count;ok=contacts_.replace(value.contacts,value.count)==0;break;
      }
    }
  }else ok=extended_?append_->revokeTrustedPeer(node,pub,context_.quiescent())==0:replay_.revokeTrustedPeer(node,pub);
#endif
  phase_=Phase::None;
  if(!context_.quiescent()){fail(State::Frozen,-EIO);return false;}
  observeFailure();return ok;
}
}
