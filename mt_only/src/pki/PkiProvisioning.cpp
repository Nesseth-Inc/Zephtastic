// SPDX-License-Identifier: GPL-3.0-only
#include "PkiProvisioning.h"
#include "MtSignature.h"
#include <string.h>

namespace mt::pki {
namespace {
template<size_t N>struct Scratch {uint8_t bytes[N]{};~Scratch(){eraseIdentityBytes(bytes,N);}};
struct Snapshot {BootSettings &boot;~Snapshot(){boot.end();}};
bool validNode(uint32_t node){return node>1&&node<0xfffffffeu;}
bool admissible(const uint8_t pub[32]){Scratch<32> ed;return bench::MtSignature::admissiblePublicKey(pub,ed.bytes);}
bool validPeer(const IdentityBinding &local,const ProvisionPeer &peer){
  return validNode(peer.node)&&peer.node!=local.node&&memcmp(peer.publicKey,local.publicKey,32)&&
    bench::MtSignature::nodeId(peer.publicKey)==peer.node&&admissible(peer.publicKey);
}
const char *key(unsigned step){return step==1?"mtpki/ledger0":step==2?"mtpki/ledger1":step==3?"mtidentity/state0":nullptr;}
size_t size(unsigned step){return step==3?IdentityJournal::RecordSize:PkiLedger::RecordSize;}
}
PkiProvisioning::Lease::Lease(const PkiProvisioning &owner):owner_(owner){
  if(!owner_.context_.tryEnter())return;
  if(owner_.busy_){owner_.context_.leave();return;}
  owner_.busy_=true;held_=true;
}
PkiProvisioning::Lease::~Lease(){if(held_){owner_.busy_=false;owner_.context_.leave();}}
PkiProvisioning::PkiProvisioning(ProvisioningSettings &settings,ProvisioningContext &context,EntropySource &entropy):
  settings_(settings),context_(context),entropy_(entropy),backend_(*this){}
PkiProvisioning::~PkiProvisioning(){wipe();}
void PkiProvisioning::wipe(){eraseIdentityBytes(scalar_,sizeof(scalar_));staged_=IdentityBinding{};started_=0;step_=0;wroteStep_=false;}
int PkiProvisioning::fail(State state,int error){wipe();state_=state;error_=error;return error;}
bool PkiProvisioning::expired()const{return (state_==State::Staged||state_==State::Committing)&&context_.millis()-started_>=StageLifetimeMs;}
int PkiProvisioning::gate()const{if(expired())return -ETIMEDOUT;return context_.quiescent()?0:-EACCES;}
int PkiProvisioning::endFailure(int error){
  return fail(attemptedWrite_?State::Frozen:error==-ETIMEDOUT?State::Expired:State::Rejected,error<0?error:-EIO);
}
PkiProvisioning::State PkiProvisioning::state()const{Lease hold(*this);return hold?state_:State::Busy;}
int PkiProvisioning::lastError()const{Lease hold(*this);return hold?error_:-EBUSY;}
bool PkiProvisioning::exact(const IdentityBinding &expected)const{
  return expected.version==staged_.version&&expected.algorithm==staged_.algorithm&&
    expected.generation==staged_.generation&&expected.node==staged_.node&&!memcmp(expected.publicKey,staged_.publicKey,32);
}
int PkiProvisioning::load(IdentityJournal &identity,PkiLedger &ledger){
  int rc=gate();if(rc)return rc;
  rc=settings_.begin();if(rc)return rc;
  Snapshot snapshot{settings_};if((rc=gate()))return rc;
  rc=identity.load(settings_,deriver_);if(rc)return rc;
  if((rc=gate()))return rc;
  rc=ledger.load(settings_);if(rc)return rc;
  return gate();
}
int PkiProvisioning::Backend::write(const char *name,const uint8_t *data,size_t length){
  const char *expected=key(owner_.step_);
  if(!owner_.busy_||owner_.state_!=State::Committing||owner_.wroteStep_||!expected||!name||
     strcmp(name,expected)||!data||length!=size(owner_.step_))return -EACCES;
  int rc=owner_.gate();if(rc)return rc;
  owner_.attemptedWrite_=true;owner_.wroteStep_=true;
  rc=owner_.settings_.persist(name,data,length);
  if(rc)return rc<0?rc:-EIO;
  return owner_.gate();
}
int PkiProvisioning::Backend::read(const char *name,uint8_t *out,size_t capacity){
  const char *expected=key(owner_.step_);
  if(!owner_.busy_||owner_.state_!=State::Committing||!owner_.wroteStep_||!expected||!name||
     strcmp(name,expected)||!out||capacity!=size(owner_.step_))return -EACCES;
  int rc=owner_.gate();if(rc){eraseIdentityBytes(out,capacity);return rc;}
  rc=owner_.settings_.begin();if(rc){eraseIdentityBytes(out,capacity);return rc;}
  Snapshot snapshot{owner_.settings_};rc=owner_.gate();
  if(!rc)rc=owner_.settings_.read(name,out,capacity);
  const int after=owner_.gate();if(after)rc=after;
  if(rc!=int(capacity)){eraseIdentityBytes(out,capacity);return rc<0?rc:-EILSEQ;}
  return rc;
}
int PkiProvisioning::stage(IdentityBinding &out){
  Lease hold(*this);if(!hold)return -EBUSY;
  if(state_!=State::New&&state_!=State::Staged)return -EACCES;
  int rc=gate();if(rc)return endFailure(rc);
  if(state_==State::Staged){
    IdentityJournal identity;PkiLedger ledger;
    rc=load(identity,ledger);if(rc)return fail(State::Frozen,rc);
    if(identity.state()!=IdentityJournal::State::Empty||ledger.state()!=PkiLedger::State::Empty)return endFailure(-EEXIST);
    if(!context_.candidateAllowed(staged_))return endFailure(-EEXIST);
    if((rc=gate()))return endFailure(rc);
    out=staged_;return 0;
  }
  IdentityJournal identity;PkiLedger ledger;
  rc=load(identity,ledger);if(rc)return fail(State::Frozen,rc);
  if(identity.state()!=IdentityJournal::State::Empty||ledger.state()!=PkiLedger::State::Empty)return endFailure(-EEXIST);
  // Start the finite lifetime before the one bounded entropy operation.
  state_=State::Staged;started_=context_.millis();
  if(!entropy_.fill(scalar_,32))return endFailure(-ENODATA);
  if((rc=gate()))return endFailure(rc);
  scalar_[0]&=248;scalar_[31]=(scalar_[31]&127)|64;
  if(!deriver_.derivePublic(scalar_,staged_.publicKey))return endFailure(-EIO);
  staged_.generation=1;staged_.node=bench::MtSignature::nodeId(staged_.publicKey);
  if(!validNode(staged_.node)||!admissible(staged_.publicKey)||!context_.candidateAllowed(staged_))return endFailure(-EEXIST);
  if((rc=gate()))return endFailure(rc);
  out=staged_;return 0;
}
int PkiProvisioning::commit(const IdentityBinding &expected,const ProvisionPeer &pin){
  Lease hold(*this);if(!hold)return -EBUSY;
  if(state_!=State::Staged)return -EACCES;
  int rc=gate();if(rc)return endFailure(rc);
  // Copy public confirmation data before invoking external policy/storage hooks.
  const IdentityBinding confirmed=expected;const ProvisionPeer peer=pin;
  if(!exact(confirmed)||!validPeer(staged_,peer))return endFailure(-EINVAL);
  if(!context_.candidateAllowed(staged_)||!context_.peerAllowed(staged_,peer))return endFailure(-EEXIST);
  if((rc=gate()))return endFailure(rc);
  IdentityJournal identity;PkiLedger ledger;
  rc=load(identity,ledger);if(rc)return fail(State::Frozen,rc);
  if(identity.state()!=IdentityJournal::State::Empty||ledger.state()!=PkiLedger::State::Empty)return endFailure(-EEXIST);
  if(!context_.candidateAllowed(staged_)||!context_.peerAllowed(staged_,peer))return endFailure(-EEXIST);
  if((rc=gate()))return endFailure(rc);
  state_=State::Committing;step_=1;wroteStep_=false;
  rc=ledger.initialize(backend_,staged_.publicKey,staged_.node,true);if(rc)return endFailure(rc);
  step_=2;wroteStep_=false;
  rc=ledger.addTrustedPeer(backend_,peer.node,peer.publicKey,true);if(rc)return endFailure(rc);
  step_=3;wroteStep_=false;
  {
    IdentityCandidate candidate(staged_,scalar_);
    rc=identity.store(backend_,deriver_,candidate,true);
  }
  if(rc)return endFailure(rc);
  IdentityBinding binding;KeyRef ref;uint32_t only=0;
  if(!identity.binding(binding,ref)||!exact(binding)||!ledger.boundTo(binding.publicKey,binding.node)||
     !ledger.singleStoredActivePeer(only)||only!=peer.node||!ledger.active(peer.node,peer.publicKey))return endFailure(-EIO);
  if((rc=gate()))return endFailure(rc);
  wipe();state_=State::Committed;error_=0;return 0;
}
int PkiProvisioning::tick(){
  Lease hold(*this);if(!hold)return -EBUSY;
  if(state_!=State::Staged)return 0;
  const int rc=gate();return rc?endFailure(rc):0;
}
bool PkiProvisioning::cancel(){
  Lease hold(*this);if(!hold||(state_!=State::New&&state_!=State::Staged))return false;
  fail(State::Cancelled,0);return true;
}
int PkiProvisioning::inspect(ProvisionStatus &out){
  Lease hold(*this);if(!hold)return -EBUSY;
  if(expired())endFailure(-ETIMEDOUT);
  if(!context_.quiescent())return -EACCES;
  ProvisionStatus result;result.attempt=state_;
  IdentityJournal identity;PkiLedger ledger;
  int rc=load(identity,ledger);
  if(rc){
    if(state_==State::Staged){if(rc==-ETIMEDOUT)endFailure(rc);else fail(State::Frozen,rc);}
    result.attempt=state_;result.error=rc;result.stored=rc==-EILSEQ?ProvisionStored::Corrupt:ProvisionStored::Frozen;out=result;return rc;
  }
  const bool haveIdentity=identity.state()==IdentityJournal::State::Ready,haveLedger=ledger.state()==PkiLedger::State::Ready;
  if(!haveIdentity&&!haveLedger)result.stored=ProvisionStored::Empty;
  else if(haveIdentity!=haveLedger)result.stored=ProvisionStored::Partial;
  else{
    IdentityBinding binding;KeyRef ref;ProvisionPeer peer;
    if(!identity.binding(binding,ref)||!ledger.boundTo(binding.publicKey,binding.node)){
      result.stored=ProvisionStored::Mismatch;result.error=rc=-EXDEV;
    }else if(!ledger.singleStoredActivePeer(peer.node))result.stored=ProvisionStored::Partial;
    else if(!ledger.lookup(peer.node,peer.publicKey)||!validPeer(binding,peer)){
      result.stored=ProvisionStored::Corrupt;result.error=rc=-EILSEQ;
    }else{
      result.local=binding;result.peer=peer;result.hasBinding=result.hasPeer=true;result.stored=ProvisionStored::Complete;
    }
  }
  if(const int after=gate()){
    if(state_==State::Staged)endFailure(after);
    result=ProvisionStatus{};result.attempt=state_;result.stored=ProvisionStored::Frozen;result.error=rc=after;
  }
  out=result;return rc;
}
}
