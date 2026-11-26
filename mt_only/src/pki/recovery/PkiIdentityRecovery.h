// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../PkiStorage.h"
#include "../PkiContactSnapshot.h"
#include "../PkiFreshRoot.h"
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
#include "../provisioning/ImportedDomain.h"
#endif
#include "MtSignature.h"
#include <errno.h>
#include <string.h>

namespace mt::pki {

struct IdentityRecoveryGate {
  virtual ~IdentityRecoveryGate()=default;
  virtual bool identityRecoveryQuiescent()const=0;
};

// Read-only overlay used only by the explicit recovery operation. It presents
// canonical candidate identity records to PkiStorage::prepare while every
// ledger, nonce, append, and contact read still comes from the mounted store.
class IdentityRecoveryBootOverlay final:public BootSettings {
  BootSettings &base_;
  uint8_t records_[2][IdentityJournal::RecordSize]{};
  bool presentInOverlay_[2]{};
  bool physicallyAbsent_[2]{};
  bool observed_[2]{};
  bool active_=false;

  static int identitySlot(const char *key) {
    if(key&&!strcmp(key,"mtidentity/state0"))return 0;
    if(key&&!strcmp(key,"mtidentity/state1"))return 1;
    return -1;
  }
  static uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
  }
  static uint32_t crc32(const uint8_t *p,size_t n) {
    uint32_t crc=UINT32_MAX;
    for(size_t i=0;i<n;++i){crc^=p[i];for(unsigned bit=0;bit<8;++bit)
      crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1u)));}
    return ~crc;
  }
  static void encode(const IdentityBinding &binding,const uint8_t scalar[32],uint8_t out[IdentityJournal::RecordSize]) {
    memset(out,0,IdentityJournal::RecordSize);memcpy(out,"ZTID",4);
    out[4]=binding.version;out[5]=binding.algorithm;out[6]=uint8_t(IdentityJournal::RecordSize);
    for(unsigned i=0;i<4;++i){out[8+i]=uint8_t(binding.generation>>(8*i));out[12+i]=uint8_t(binding.node>>(8*i));}
    memcpy(out+16,scalar,32);memcpy(out+48,binding.publicKey,32);
    const uint32_t crc=crc32(out,80);for(unsigned i=0;i<4;++i)out[80+i]=uint8_t(crc>>(8*i));
  }
public:
  explicit IdentityRecoveryBootOverlay(BootSettings &base):base_(base){}
  IdentityRecoveryBootOverlay(const IdentityRecoveryBootOverlay &)=delete;
  IdentityRecoveryBootOverlay &operator=(const IdentityRecoveryBootOverlay &)=delete;
  ~IdentityRecoveryBootOverlay(){clear();}
  void clear(){eraseIdentityBytes(records_,sizeof(records_));memset(presentInOverlay_,0,sizeof(presentInOverlay_));}

  bool configure(const IdentityBinding &binding,const uint8_t scalar[32]) {
    eraseIdentityBytes(records_,sizeof(records_));memset(presentInOverlay_,0,sizeof(presentInOverlay_));
    memset(physicallyAbsent_,0,sizeof(physicallyAbsent_));memset(observed_,0,sizeof(observed_));
    if(!scalar||binding.generation!=1)return false;
    presentInOverlay_[0]=true;encode(binding,scalar,records_[0]);
    return true;
  }
  int begin()override {
    if(active_)return -EBUSY;
    memset(observed_,0,sizeof(observed_));memset(physicallyAbsent_,0,sizeof(physicallyAbsent_));
    const int rc=base_.begin();if(!rc)active_=true;return rc;
  }
  void end()override {if(active_)base_.end();active_=false;}
  int read(const char *key,uint8_t *out,size_t capacity)override {
    if(!active_||!key||!out)return -EACCES;
    const int slot=identitySlot(key);
    if(slot<0)return base_.read(key,out,capacity);
    if(capacity!=IdentityJournal::RecordSize)return -EINVAL;
    uint8_t original[IdentityJournal::RecordSize]{};
    const int n=base_.read(key,original,sizeof(original));eraseIdentityBytes(original,sizeof(original));
    observed_[slot]=true;physicallyAbsent_[slot]=n==-ENOENT;
    // Preserve non-data backend failures. Corrupt/oversized identity values
    // are replaced only in this RAM proof view and will block the write phase.
    if(n<0&&n!=-ENOENT&&n!=-EILSEQ&&n!=-E2BIG)return n;
    if(!presentInOverlay_[slot])return -ENOENT;
    memcpy(out,records_[slot],capacity);return int(capacity);
  }
  bool bothIdentitySlotsAbsent()const {
    return observed_[0]&&observed_[1]&&physicallyAbsent_[0]&&physicallyAbsent_[1];
  }
  const uint8_t *record(unsigned slot)const{return slot<2&&presentInOverlay_[slot]?records_[slot]:nullptr;}

  // The active compact-contact snapshot is the durable identity-generation
  // witness. PkiStorage::prepare later validates its CRC, identity/root binding,
  // adjacent slot state, replay, and nonce authority against the candidate.
  bool inferGeneration(uint32_t node,const uint8_t publicKey[32],uint32_t &generation) {
    if(!node)return false;
    const int begin=base_.begin();if(begin)return false;
    uint8_t freshBytes[PkiFreshRoot::RecordBytes]{};
    const int freshRead=base_.read("mtpki/fresh0",freshBytes,sizeof(freshBytes));
    if(freshRead!=-ENOENT) {
      PkiFreshRoot root;
      bool valid=freshRead==int(sizeof(freshBytes))&&PkiFreshRoot::decode(freshBytes,sizeof(freshBytes),root)&&
        root.node==node&&!memcmp(root.publicKey,publicKey,32);
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
      if(freshRead==int(sizeof(freshBytes))&&!memcmp(freshBytes,"ZTI1",4)){
        IdentityBinding owner{};
        owner.generation=1;
        owner.node=node;
        memcpy(owner.publicKey,publicKey,sizeof(owner.publicKey));
        valid=ImportedDomain::matchesRoot(freshBytes,sizeof(freshBytes),owner,root);
      }
#endif
      eraseIdentityBytes(freshBytes,sizeof(freshBytes));base_.end();
      if(valid)generation=root.identityGeneration;
      return valid; // A positive fresh selector never falls back to legacy.
    }
    eraseIdentityBytes(freshBytes,sizeof(freshBytes));
    bool found=false;uint32_t candidate=0;bool ok=true;
    for(unsigned slot=0;slot<2&&ok;++slot) {
      uint8_t raw[PkiLedger::RecordSize]{};
      const int n=base_.read(slot?"mtpki/ledger1":"mtpki/ledger0",raw,sizeof(raw));
      if(n==-ENOENT)continue;
      if(n<0){ok=false;eraseIdentityBytes(raw,sizeof(raw));break;}
      if(n==int(PkiContactSnapshot::RecordBytes)&&get32(raw)==PkiContactSnapshot::Magic&&
         uint32_t(raw[4]|(uint16_t(raw[5])<<8))==PkiContactSnapshot::Version&&get32(raw+12)==node&&
         !memcmp(raw+16,publicKey,32)) {
        const uint32_t value=get32(raw+48);
        if(!value||(found&&candidate!=value))ok=false;
        else {candidate=value;found=true;}
      }
      eraseIdentityBytes(raw,sizeof(raw));
    }
    base_.end();
    if(!ok||!found)return false;
    generation=candidate;return true;
  }
};

// Explicit existing-identity recovery. The proof PkiStorage is separate from
// the live startup owner and is never activated. A failed/uncertain identity
// write is terminal for this helper instance; it never retries or repairs.
class PkiIdentityRecovery {
  SettingsBackend &runtime_;PkiStorageContext &context_;IdentityRecoveryGate &gate_;
  PkiStorage &live_;IdentityRecoveryBootOverlay &overlay_;PkiStorage &proof_;
  bool attempted_=false,frozen_=false;
  static const char *key(unsigned slot){return slot?"mtidentity/state1":"mtidentity/state0";}
  class Lease {
    PkiStorageContext &context_;bool held_=false;
  public:
    explicit Lease(PkiStorageContext &context):context_(context),held_(context_.tryEnter()){}
    ~Lease(){if(held_)context_.leave();}
    explicit operator bool()const{return held_;}
  };
public:
  PkiIdentityRecovery(SettingsBackend &runtime,PkiStorageContext &context,IdentityRecoveryGate &gate,
                      PkiStorage &live,IdentityRecoveryBootOverlay &overlay,PkiStorage &proof):
    runtime_(runtime),context_(context),gate_(gate),live_(live),overlay_(overlay),proof_(proof){}
  PkiIdentityRecovery(const PkiIdentityRecovery &)=delete;
  PkiIdentityRecovery &operator=(const PkiIdentityRecovery &)=delete;

  // Input is exactly the existing canonical 32-byte scalar. It is neither
  // normalized nor exported. Success persists identity records only; caller
  // must reboot/re-prepare before constructing or activating a runtime.
  int restore(const uint8_t suppliedScalar[32]) {
    if(frozen_||attempted_)return -EACCES;
    if(!suppliedScalar)return -EINVAL;
    uint8_t scalar[32]{};memcpy(scalar,suppliedScalar,sizeof(scalar));
    struct Wipe {uint8_t *p;~Wipe(){eraseIdentityBytes(p,32);}} wipe{scalar};
    if((scalar[0]&7u)||(scalar[31]&0xc0u)!=0x40u)return -EINVAL;
    MonocypherIdentityDeriver deriver;uint8_t publicKey[32]{},admissibleScratch[32]{};
    struct PubWipe {uint8_t *p;~PubWipe(){eraseIdentityBytes(p,32);}} pubWipe{publicKey};
    struct AdmissibleWipe {uint8_t *p;~AdmissibleWipe(){eraseIdentityBytes(p,32);}} admissibleWipe{admissibleScratch};
    if(!deriver.derivePublic(scalar,publicKey)||!bench::MtSignature::admissiblePublicKey(publicKey,admissibleScratch))return -EINVAL;
    const uint32_t node=bench::MtSignature::nodeId(publicKey);
    if(node<=1||node>=0xfffffffeu)return -EINVAL;

    Lease hold(context_);if(!hold)return -EBUSY;
    if(!gate_.identityRecoveryQuiescent())return -EACCES;
    const auto liveState=live_.state();
    if(liveState==PkiStorage::State::Busy)return -EBUSY;
    if(liveState==PkiStorage::State::Active) {
      IdentityBinding current;
      if(live_.preparedBinding(current)&&current.node==node&&!memcmp(current.publicKey,publicKey,32))return 0;
      return -EEXIST;
    }
    if(liveState==PkiStorage::State::Prepared) {
      IdentityBinding current;
      if(live_.preparedBinding(current)&&current.node==node&&!memcmp(current.publicKey,publicKey,32))return 0;
      return -EEXIST;
    }

    uint32_t generation=0;
    if(!overlay_.inferGeneration(node,publicKey,generation))return -ENODATA;
    // Multi-slot reconstruction cannot be resumed safely after loss of power
    // between two writes without a durable continuation witness. Support only
    // the atomic first-generation identity record in this implementation.
    if(generation!=1)return -ENOTSUP;
    IdentityBinding candidate{};candidate.version=1;candidate.algorithm=1;candidate.generation=generation;
    candidate.node=node;memcpy(candidate.publicKey,publicKey,32);
    if(!overlay_.configure(candidate,scalar)){eraseIdentityBytes(&candidate,sizeof(candidate));return -EINVAL;}
    struct OverlayWipe {IdentityRecoveryBootOverlay &overlay;~OverlayWipe(){overlay.clear();}} overlayWipe{overlay_};
    attempted_=true;

    // This is the sole readiness proof: real PkiStorage parsing of a temporary
    // canonical identity over immutable current ledger/counter/history/contact
    // reads. prepare() performs no writes and does not activate.
    const int proofResult=proof_.prepare();
    IdentityBinding proven{};
    if(proofResult||proof_.state()!=PkiStorage::State::Prepared||
       !proof_.preparedBinding(proven)||proven.version!=candidate.version||
       proven.algorithm!=candidate.algorithm||proven.generation!=candidate.generation||
       proven.node!=candidate.node||memcmp(proven.publicKey,candidate.publicKey,32)||
       !overlay_.bothIdentitySlotsAbsent()) {
      (void)proof_.close();eraseIdentityBytes(&proven,sizeof(proven));eraseIdentityBytes(&candidate,sizeof(candidate));
      return proofResult?proofResult:-EEXIST;
    }
    eraseIdentityBytes(&proven,sizeof(proven));
    // Drop the verified temporary scalar before touching durable identity.
    if(!proof_.close()){eraseIdentityBytes(&candidate,sizeof(candidate));return -EIO;}
    if(!gate_.identityRecoveryQuiescent()){eraseIdentityBytes(&candidate,sizeof(candidate));return -EACCES;}

    for(unsigned slot=0;slot<1;++slot) {
      const uint8_t *record=overlay_.record(slot);uint8_t readback[IdentityJournal::RecordSize]{};
      if(!record||!gate_.identityRecoveryQuiescent()) {frozen_=true;eraseIdentityBytes(readback,sizeof(readback));eraseIdentityBytes(&candidate,sizeof(candidate));return -EACCES;}
      const int wr=runtime_.write(key(slot),record,IdentityJournal::RecordSize);
      if(wr){frozen_=true;eraseIdentityBytes(readback,sizeof(readback));eraseIdentityBytes(&candidate,sizeof(candidate));return wr<0?wr:-EIO;}
      const int n=runtime_.read(key(slot),readback,sizeof(readback));
      const bool exact=n==int(sizeof(readback))&&!memcmp(record,readback,sizeof(readback));
      eraseIdentityBytes(readback,sizeof(readback));
      if(!exact||!gate_.identityRecoveryQuiescent()) {
        frozen_=true;eraseIdentityBytes(&candidate,sizeof(candidate));return n<0&&n!=-ENOENT?n:-EIO;
      }
    }
    eraseIdentityBytes(&candidate,sizeof(candidate));return 0;
  }
};
}
