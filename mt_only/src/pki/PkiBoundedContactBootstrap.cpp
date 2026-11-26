// SPDX-License-Identifier: GPL-3.0-only
#include "PkiBoundedContactBootstrap.h"
#include <string.h>

namespace mt::pki {
namespace {
bool sameIdentity(const IdentityBinding &a,const IdentityBinding &b){
  return a.version==b.version&&a.algorithm==b.algorithm&&a.generation==b.generation&&
    a.node==b.node&&!memcmp(a.publicKey,b.publicKey,sizeof(a.publicKey));
}
}

PkiBoundedContactBootstrap::PkiBoundedContactBootstrap(AppendStore &appendStore,
    PkiNonceCounterStore &counterStore,IdentityJournal &identity):
  appendStore_(appendStore),identity_(identity),history_(appendStore),counter_(counterStore){}

PkiBoundedContactBootstrap::~PkiBoundedContactBootstrap(){
  counter_.close();history_.close();ledger_.close();
}

int PkiBoundedContactBootstrap::fail(int result){
  counter_.close();history_.close();ledger_.close();state_=State::Failed;
  error_=result<0?result:-EIO;return error_;
}

int PkiBoundedContactBootstrap::prepare(){
  if(state_!=State::New)return -EACCES;
  IdentityBinding before;KeyRef beforeRef;
  if(!identity_.binding(before,beforeRef)||!identity_.active(beforeRef))return fail(-EACCES);

  uint8_t snapshot[PkiLedger::RecordSize]{};AppendAnchor anchor{};
  int rc=PkiAppendHistory::reconstructBaseline(appendStore_,before.node,before.publicKey,snapshot,anchor);
  if(rc)return fail(rc);
  rc=ledger_.loadValidatedAppendBaseline(snapshot,sizeof(snapshot),anchor,before.node,before.publicKey);
  eraseIdentityBytes(snapshot,sizeof(snapshot));
  if(rc)return fail(rc);

  rc=history_.open(ledger_,anchor);
  if(rc)return fail(rc);
  PkiLedger::Status replayed;
  if(!history_.ready()||!history_.counterMode()||history_.remaining()!=0||
     !history_.status(replayed)||replayed.format!=3||
     replayed.generation!=anchor.generation+PkiAppendHistory::EventLimit)return fail(-EILSEQ);

  IdentityBinding after;KeyRef afterRef;
  if(!identity_.binding(after,afterRef)||!identity_.active(afterRef)||
     beforeRef.slot!=afterRef.slot||beforeRef.generation!=afterRef.generation||
     !sameIdentity(before,after))return fail(-EXDEV);

  // The current format binds nonce authority to local node/public key, the
  // fully replayed generation, and immutable root CRC. It has no identity-
  // generation field and no extra per-peer nonce domain.
  PkiNonceCounterBinding nonceBinding{};nonceBinding.node=after.node;
  memcpy(nonceBinding.publicKey,after.publicKey,sizeof(nonceBinding.publicKey));
  nonceBinding.legacyGeneration=replayed.generation;nonceBinding.legacyCrc=anchor.rootCrc;
  rc=counter_.open(nonceBinding,true); // true only after full BMIT replay and identity recheck.
  if(rc||!counter_.active())return fail(rc?rc:-EACCES);

  // Publish readiness last. Until here, even the validated contact view and
  // active counter remain private to this object.
  state_=State::Ready;error_=0;return 0;
}

bool PkiBoundedContactBootstrap::status(PkiLedger::Status &out)const{
  return ready()&&history_.status(out);
}
bool PkiBoundedContactBootstrap::activePeers(uint32_t *out,size_t capacity,size_t &count)const{
  return ready()&&history_.activePeers(out,capacity,count);
}
bool PkiBoundedContactBootstrap::lookup(uint32_t node,uint8_t publicKey[32])const{
  return ready()&&history_.lookup(node,publicKey);
}
uint32_t PkiBoundedContactBootstrap::nonceReservedThrough()const{
  return ready()&&counter_.active()?counter_.reservedThrough():0;
}

}
