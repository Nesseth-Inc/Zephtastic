// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../PkiContactStore.h"
#include "../PkiNonceCounter.h"
#include "../PkiProvisioning.h"
#include "../PkiFreshRoot.h"
#include "MtSignature.h"
#include <errno.h>
#include <string.h>

#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
#include "ImportedDomain.h"
#endif

namespace mt::pki {
template<size_t N>struct FreshSecret {uint8_t bytes[N]{};~FreshSecret(){eraseIdentityBytes(bytes,N);}};

// Write-ahead pending identity. Its separate key is never interpreted as an
// active identity by PkiStorage. Keeping this exact private candidate permits
// an owner-confirmed continuation after power loss without another RNG draw.
struct PkiFreshIntent {
  static constexpr size_t RecordBytes=84;
  static constexpr const char *Key="mtpki/fresh-pending";
  static uint32_t get(const uint8_t *p){return PkiFreshRoot::word(p);}
  static void put(uint8_t *p,uint32_t n){PkiFreshRoot::put(p,n);}
  static bool encode(const IdentityBinding &b,const uint8_t scalar[32],uint8_t out[RecordBytes]){
    if(!scalar||!out)return false;memset(out,0,RecordBytes);memcpy(out,"ZTPI",4);out[4]=1;out[5]=b.algorithm;
    put(out+8,b.generation);put(out+12,b.node);memcpy(out+16,scalar,32);memcpy(out+48,b.publicKey,32);
    put(out+80,PkiFreshRoot::checksum(out,80));return true;
  }
  static bool decode(const uint8_t *in,size_t n,IdentityBinding &b,uint8_t scalar[32],IdentityDeriver &deriver){
    if(!in||n!=RecordBytes||memcmp(in,"ZTPI",4)||in[4]!=1||in[5]!=1||in[6]||in[7]||
       get(in+80)!=PkiFreshRoot::checksum(in,80)||get(in+8)!=1||get(in+12)<=1||get(in+12)>=0xfffffffeu)return false;
    uint8_t candidateScalar[32];memcpy(candidateScalar,in+16,32);
    if((candidateScalar[0]&7)||(candidateScalar[31]&0xc0)!=0x40){eraseIdentityBytes(candidateScalar,sizeof(candidateScalar));return false;}
    b.generation=get(in+8);b.node=get(in+12);b.algorithm=1;b.version=1;memcpy(b.publicKey,in+48,32);
    uint8_t derived[32]{},ed[32]{};const bool valid=deriver.derivePublic(candidateScalar,derived)&&
      !memcmp(derived,b.publicKey,32)&&bench::MtSignature::nodeId(b.publicKey)==b.node&&
      bench::MtSignature::admissiblePublicKey(b.publicKey,ed);
    eraseIdentityBytes(derived,sizeof(derived));eraseIdentityBytes(ed,sizeof(ed));
    if(!valid){eraseIdentityBytes(candidateScalar,sizeof(candidateScalar));return false;}
    memcpy(scalar,candidateScalar,32);eraseIdentityBytes(candidateScalar,sizeof(candidateScalar));return true;
  }
};

// The explicit setup writer must resolve to the same already-mounted NVS
// namespace as BootSettings. persist() is permitted only for the fixed fresh
// domain records used below; implementations must not mount or repair NVS.
struct FreshProvisioningSettings:ProvisioningSettings {};

class PkiFreshProvisioning {
public:
  enum class State:uint8_t {New,Staged,Committing,Committed,Rejected,Expired,Cancelled,Frozen,Busy};
  static constexpr uint64_t StageLifetimeMs=120000;
  static constexpr const char *RootKey="mtpki/fresh0";
private:
  FreshProvisioningSettings &settings_;ProvisioningContext &context_;EntropySource &entropy_;
  MonocypherIdentityDeriver deriver_;uint8_t scalar_[32]{};IdentityBinding staged_{};
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  bool imported_=false;uint8_t importSalt_[ImportedDomain::SaltSize]{};
  int savedPrefix_=-1;
#endif
  uint64_t started_=0;mutable bool busy_=false;bool attemptedWrite_=false;
  State state_=State::New;int error_=0;
  class Lease {const PkiFreshProvisioning &o_;bool held_=false;public:
    explicit Lease(const PkiFreshProvisioning &o):o_(o){if(!o_.context_.tryEnter())return;
      if(o_.busy_){o_.context_.leave();return;}o_.busy_=true;held_=true;}
    ~Lease(){if(held_){o_.busy_=false;o_.context_.leave();}}explicit operator bool()const{return held_;}};
  class Backend final:public SettingsBackend {
    PkiFreshProvisioning &o_;unsigned writeStep_=0;
  public:explicit Backend(PkiFreshProvisioning &o):o_(o){}
    int read(const char *key,uint8_t *out,size_t n)override{
      if(!key||!out||!o_.busy_||!o_.context_.quiescent())return -EACCES;
      const int rc=o_.settings_.begin();if(rc)return rc;const int got=o_.settings_.read(key,out,n);o_.settings_.end();
      return o_.context_.quiescent()?got:-EACCES;
    }
    int write(const char *key,const uint8_t *data,size_t n)override{
      const char *expected=nullptr;size_t length=0;
      switch(writeStep_){case 1:expected="mtpki/nonces0";length=PkiNonceCounter::RecordBytes;break;
        case 2:expected="mtpki/nonces1";length=PkiNonceCounter::RecordBytes;break;
        case 3:expected="mtpki/contacts0";length=PkiContactStore::RecordBytes;break;
        case 4:expected="mtpki/contacts1";length=PkiContactStore::RecordBytes;break;
        case 5:expected=PkiFreshProvisioning::RootKey;length=PkiFreshRoot::RecordBytes;break;
        case 6:expected="mtidentity/state0";length=IdentityJournal::RecordSize;break;default:break;}
      if(!o_.busy_||!expected||!key||strcmp(key,expected)||!data||n!=length||!o_.context_.quiescent())return -EACCES;
      o_.attemptedWrite_=true;const int rc=o_.settings_.persist(key,data,n);
      if(writeStep_==3)writeStep_=4;else if(writeStep_==4)writeStep_=0;
      if(rc)return rc<0?rc:-EIO;return o_.context_.quiescent()?0:-EACCES;
    }
    void step(unsigned n){writeStep_=n;}
  } backend_;
  class CounterStore final:public PkiNonceCounterStore {
    Backend &b_;public:explicit CounterStore(Backend &b):b_(b){}
    int read(unsigned slot,uint8_t *out,size_t n)override{return slot<2?b_.read(slot?"mtpki/nonces1":"mtpki/nonces0",out,n):-EINVAL;}
    int write(unsigned slot,const uint8_t *data,size_t n)override{b_.step(slot+1);return slot<2?b_.write(slot?"mtpki/nonces1":"mtpki/nonces0",data,n):-EINVAL;}
  } counterStore_;
  bool exact(const IdentityBinding &b)const{return b.version==staged_.version&&b.algorithm==staged_.algorithm&&
    b.generation==staged_.generation&&b.node==staged_.node&&!memcmp(b.publicKey,staged_.publicKey,32);}
  int refuse(int rc){eraseIdentityBytes(scalar_,sizeof(scalar_));staged_=IdentityBinding{};
    state_=attemptedWrite_?State::Frozen:(rc==-ETIMEDOUT?State::Expired:State::Rejected);error_=rc;return rc;}
  int gate(){if((state_==State::Staged||state_==State::Committing)&&context_.millis()-started_>=StageLifetimeMs)return -ETIMEDOUT;
    return context_.quiescent()?0:-EACCES;}
  int inspectSlots(bool allowIntent){
    const char *keys[]={"mtidentity/state0","mtidentity/state1","mtpki/ledger0","mtpki/ledger1",
      "mtpki/nonces0","mtpki/nonces1","mtpki/contacts0","mtpki/contacts1",RootKey,PkiFreshIntent::Key};
    uint8_t buf[PkiLedger::RecordSize]{};
    for(const char *key:keys){size_t cap=!strcmp(key,PkiFreshIntent::Key)?PkiFreshIntent::RecordBytes:
      !strncmp(key,"mtidentity",10)?IdentityJournal::RecordSize:
      !strncmp(key,"mtpki/ledger",11)?PkiLedger::RecordSize:
      !strncmp(key,"mtpki/nonces",12)?PkiNonceCounter::RecordBytes:
      !strncmp(key,"mtpki/contacts",14)?PkiContactStore::RecordBytes:PkiFreshRoot::RecordBytes;
      int rc=settings_.begin();if(rc)return rc;const int n=settings_.read(key,buf,cap);settings_.end();
      if(!strcmp(key,PkiFreshIntent::Key)&&allowIntent&&n==int(cap)){eraseIdentityBytes(buf,sizeof(buf));continue;}
      eraseIdentityBytes(buf,sizeof(buf));if(n!=-ENOENT)return n<0?n:-EEXIST;if((rc=gate()))return rc;
    }
    return 0;
  }
  int readKey(const char *key,uint8_t *out,size_t cap){int rc=gate();if(rc)return rc;rc=settings_.begin();if(rc)return rc;
    const int n=settings_.read(key,out,cap);settings_.end();const int after=gate();
    if(after){if(out)eraseIdentityBytes(out,cap);return after;}return n;}
  int emptyIdentityAndLegacy(){uint8_t bytes[PkiLedger::RecordSize]{};const char *keys[]={
    "mtidentity/state0","mtidentity/state1","mtpki/ledger0","mtpki/ledger1"};
    for(const char *key:keys){const size_t cap=!strncmp(key,"mtidentity",10)?IdentityJournal::RecordSize:PkiLedger::RecordSize;
      const int n=readKey(key,bytes,cap);eraseIdentityBytes(bytes,sizeof(bytes));if(n!=-ENOENT)return n<0?n:-EEXIST;}return gate();}
  int ensureContacts(const PkiFreshRoot &root, bool writeMissing) {
    uint8_t expected[PkiContactStore::RecordBytes]{}, readback[PkiContactStore::RecordBytes]{};
    auto inspect = [&](unsigned slot) {
      if (!PkiContactSnapshot::encode(staged_, slot + 1, root.generation, root.crc,
                                      nullptr, 0, expected, sizeof(expected))) return -EINVAL;
      const int n = readKey(PkiContactStore::key(slot), readback, sizeof(readback));
      if (n < 0) return n;
      return n == int(sizeof(readback)) && !memcmp(readback, expected, sizeof(expected)) ? 0 : -EILSEQ;
    };

    for (unsigned slot = 0; slot < 2; ++slot) {
      const int rc = inspect(slot);
      if (rc && rc != -ENOENT) return rc;
    }
    if (!writeMissing) return 0;

    for (unsigned slot = 0; slot < 2; ++slot) {
      int rc = inspect(slot);
      if (!rc) continue;
      if (rc != -ENOENT) return rc;
      backend_.step(slot + 3);
      rc = backend_.write(PkiContactStore::key(slot), expected, sizeof(expected));
      if (rc) return rc;
      rc = inspect(slot);
      if (rc) return rc;
    }
    // A successful write of the other row is not proof that both survived.
    // Check both complete snapshots before the caller publishes the selector.
    for (unsigned slot = 0; slot < 2; ++slot) {
      const int rc = inspect(slot);
      if (rc) return rc;
    }
    return 0;
  }
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  int savedImportPrefix() {
    // A missing predecessor followed by a present row is not a power cut
    // during the forward-only genesis transaction. Check before issuing consent.
    struct Row { const char *key; size_t bytes; };
    const Row ordered[] = {{"mtpki/nonces0", PkiNonceCounter::RecordBytes},
                           {"mtpki/nonces1", PkiNonceCounter::RecordBytes},
                           {"mtpki/contacts0", PkiContactStore::RecordBytes},
                           {"mtpki/contacts1", PkiContactStore::RecordBytes},
                           {RootKey, PkiFreshRoot::RecordBytes}};
    uint8_t probe[PkiContactStore::RecordBytes]{};
    int present = 0;
    bool missing = false;
    for (const Row &row : ordered) {
      const int count = readKey(row.key, probe, row.bytes);
      if (count == -ENOENT) missing = true;
      else if (count != int(row.bytes) || missing) return count < 0 ? count : -EILSEQ;
      else ++present;
    }

    return present;
  }
#endif
public:
  PkiFreshProvisioning(FreshProvisioningSettings &s,ProvisioningContext &c,EntropySource &e):
    settings_(s),context_(c),entropy_(e),backend_(*this),counterStore_(backend_){}
  PkiFreshProvisioning(const PkiFreshProvisioning&)=delete;PkiFreshProvisioning&operator=(const PkiFreshProvisioning&)=delete;
  ~PkiFreshProvisioning(){eraseIdentityBytes(scalar_,sizeof(scalar_));}
  State state()const{Lease l(*this);return l?state_:State::Busy;}
  int lastError()const{Lease l(*this);return l?error_:-EBUSY;}
  int stage(IdentityBinding &out){
    Lease l(*this);if(!l)return -EBUSY;if(state_!=State::New&&state_!=State::Staged)return -EACCES;
    int rc=gate();if(rc)return refuse(rc);if(state_==State::Staged){
      FreshSecret<PkiFreshIntent::RecordBytes> intent;rc=readKey(PkiFreshIntent::Key,intent.bytes,sizeof(intent.bytes));
      if(rc==int(sizeof(intent.bytes))){IdentityBinding pending{};FreshSecret<32> scalar;
        const bool valid=PkiFreshIntent::decode(intent.bytes,sizeof(intent.bytes),pending,scalar.bytes,deriver_)&&exact(pending)&&!memcmp(scalar.bytes,scalar_,32);
        if(!valid)return refuse(-EILSEQ);
        if((rc=emptyIdentityAndLegacy()))return refuse(rc);
      }else if(rc==-ENOENT){if((rc=inspectSlots(false)))return refuse(rc);}
      else return refuse(rc<0?rc:-EILSEQ);out=staged_;return 0;}
    FreshSecret<PkiFreshIntent::RecordBytes> intent;rc=readKey(PkiFreshIntent::Key,intent.bytes,sizeof(intent.bytes));
    if(rc==int(sizeof(intent.bytes))){if(!PkiFreshIntent::decode(intent.bytes,sizeof(intent.bytes),staged_,scalar_,deriver_))return refuse(-EILSEQ);
      if((rc=emptyIdentityAndLegacy()))return refuse(rc);state_=State::Staged;started_=context_.millis();
      if(!context_.candidateAllowed(staged_))return refuse(-EEXIST);out=staged_;return 0;}
    if(rc!=-ENOENT)return refuse(rc<0?rc:-EILSEQ);
    if((rc=inspectSlots(false)))return refuse(rc);
    state_=State::Staged;started_=context_.millis();
    if(!entropy_.fill(scalar_,sizeof(scalar_)))return refuse(-ENODATA);if((rc=gate()))return refuse(rc);
    scalar_[0]&=248;scalar_[31]=(scalar_[31]&127)|64;staged_.generation=1;
    if(!deriver_.derivePublic(scalar_,staged_.publicKey))return refuse(-EIO);
    staged_.node=bench::MtSignature::nodeId(staged_.publicKey);
    uint8_t ed[32]{};const bool publicValid=bench::MtSignature::admissiblePublicKey(staged_.publicKey,ed);
    eraseIdentityBytes(ed,sizeof(ed));
    if(staged_.node<=1||staged_.node>=0xfffffffeu||!publicValid||!context_.candidateAllowed(staged_))return refuse(-EEXIST);
    if((rc=gate()))return refuse(rc);out=staged_;return 0;
  }
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  bool clearImportSession() {
    Lease hold(*this);
    if (!hold || attemptedWrite_ || state_ == State::Committed || state_ == State::Committing)
      return false;
    eraseIdentityBytes(scalar_, sizeof(scalar_));
    eraseIdentityBytes(importSalt_, sizeof(importSalt_));
    staged_ = {};
    started_ = 0;
    imported_ = false;
    savedPrefix_ = -1;
    state_ = State::New;
    error_ = 0;
    return true;
  }

  // Recover only the ordered prefix written by our own previously confirmed
  // import. An active identity, a later counter reservation or a trust edit
  // belongs to identity-journal repair, never first-install continuation.
  int stageSavedImport(IdentityBinding &out) {
    Lease hold(*this);
    if (!hold) return -EBUSY;
    if (state_ != State::New) return -EACCES;
    int rc = emptyIdentityAndLegacy();
    if (rc) return refuse(rc);

    FreshSecret<PkiFreshIntent::RecordBytes> intent;
    rc = readKey(PkiFreshIntent::Key, intent.bytes, sizeof(intent.bytes));
    if (rc != int(sizeof(intent.bytes))) return refuse(rc < 0 ? rc : -EILSEQ);
    if (memcmp(intent.bytes, "ZTP2", 4)) return refuse(-EILSEQ);
    uint8_t pub[32]{};
    if (!deriver_.derivePublic(intent.bytes + 16, pub) ||
        !ImportedDomain::binding(intent.bytes + 16, pub, staged_) ||
        !ImportedDomain::readIntent(intent.bytes, staged_, importSalt_) ||
        !context_.candidateAllowed(staged_)) return refuse(-EXDEV);

    savedPrefix_ = savedImportPrefix();
    if (savedPrefix_ < 0) return refuse(savedPrefix_);
    uint8_t probe[PkiFreshRoot::RecordBytes]{};
    uint8_t expected[PkiFreshRoot::RecordBytes]{};
    PkiFreshRoot root;
    if (!ImportedDomain::root(staged_, importSalt_, expected, root)) return refuse(-EILSEQ);
    const int n = readKey(RootKey, probe, sizeof(expected));
    if (n != -ENOENT && (n != int(sizeof(expected)) || memcmp(probe, expected, sizeof(expected))))
      return refuse(n < 0 ? n : -EXDEV);

    PkiNonceCounterBinding counterBinding{};
    counterBinding.node = staged_.node;
    counterBinding.legacyGeneration = root.generation;
    counterBinding.legacyCrc = root.crc;
    FreshSecret<32> permutation;
    ImportedDomain::keys(staged_, intent.bytes + 16, importSalt_, counterBinding, permutation.bytes);
    PkiNonceCounter counter(counterStore_);
    if ((rc = counter.preflightGenesis(counterBinding, true)) || (rc = ensureContacts(root, false)) ||
        (rc = gate())) return refuse(rc);

    memcpy(scalar_, intent.bytes + 16, sizeof(scalar_));
    imported_ = true;
    started_ = context_.millis();
    state_ = State::Staged;
    out = staged_;
    return 0;
  }

  // An authorized candidate stays in RAM. After an interrupted commit only
  // the original scalar/public pair may resume the saved nonce namespace.
  int stageImported(const uint8_t supplied[32], const uint8_t pub[32], IdentityBinding &out) {
    Lease hold(*this);
    if (!hold) return -EBUSY;
    if (state_ != State::New) return -EACCES;
    int rc = gate();
    if (rc) return refuse(rc);
    if (!supplied || !pub) return refuse(-EINVAL);

    // Canonicalize the five X25519/XEdDSA scalar bits and require an exact
    // match to the public key supplied by stock. Reject effectively zero input.
    FreshSecret<32> normalized;
    memcpy(normalized.bytes, supplied, 32);
    normalized.bytes[0] &= 248;
    normalized.bytes[31] &= 63;
    uint8_t any = 0;
    for (auto byte : normalized.bytes) any |= byte;
    if (!any) return refuse(-EINVAL);
    normalized.bytes[31] |= 64;
    if (!ImportedDomain::binding(normalized.bytes, pub, staged_) || !context_.candidateAllowed(staged_))
      return refuse(-EINVAL);

    FreshSecret<PkiFreshIntent::RecordBytes> prior;
    rc = readKey(PkiFreshIntent::Key, prior.bytes, sizeof(prior.bytes));
    if (rc == -ENOENT) {
      rc = inspectSlots(false);
      if (rc) return refuse(rc);
      if (!entropy_.fill(importSalt_, sizeof(importSalt_)) || !ImportedDomain::hasSalt(importSalt_))
        return refuse(-EIO);
    } else {
      if (rc != int(sizeof(prior.bytes))) return refuse(rc < 0 ? rc : -EILSEQ);
      if (!ImportedDomain::readIntent(prior.bytes, staged_, importSalt_) ||
          crypto_verify32(prior.bytes + 16, normalized.bytes) != 0) return refuse(-EEXIST);
      rc = emptyIdentityAndLegacy();
      if (rc) return refuse(rc);
    }
    rc = gate();
    if (rc) return refuse(rc);
    memcpy(scalar_, normalized.bytes, 32);
    imported_ = true;
    state_ = State::Staged;
    started_ = context_.millis();
    out = staged_;
    return 0;
  }
#endif
  int confirm(const IdentityBinding &expected){
    Lease l(*this);if(!l)return -EBUSY;if(state_!=State::Staged)return -EACCES;
    int rc=gate();if(rc)return refuse(rc);if(!exact(expected))return refuse(-EINVAL);
    if((rc=emptyIdentityAndLegacy()))return refuse(rc);if(!context_.candidateAllowed(staged_))return refuse(-EEXIST);
    uint8_t rootBytes[PkiFreshRoot::RecordBytes]{};PkiFreshRoot root;
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    if(imported_){
      if(savedPrefix_>=0 && savedImportPrefix()!=savedPrefix_)return refuse(-ESTALE);
      if(!ImportedDomain::root(staged_,importSalt_,rootBytes,root))return refuse(-EINVAL);
    }
    else
#endif
    if(!PkiFreshRoot::encode(staged_,rootBytes,&root))return refuse(-EINVAL);
    PkiNonceCounter counter(counterStore_);PkiNonceCounterBinding binding{};binding.node=staged_.node;
    memcpy(binding.publicKey,staged_.publicKey,32);binding.legacyGeneration=root.generation;binding.legacyCrc=root.crc;
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    if(imported_){FreshSecret<32> key;ImportedDomain::keys(staged_,scalar_,importSalt_,binding,key.bytes);}
#endif
    uint8_t existingRoot[PkiFreshRoot::RecordBytes]{};rc=readKey(RootKey,existingRoot,sizeof(existingRoot));
    const bool rootAbsent=rc==-ENOENT;
    if(rc!=-ENOENT&&(rc!=int(sizeof(existingRoot))||memcmp(existingRoot,rootBytes,sizeof(rootBytes))))return refuse(rc<0?rc:-EILSEQ);
    if((rc=counter.preflightGenesis(binding,true)))return refuse(rc);
    if((rc=ensureContacts(root,false)))return refuse(rc);
    FreshSecret<PkiFreshIntent::RecordBytes> intent,intentRead;
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    if(imported_)ImportedDomain::intent(staged_,scalar_,importSalt_,intent.bytes);
    else
#endif
    if(!PkiFreshIntent::encode(staged_,scalar_,intent.bytes))return refuse(-EINVAL);
    rc=readKey(PkiFreshIntent::Key,intentRead.bytes,sizeof(intentRead.bytes));
    if(rc==-ENOENT){
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
      if(savedPrefix_>=0)return refuse(-ESTALE);
#endif
      if((rc=gate()))return refuse(rc);attemptedWrite_=true;
      if((rc=settings_.persist(PkiFreshIntent::Key,intent.bytes,sizeof(intent.bytes))))return refuse(rc<0?rc:-EIO);
      rc=readKey(PkiFreshIntent::Key,intentRead.bytes,sizeof(intentRead.bytes));
    }
    if(rc!=int(sizeof(intentRead.bytes))||memcmp(intent.bytes,intentRead.bytes,sizeof(intent.bytes)))return refuse(rc<0?rc:-EILSEQ);
    state_=State::Committing;
    if((rc=counter.initialize(binding,true)))return refuse(rc);
    if((rc=ensureContacts(root,true)))return refuse(rc);
    if(rootAbsent){backend_.step(5);if((rc=backend_.write(RootKey,rootBytes,sizeof(rootBytes))))return refuse(rc);}
    uint8_t verify[PkiFreshRoot::RecordBytes]{};const int n=readKey(RootKey,verify,sizeof(verify));
    if(n!=int(sizeof(verify))||memcmp(verify,rootBytes,sizeof(verify)))return refuse(n<0?n:-EIO);
    backend_.step(6);IdentityJournal identity;IdentityCandidate candidate(staged_,scalar_);
    if((rc=identity.load(backend_,deriver_))||identity.state()!=IdentityJournal::State::Empty)return refuse(rc?rc:-EEXIST);
    if((rc=identity.store(backend_,deriver_,candidate,true)))return refuse(rc);
    IdentityBinding stored;KeyRef ref;if(!identity.binding(stored,ref)||!exact(stored)||!identity.active(ref))return refuse(-EIO);
    if((rc=gate()))return refuse(rc);eraseIdentityBytes(scalar_,sizeof(scalar_));staged_=IdentityBinding{};
    state_=State::Committed;error_=0;return 0;
  }
  int tick(){Lease l(*this);if(!l)return -EBUSY;if(state_!=State::Staged)return 0;const int rc=gate();return rc?refuse(rc):0;}
  bool cancel(){Lease l(*this);if(!l||(state_!=State::New&&state_!=State::Staged))return false;
    eraseIdentityBytes(scalar_,sizeof(scalar_));staged_=IdentityBinding{};state_=State::Cancelled;return true;}
};
}
