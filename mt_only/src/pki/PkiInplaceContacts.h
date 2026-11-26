// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiLedger.h"
#include "PkiContactStore.h"
#include "PkiContactMigrationMarker.h"

namespace mt::pki {
// Reuses the two existing NVS ledger VALUE names. The owner must first replay
// the whole preserved BMIT history and verify the durable nonce pair, and must
// serialize every call with the service/storage barrier. No names, identities,
// nonce records, append-root bytes or history cells are deleted or initialized.
class PkiInplaceContacts {
public:
  enum class State:uint8_t {New,Legacy,Pending,Ready,Frozen};
private:
  enum class Kind:uint8_t {Base,Selector,Marker,Contact};
  SettingsBackend &backend_;const PkiLedger &baseline_;
  IdentityBinding local_{};AppendAnchor root_{};
  State state_=State::New;unsigned first_=0;
  PkiPeerPin legacy_[PkiContactSnapshot::Capacity]{};size_t legacyCount_=0;
  PkiContactStore store_;
  static const char *physical(unsigned slot){return slot?"mtpki/ledger1":"mtpki/ledger0";}
  int freeze(int rc){state_=State::Frozen;return rc<0?rc:-EIO;}
  bool baselineMatches(const uint8_t *record,size_t size)const{
    if(size!=PkiLedger::RecordSize)return false;
    uint8_t expected[64];
    for(size_t offset=0;offset<size;offset+=sizeof(expected)){
      const size_t n=size-offset<sizeof(expected)?size-offset:sizeof(expected);
      if(!baseline_.snapshotRead(offset,expected,n)||memcmp(record+offset,expected,n))return false;
    }
    return true;
  }
  bool genesisMatches(const uint8_t *record,size_t size,uint32_t generation,
                      const PkiPeerPin *pins,size_t count)const{
    uint8_t expected[PkiContactSnapshot::RecordBytes];
    return size==sizeof(expected)&&
      PkiContactSnapshot::encode(local_,generation,root_.generation,root_.rootCrc,
                                 pins,count,expected,sizeof(expected))&&
      !memcmp(record,expected,sizeof(expected));
  }
  int classify(unsigned slot,Kind &kind,uint32_t &generation,const PkiPeerPin *pins,size_t count){
    uint8_t raw[PkiLedger::RecordSize];const int n=backend_.read(physical(slot),raw,sizeof(raw));
    if(n<0)return n;
    generation=0;
    if(n==int(sizeof(raw))&&baselineMatches(raw,sizeof(raw))){kind=Kind::Base;return 0;}
    if(PkiLedger::matchesAppendSelector(raw,size_t(n),root_)){kind=Kind::Selector;return 0;}
    if(PkiContactMigrationMarker::matches(raw,size_t(n),local_,root_)){kind=Kind::Marker;return 0;}
    PkiContactSnapshot::Value value;
    if(n!=int(PkiContactSnapshot::RecordBytes)||
       !PkiContactSnapshot::decode(raw,size_t(n),local_,root_.generation,root_.rootCrc,value))return -EILSEQ;
    // Initial generations cannot invent or remove trust before the migration
    // has completed. Later generations are ordinary committed membership.
    if(value.generation<=2&&!genesisMatches(raw,size_t(n),value.generation,pins,count))return -EILSEQ;
    generation=value.generation;kind=Kind::Contact;return 0;
  }
  class Slots final:public SettingsBackend {
    PkiInplaceContacts &owner_;
    int slot(const char *name)const{
      if(name&&!strcmp(name,PkiContactStore::key(0)))return int(owner_.first_);
      if(name&&!strcmp(name,PkiContactStore::key(1)))return int(1-owner_.first_);
      return -1;
    }
  public:
    explicit Slots(PkiInplaceContacts &owner):owner_(owner){}
    int read(const char *name,uint8_t *out,size_t cap)override{
      const int at=slot(name);if(at<0||!out||cap!=PkiContactSnapshot::RecordBytes)return -EACCES;
      uint8_t raw[PkiLedger::RecordSize];const int n=owner_.backend_.read(physical(unsigned(at)),raw,sizeof(raw));
      if(n<0)return n;
      if(n==int(cap)){memcpy(out,raw,cap);return n;}
      // Only exact validated legacy/marker records are logically absent.
      if(owner_.baselineMatches(raw,size_t(n))||
         PkiLedger::matchesAppendSelector(raw,size_t(n),owner_.root_)||
         PkiContactMigrationMarker::matches(raw,size_t(n),owner_.local_,owner_.root_))return -ENOENT;
      return -EILSEQ;
    }
    int write(const char *name,const uint8_t *data,size_t size)override{
      const int at=slot(name);if(at<0||!data||size!=PkiContactSnapshot::RecordBytes)return -EACCES;
      return owner_.backend_.write(physical(unsigned(at)),data,size);
    }
  } slots_;
public:
  PkiInplaceContacts(SettingsBackend &backend,const PkiLedger &baseline):
    backend_(backend),baseline_(baseline),slots_(*this){}
  PkiInplaceContacts(const PkiInplaceContacts &)=delete;
  PkiInplaceContacts &operator=(const PkiInplaceContacts &)=delete;
  State state()const{return state_;}
  bool ready()const{return state_==State::Ready&&store_.ready();}
  uint32_t generation()const{return ready()?store_.generation():0;}
  // Read-only; pending genesis may retain only the exact old effective pins.
  int load(const IdentityBinding &local,const AppendAnchor &root,const PkiPeerPin *legacy,size_t count){
    if(state_!=State::New)return -EACCES;
    PkiPeerSet checked;AppendAnchor expected;
    if(!(count?checked.assign(local,legacy,count):checked.assignEmpty(local))||
       !baseline_.boundTo(local.publicKey,local.node)||!baseline_.appendAnchor(expected)||
       expected.generation!=root.generation||expected.snapshotCrc!=root.snapshotCrc||
       expected.rootCrc!=root.rootCrc||expected.capacity!=root.capacity)return freeze(-EINVAL);
    local_=local;root_=root;legacyCount_=count;
    for(size_t i=0;i<count;++i)legacy_[i]=*checked.at(i);
    Kind kind[2];uint32_t generation[2];
    for(unsigned i=0;i<2;++i){const int rc=classify(i,kind[i],generation[i],legacy,count);if(rc)return freeze(rc);}
    if((kind[0]==Kind::Base&&kind[1]==Kind::Selector)||
       (kind[1]==Kind::Base&&kind[0]==Kind::Selector)){
      first_=kind[0]==Kind::Base?0:1;state_=State::Legacy;return 0;
    }
    for(unsigned marker=0;marker<2;++marker){
      const unsigned other=1-marker;
      if(kind[marker]==Kind::Marker&&(kind[other]==Kind::Base||
         (kind[other]==Kind::Contact&&generation[other]==1))){
        first_=other;state_=State::Pending;return 0;
      }
    }
    if(kind[0]!=Kind::Contact||kind[1]!=Kind::Contact)return freeze(-EILSEQ);
    first_=0;const int rc=store_.load(slots_,local_,root_.generation,root_.rootCrc);
    if(rc||!store_.ready())return freeze(rc?rc:-EILSEQ);
    state_=State::Ready;return 0;
  }
  // Explicit owner action, never called automatically at startup. A 64-byte
  // marker first replaces the redundant 1944-byte selector; existing-name
  // value overwrites avoid the settings name/value tombstone window.
  int migrate(){
    if(state_!=State::Legacy&&state_!=State::Pending)return -EACCES;
    if(state_==State::Legacy){
      uint8_t marker[PkiContactMigrationMarker::RecordBytes],readback[PkiLedger::RecordSize];
      if(!PkiContactMigrationMarker::encode(local_,root_,marker,sizeof(marker)))return freeze(-EINVAL);
      int rc=backend_.write(physical(1-first_),marker,sizeof(marker));if(rc)return freeze(rc);
      const int n=backend_.read(physical(1-first_),readback,sizeof(readback));
      if(n!=int(sizeof(marker))||memcmp(marker,readback,sizeof(marker)))return freeze(n<0?n:-EIO);
      state_=State::Pending;
    }
    // Complete only this admitted, canonical genesis. General-purpose store
    // initialization/resume remains available to offline callers; production
    // reuses its ordinary two-record loader to publish the verified pair.
    uint8_t record[PkiContactSnapshot::RecordBytes],readback[PkiLedger::RecordSize];
    uint8_t marker[PkiContactMigrationMarker::RecordBytes];
    if(!PkiContactMigrationMarker::encode(local_,root_,marker,sizeof(marker)))return freeze(-EINVAL);
    int n=backend_.read(physical(1-first_),readback,sizeof(readback));
    if(n!=int(sizeof(marker))||memcmp(marker,readback,sizeof(marker)))return freeze(n<0?n:-EILSEQ);
    if(!PkiContactSnapshot::encode(local_,1,root_.generation,root_.rootCrc,
                                  legacy_,legacyCount_,record,sizeof(record)))return freeze(-EINVAL);
    n=slots_.read(PkiContactStore::key(0),readback,sizeof(record));
    if(n==-ENOENT){
      const int rc=slots_.write(PkiContactStore::key(0),record,sizeof(record));if(rc)return freeze(rc);
      n=slots_.read(PkiContactStore::key(0),readback,sizeof(record));
    }
    if(n!=int(sizeof(record))||memcmp(record,readback,sizeof(record)))return freeze(n<0?n:-EIO);
    if(!PkiContactSnapshot::encode(local_,2,root_.generation,root_.rootCrc,
                                  legacy_,legacyCount_,record,sizeof(record)))return freeze(-EINVAL);
    int rc=slots_.write(PkiContactStore::key(1),record,sizeof(record));if(rc)return freeze(rc);
    n=slots_.read(PkiContactStore::key(1),readback,sizeof(record));
    if(n!=int(sizeof(record))||memcmp(record,readback,sizeof(record)))return freeze(n<0?n:-EIO);
    // load() independently decodes/readbacks BOTH adjacent records against the
    // exact identity/root binding. No readiness is published before this.
    rc=store_.load(slots_,local_,root_.generation,root_.rootCrc);
    if(rc||!store_.ready())return freeze(rc?rc:-EILSEQ);
    state_=State::Ready;return 0;
  }
  bool snapshot(PkiContactSnapshot::Value &out)const{return ready()&&store_.snapshot(out);}
  int replace(const PkiPeerPin *pins,size_t count){
    if(!ready())return -EACCES;
    const int rc=store_.replace(slots_,pins,count);
    if(!store_.ready())return freeze(rc?rc:-EIO);
    return rc;
  }
};
}
