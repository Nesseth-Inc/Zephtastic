// SPDX-License-Identifier: GPL-3.0-only
// M168 compact-only runtime startup/reservation tests. Synthetic storage only.
// This fixture reuses the typed full BMIT root/counter construction from M154,
// then seeds the contact pair with the production snapshot codec. It never
// instantiates a legacy PkiStorage class in this compact-flag translation unit.
#define main m154_preserved_bounded_boot_fixture_main
#include "m154_preserved_bounded_boot_tests.cpp"
#undef main

#ifndef CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY
#error "Compile this case only with the compact-runtime production define enabled"
#endif

namespace {
struct Memory final : mt::SettingsBackend {
  Values rows;
  unsigned reads=0,writes=0;
  std::string failWriteKey;
  bool failWrite=false,commitBeforeWriteFailure=false;
  std::string corruptReadKey;
  bool corruptRead=false;
  uint32_t *watchedOutput=nullptr;
  uint32_t sentinel=0;
  bool outputUnpublishedAtCounterWrite=false;
  bool outputUnpublishedAtCounterRead=false;

  explicit Memory(const Values &initial):rows(initial){}
  int read(const char *name,uint8_t *out,size_t cap)override{
    ++reads;
    if(!name||!out)return -EINVAL;
    const auto it=rows.find(name);if(it==rows.end())return -ENOENT;
    if(it->second.size()>cap)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());
    if(watchedOutput&&corruptReadKey==name&&
       (std::string(name)=="mtpki/nonces0"||std::string(name)=="mtpki/nonces1"))
      outputUnpublishedAtCounterRead=(*watchedOutput==sentinel);
    if(corruptRead&&corruptReadKey==name&&!it->second.empty())out[0]^=1;
    return int(it->second.size());
  }
  int write(const char *name,const uint8_t *data,size_t size)override{
    ++writes;
    if(!name||!data)return -EINVAL;
    if(watchedOutput&&(std::string(name)=="mtpki/nonces0"||std::string(name)=="mtpki/nonces1"))
      outputUnpublishedAtCounterWrite=(*watchedOutput==sentinel);
    if(failWrite&&failWriteKey==name){
      if(commitBeforeWriteFailure)rows[name]=Bytes(data,data+size);
      return -EIO;
    }
    rows[name]=Bytes(data,data+size);return 0;
  }
};
struct CompactBoot final : BootSettings {
  Memory &memory;bool held=false;unsigned begins=0,ends=0;
  explicit CompactBoot(Memory &m):memory(m){}
  int begin()override{if(held)return -EBUSY;held=true;++begins;return 0;}
  void end()override{assert(held);held=false;++ends;}
  int read(const char *name,uint8_t *out,size_t size)override{
    return held?memory.read(name,out,size):-EACCES;
  }
};
struct CompactAttempt {
  Memory settings;
  CompactBoot boot{settings};
  Context context;
  Entropy entropy;
  Nor nor;
  PkiAppendHistory append{nor};
  PkiStorage storage{boot,settings,context,entropy,&append};
  CompactAttempt(const Values &rows,const Bytes &flash):settings(rows){nor.bytes=flash;}
};

void encodePair(Values &rows,const IdentityBinding &local,const AppendAnchor &root,
                const PkiPeerPin *pins,size_t count,uint32_t firstGeneration=3){
  assert(firstGeneration>2&&firstGeneration<UINT32_MAX);
  for(unsigned slot=0;slot<2;++slot){
    Bytes record(PkiContactSnapshot::RecordBytes);
    assert(PkiContactSnapshot::encode(local,firstGeneration+slot,root.generation,
      root.rootCrc,pins,count,record.data(),record.size()));
    rows[slot?"mtpki/ledger1":"mtpki/ledger0"]=record;
  }
}
PkiPeerPin aliasPin(uint32_t alias,const Key &key){
  PkiPeerPin pin{};pin.node=alias;memcpy(pin.publicKey,key.binding.publicKey,32);
  assert(alias!=key.binding.node);return pin;
}
void expectClosed(const Values &rows,const Bytes &flash){
  CompactAttempt a(rows,flash);const Values saved=a.settings.rows;
  const unsigned writes=a.settings.writes,programs=a.nor.programs;
  assert(a.storage.prepare()<0);
  assert(a.storage.state()!=PkiStorage::State::Prepared&&a.storage.state()!=PkiStorage::State::Active);
  assert(!a.storage.boundedHistory()&&!a.storage.compactContacts());
  assert(a.settings.rows==saved&&a.settings.writes==writes&&a.nor.programs==programs);
  assert(!a.boot.held&&a.boot.begins==a.boot.ends);
  uint32_t output=0xdecafbad;
  assert(!a.storage.reserve({},nullptr,0,0x77110001,output)&&output==0xdecafbad);
}
void validCompactBoot(const Durable &durable,bool empty){
  Values rows=durable.settings.rows;PkiPeerPin alias{};Key owner{219};
  if(!empty)alias=aliasPin(0x20112233u,owner);
  encodePair(rows,durable.local.binding,durable.anchor,empty?nullptr:&alias,empty?0:1);
  CompactAttempt a(rows,durable.nor.bytes);const Values before=a.settings.rows;
  const unsigned writes=a.settings.writes,programs=a.nor.programs;
  assert(a.storage.prepare()==0&&a.storage.state()==PkiStorage::State::Prepared);
  IdentityBinding current{};assert(a.storage.preparedBinding(current));
  assert(current.node==durable.local.binding.node&&!memcmp(current.publicKey,durable.local.binding.publicKey,32));
  PkiPeerSet peers;assert(a.storage.preparedPeers(peers));
  assert(peers.count()==(empty?0:1));
  if(empty)assert(!a.storage.preparedPeer(alias.node));
  else {
    const auto *found=peers.find(alias.node);
    assert(found&&!memcmp(found->publicKey,owner.binding.publicKey,32));
    // The alias is an explicit owner pin; its public key is not used to invent
    // a CRC-derived node number. Manual NodeCache policy is separate metadata.
  }
  assert(a.settings.rows==before&&a.settings.writes==writes&&a.nor.programs==programs);
  assert(a.storage.activate(durable.local.binding.node)==0);
  assert(a.storage.boundedHistory()&&a.storage.compactContacts());
  assert(a.storage.contactGeneration()==4&&a.storage.nonceReservedThrough()==PkiNonceCounter::DefaultBlockSize);
  assert(a.settings.rows==before&&a.settings.writes==writes&&a.nor.programs==programs);
  if(!empty){uint8_t found[32]{};assert(a.storage.lookup(alias.node,found));
    assert(!memcmp(found,owner.binding.publicKey,32)&&a.storage.active(alias.node,found));}
  assert(a.storage.revoked(durable.first.binding.node)); // immutable tombstone survives compact membership.
}

void corruptAndIncompleteInputsFailClosed(const Durable &durable){
  Values compact=durable.settings.rows;Key aliasKey{227};auto pin=aliasPin(0x21335577u,aliasKey);
  encodePair(compact,durable.local.binding,durable.anchor,&pin,1);
  expectClosed(durable.settings.rows,durable.nor.bytes); // valid old source cannot fall back to legacy ledger.
  {
    Values bad=durable.settings.rows;bad.erase("mtpki/ledger0");bad.erase("mtpki/ledger1");
    expectClosed(bad,durable.nor.bytes); // required compact selector/state missing.
  }
  {
    Values bad=compact;bad.erase("mtpki/ledger1");expectClosed(bad,durable.nor.bytes);
  }
  {
    Values bad=compact;bad["mtpki/ledger1"][100]^=0x20;expectClosed(bad,durable.nor.bytes);
  }
  {
    Values bad=compact;Bytes wrongRoot(PkiContactSnapshot::RecordBytes);
    assert(PkiContactSnapshot::encode(durable.local.binding,4,durable.anchor.generation,
      durable.anchor.rootCrc^1u,&pin,1,wrongRoot.data(),wrongRoot.size()));
    bad["mtpki/ledger1"]=wrongRoot;expectClosed(bad,durable.nor.bytes);
  }
  {
    Values bad=compact;Bytes gap(PkiContactSnapshot::RecordBytes);
    assert(PkiContactSnapshot::encode(durable.local.binding,6,durable.anchor.generation,
      durable.anchor.rootCrc,&pin,1,gap.data(),gap.size()));
    bad["mtpki/ledger1"]=gap;expectClosed(bad,durable.nor.bytes);
  }
  {
    Values bad=compact;bad.erase("mtpki/nonces1");expectClosed(bad,durable.nor.bytes);
  }
  {
    Values bad=compact;bad["mtpki/nonces0"][PkiNonceCounter::RecordBytes-1]^=0x40;
    expectClosed(bad,durable.nor.bytes);
  }
  {
    Values bad=compact;bad["mtpki/nonces0"][8]^=0x01;expectClosed(bad,durable.nor.bytes);
  }
  {
    Bytes bad=durable.nor.bytes;bad[4092]='C';expectClosed(compact,bad); // counter-mode selector/commit proof.
  }
  {
    Bytes bad=durable.nor.bytes;bad[128+17]^=1;expectClosed(compact,bad); // immutable root baseline corruption.
  }
  {
    Bytes bad=durable.nor.bytes;bad[PkiAppendHistory::RootBytes+17]^=1;expectClosed(compact,bad); // replayed event corruption.
  }
  {
    CompactAttempt noAppend(compact,durable.nor.bytes);
    PkiStorage noHistory(noAppend.boot,noAppend.settings,noAppend.context,noAppend.entropy,nullptr);
    const Values saved=noAppend.settings.rows;const unsigned writes=noAppend.settings.writes;
    assert(noHistory.prepare()<0&&noHistory.state()!=PkiStorage::State::Prepared);
    assert(noAppend.settings.rows==saved&&noAppend.settings.writes==writes&&noAppend.nor.programs==0);
  }
}

void oldIdsAreRefusedAndNewNonceIsDurableBeforePublication(const Durable &durable){
  Values rows=durable.settings.rows;Key owner{231};auto pin=aliasPin(0x22446688u,owner);
  encodePair(rows,durable.local.binding,durable.anchor,&pin,1);
  CompactAttempt a(rows,durable.nor.bytes);
  assert(a.storage.prepare()==0&&a.storage.activate(durable.local.binding.node)==0);
  IdentityBinding local{};KeyRef ref{};auto *identity=a.storage.runtimeIdentity();
  assert(identity&&identity->binding(local,ref));a.context.storagePaused=true;
  const Values beforeOld=a.settings.rows;const unsigned writesOld=a.settings.writes;
  uint32_t extra=0xdecafbad;
  assert(!a.storage.reserve(ref,local.publicKey,local.node,0x51000001u,extra));
  assert(extra==0xdecafbad&&a.settings.writes==writesOld&&a.settings.rows==beforeOld);

  a.settings.watchedOutput=&extra;a.settings.sentinel=0xdecafbad;
  a.settings.corruptReadKey="mtpki/nonces0";
  const unsigned beforeWrite=a.settings.writes;const uint32_t through=a.storage.nonceReservedThrough();
  assert(through==PkiNonceCounter::DefaultBlockSize);
  assert(a.storage.reserve(ref,local.publicKey,local.node,0xf1000001u,extra));
  assert(extra==PkiNonceCounter::DefaultBlockSize+1);
  assert(a.settings.outputUnpublishedAtCounterWrite&&a.settings.outputUnpublishedAtCounterRead);
  assert(a.settings.writes==beforeWrite+1&&a.storage.nonceReservedThrough()==2*PkiNonceCounter::DefaultBlockSize);

  Values failRows=rows;CompactAttempt failed(failRows,durable.nor.bytes);
  assert(failed.storage.prepare()==0&&failed.storage.activate(durable.local.binding.node)==0);
  auto *failedIdentity=failed.storage.runtimeIdentity();KeyRef failedRef{};IdentityBinding failedLocal{};
  assert(failedIdentity&&failedIdentity->binding(failedLocal,failedRef));failed.context.storagePaused=true;
  failed.settings.watchedOutput=&extra;failed.settings.sentinel=0xdecafbad;
  failed.settings.failWriteKey="mtpki/nonces0";failed.settings.failWrite=true;failed.settings.commitBeforeWriteFailure=true;
  extra=0xdecafbad;const unsigned writes=failed.settings.writes;
  assert(!failed.storage.reserve(failedRef,failedLocal.publicKey,failedLocal.node,0xf1000002u,extra));
  assert(extra==0xdecafbad&&failed.settings.writes==writes+1);
  assert(failed.storage.state()==PkiStorage::State::Frozen);
  const unsigned noRetryWrites=failed.settings.writes;
  assert(!failed.storage.reserve(failedRef,failedLocal.publicKey,failedLocal.node,0xf1000003u,extra));
  assert(failed.settings.writes==noRetryWrites&&extra==0xdecafbad);

  CompactAttempt readback(rows,durable.nor.bytes);
  assert(readback.storage.prepare()==0&&readback.storage.activate(durable.local.binding.node)==0);
  auto *readbackIdentity=readback.storage.runtimeIdentity();KeyRef readbackRef{};IdentityBinding readbackLocal{};
  assert(readbackIdentity&&readbackIdentity->binding(readbackLocal,readbackRef));readback.context.storagePaused=true;
  readback.settings.corruptReadKey="mtpki/nonces0";readback.settings.corruptRead=true;
  extra=0xdecafbad;const unsigned readbackWrites=readback.settings.writes;
  assert(!readback.storage.reserve(readbackRef,readbackLocal.publicKey,readbackLocal.node,0xf1000004u,extra));
  assert(extra==0xdecafbad&&readback.settings.writes==readbackWrites+1);
  assert(readback.storage.state()==PkiStorage::State::Frozen);
  const unsigned frozenWrites=readback.settings.writes;
  assert(!readback.storage.reserve(readbackRef,readbackLocal.publicKey,readbackLocal.node,0xf1000005u,extra));
  assert(readback.settings.writes==frozenWrites&&extra==0xdecafbad);
}

void compactStorageMutationAndAuthenticatedReplay(const Durable &durable){
  Values rows=durable.settings.rows;
  encodePair(rows,durable.local.binding,durable.anchor,nullptr,0);
  CompactAttempt a(rows,durable.nor.bytes);
  assert(a.storage.prepare()==0&&a.storage.activate(durable.local.binding.node)==0);
  a.context.storagePaused=true;
  IdentityBinding local{};KeyRef ref{};auto *identity=a.storage.runtimeIdentity();
  assert(identity&&identity->binding(local,ref));

  Key owner{219};const uint32_t ownerNode=0x3055aab1u;
  auto ownerPin=aliasPin(ownerNode,owner);
  assert(bench::MtSignature::nodeId(owner.binding.publicKey)!=ownerNode);
  const Bytes nonce0=a.settings.rows.at("mtpki/nonces0");
  const Bytes nonce1=a.settings.rows.at("mtpki/nonces1");
  const unsigned norPrograms=a.nor.programs;

  const unsigned noCrcWrites=a.settings.writes;
  const uint32_t noCrcGeneration=a.storage.contactGeneration();
  const Values noCrcRows=a.settings.rows;
  assert(a.storage.addTrustedPeer(local,ownerPin)==-EINVAL); // RF path remains CRC-strict.
  uint8_t found[32]{};
  assert(a.settings.writes==noCrcWrites&&a.settings.rows==noCrcRows&&
         a.storage.contactGeneration()==noCrcGeneration&&
         !a.storage.lookup(ownerNode,found));

  assert(a.storage.putOwnerPeer(local,ownerPin)==0); // Explicit owner pin permits an alias.
  assert(a.storage.lookup(ownerNode,found)&&!memcmp(found,owner.binding.publicKey,32));
  const uint32_t ownerGeneration=a.storage.contactGeneration();
  const unsigned idempotentWrites=a.settings.writes;
  assert(a.storage.putOwnerPeer(local,ownerPin)==0);
  assert(a.storage.contactGeneration()==ownerGeneration&&a.settings.writes==idempotentWrites);

  Key rfPeer{241};PkiPeerPin rfPin{};rfPin.node=rfPeer.binding.node;
  memcpy(rfPin.publicKey,rfPeer.binding.publicKey,32);
  assert(a.storage.addTrustedPeer(local,rfPin)==0);
  assert(a.storage.lookup(rfPin.node,found)&&!memcmp(found,rfPin.publicKey,32));
  const uint32_t rxId=1; // Also used by putOwnerPeer's retired-window lookup.
  assert(a.storage.seen(ownerNode,owner.binding.publicKey,rxId)==ReplayCheck::Fresh);
  assert(a.storage.commitAuthenticated(ownerNode,owner.binding.publicKey,rxId));
  assert(a.storage.seen(ownerNode,owner.binding.publicKey,rxId)==ReplayCheck::Seen);
  assert(a.storage.seen(rfPin.node,rfPin.publicKey,rxId)==ReplayCheck::Fresh);
  assert(a.storage.commitAuthenticated(rfPin.node,rfPin.publicKey,rxId));
  assert(a.storage.seen(rfPin.node,rfPin.publicKey,rxId)==ReplayCheck::Seen);

  Key wrongKey{252};const unsigned refusalWrites=a.settings.writes;
  const Values refusalRows=a.settings.rows;
  assert(a.storage.seen(ownerNode,wrongKey.binding.publicKey,rxId)==ReplayCheck::WrongKey);
  assert(!a.storage.commitAuthenticated(ownerNode,wrongKey.binding.publicKey,rxId));
  assert(a.storage.seen(0x41556677u,rfPin.publicKey,rxId)==ReplayCheck::UnknownPeer);
  assert(!a.storage.commitAuthenticated(0x41556677u,rfPin.publicKey,rxId));
  assert(a.settings.writes==refusalWrites&&a.settings.rows==refusalRows&&
         a.storage.state()==PkiStorage::State::Active&&
         a.storage.seen(ownerNode,owner.binding.publicKey,rxId)==ReplayCheck::Seen&&
         a.storage.seen(rfPin.node,rfPin.publicKey,rxId)==ReplayCheck::Seen);

  Key replacement{263};auto replacementPin=aliasPin(ownerNode,replacement);
  assert(a.storage.putOwnerPeer(local,replacementPin)==0);
  assert(a.storage.active(ownerNode,replacement.binding.publicKey));
  assert(!a.storage.active(ownerNode,owner.binding.publicKey));
  assert(a.storage.active(rfPin.node,rfPin.publicKey));
  // Replacement drops only the retired owner's RAM replay slot; the sibling
  // keeps its accepted same-ID observation.
  assert(a.storage.seen(ownerNode,replacement.binding.publicKey,rxId)==ReplayCheck::Fresh);
  assert(a.storage.seen(rfPin.node,rfPin.publicKey,rxId)==ReplayCheck::Seen);
  assert(a.storage.commitAuthenticated(ownerNode,replacement.binding.publicKey,rxId));
  assert(a.storage.seen(ownerNode,replacement.binding.publicKey,rxId)==ReplayCheck::Seen);
  const uint32_t replacementGeneration=a.storage.contactGeneration();
  const unsigned replacementNoopWrites=a.settings.writes;
  assert(a.storage.putOwnerPeer(local,replacementPin)==0);
  assert(a.storage.contactGeneration()==replacementGeneration&&
         a.settings.writes==replacementNoopWrites);

  assert(a.storage.revokeTrustedPeer(ownerNode,replacement.binding.publicKey));
  assert(!a.storage.lookup(ownerNode,found)&&a.storage.lookup(rfPin.node,found));
  assert(!a.storage.revoked(ownerNode)); // Contact removal is not an append tombstone.
  assert(a.storage.revoked(durable.first.binding.node)); // Existing hard tombstone survives.
  assert(a.storage.seen(ownerNode,replacement.binding.publicKey,rxId)==ReplayCheck::UnknownPeer);
  assert(a.storage.seen(rfPin.node,rfPin.publicKey,rxId)==ReplayCheck::Seen);
  assert(a.settings.rows.at("mtpki/nonces0")==nonce0&&
         a.settings.rows.at("mtpki/nonces1")==nonce1&&a.nor.programs==norPrograms);
}

void compactContactUncertainCommitFreezes(const Durable &durable){
  Values rows=durable.settings.rows;
  encodePair(rows,durable.local.binding,durable.anchor,nullptr,0);
  Key owner{219};auto pin=aliasPin(0x3055aab1u,owner);
  {
    CompactAttempt a(rows,durable.nor.bytes);
    assert(a.storage.prepare()==0&&a.storage.activate(durable.local.binding.node)==0);
    a.context.storagePaused=true;IdentityBinding local{};KeyRef ref{};
    auto *identity=a.storage.runtimeIdentity();assert(identity&&identity->binding(local,ref));
    a.settings.failWrite=true;a.settings.failWriteKey="mtpki/ledger0";
    a.settings.commitBeforeWriteFailure=true;
    assert(a.storage.putOwnerPeer(local,pin)<0);
    assert(a.storage.state()==PkiStorage::State::Frozen);
    const unsigned writes=a.settings.writes;
    assert(a.storage.putOwnerPeer(local,pin)<0&&a.settings.writes==writes);
  }
  {
    CompactAttempt a(rows,durable.nor.bytes);
    assert(a.storage.prepare()==0&&a.storage.activate(durable.local.binding.node)==0);
    a.context.storagePaused=true;IdentityBinding local{};KeyRef ref{};
    auto *identity=a.storage.runtimeIdentity();assert(identity&&identity->binding(local,ref));
    a.settings.corruptRead=true;a.settings.corruptReadKey="mtpki/ledger0";
    assert(a.storage.putOwnerPeer(local,pin)<0);
    assert(a.storage.state()==PkiStorage::State::Frozen);
    const unsigned writes=a.settings.writes;
    assert(a.storage.putOwnerPeer(local,pin)<0&&a.settings.writes==writes);
  }
}
}

int main(){
  static_assert(PkiAppendHistory::EventLimit==2624,"full preserved event history");
  Durable durable;
  validCompactBoot(durable,false);
  validCompactBoot(durable,true);
  corruptAndIncompleteInputsFailClosed(durable);
  oldIdsAreRefusedAndNewNonceIsDurableBeforePublication(durable);
  compactStorageMutationAndAuthenticatedReplay(durable);
  compactContactUncertainCommitFreezes(durable);
  puts("M168_COMPACT_RUNTIME_FIXTURE_READY");return 0;
}
