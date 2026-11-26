// SPDX-License-Identifier: GPL-3.0-only
// Exercise the contact snapshot only through the real PkiStorage lifecycle.
#define main m154_preserved_bounded_boot_fixture_main
#include "m154_preserved_bounded_boot_tests.cpp"
#undef main

namespace {
PkiPeerPin pinFor(const Key &key){PkiPeerPin pin{};pin.node=key.binding.node;memcpy(pin.publicKey,key.binding.publicKey,32);return pin;}
bool sameRow(const Values &left,const Values &right,const char *name){
  const auto a=left.find(name),b=right.find(name);
  return (a==left.end())==(b==right.end())&&(a==left.end()||a->second==b->second);
}
}

int main(){
  Durable durable;
  // A power cut after the explicit migration marker but before the two contact
  // generations leaves the old effective membership live but nonmutable.
  Values pendingRows=durable.settings.rows;bool markerWritten=false;
  for(unsigned slot=0;slot<2;++slot){
    const char *key=slot?"mtpki/ledger1":"mtpki/ledger0";
    const auto it=pendingRows.find(key);assert(it!=pendingRows.end());
    if(PkiLedger::matchesAppendSelector(it->second.data(),it->second.size(),durable.anchor)){
      uint8_t marker[PkiContactMigrationMarker::RecordBytes];
      assert(PkiContactMigrationMarker::encode(durable.local.binding,durable.anchor,marker,sizeof(marker)));
      pendingRows[key]=Bytes(marker,marker+sizeof(marker));markerWritten=true;break;
    }
  }
  assert(markerWritten);
  {
    Attempt pending(pendingRows,durable.nor.bytes);
    assert(pending.storage.prepare()==0&&pending.storage.activate(durable.local.binding.node)==0);
    assert(!pending.storage.compactContacts()&&pending.storage.contactGeneration()==0);
    IdentityBinding expected;KeyRef ref;auto *identity=pending.storage.runtimeIdentity();
    assert(identity&&identity->binding(expected,ref));
    const PkiPeerPin firstPin=pinFor(durable.first);const Values before=pending.settings.rows;
    const unsigned writes=pending.settings.writes;pending.context.storagePaused=true;
    assert(pending.storage.addTrustedPeer(expected,firstPin)==-EAGAIN);
    assert(!pending.storage.revokeTrustedPeer(firstPin.node,firstPin.publicKey));
    assert(pending.settings.rows==before&&pending.settings.writes==writes);
    assert(pending.storage.enableCompactContacts(expected)==0&&pending.storage.compactContacts());
    assert(pending.storage.active(durable.later.binding.node,durable.later.binding.publicKey));
    assert(!pending.storage.active(durable.first.binding.node,durable.first.binding.publicKey));
  }

  Values nvs=durable.settings.rows;
  // Reconstruct from immutable legacy history before migrating its effective
  // contact set into the existing two NVS ledger values.
  Attempt attempt(nvs,durable.nor.bytes);
  assert(attempt.storage.prepare()==0&&attempt.storage.state()==PkiStorage::State::Prepared);
  assert(attempt.storage.activate(durable.local.binding.node)==0);
  assert(attempt.storage.extendedStorage()&&attempt.storage.boundedHistory());
  PkiPeerSet peers;assert(attempt.storage.preparedPeers(peers));
  assert(peers.count()==1&&peers.find(durable.later.binding.node));

  IdentityBinding expected;KeyRef ref;auto *identity=attempt.storage.runtimeIdentity();
  assert(identity&&identity->binding(expected,ref));
  const Values immutableBefore=attempt.settings.rows;
  const Bytes immutableJournal=attempt.nor.bytes;
  const unsigned beforeMigrationWrites=attempt.settings.writes;
  const unsigned beforeMigrationPrograms=attempt.nor.programs;
  attempt.context.storagePaused=true;
  assert(attempt.storage.enableCompactContacts(expected)==0);
  assert(attempt.storage.compactContacts()&&attempt.storage.contactGeneration()==2);
  assert(attempt.settings.writes==beforeMigrationWrites+3);
  for(const char *key:{"mtidentity/state0","mtidentity/state1","mtpki/nonces0","mtpki/nonces1"})
    assert(sameRow(immutableBefore,attempt.settings.rows,key));
  assert(attempt.nor.bytes==immutableJournal&&attempt.nor.programs==beforeMigrationPrograms);
  assert(attempt.storage.active(durable.later.binding.node,durable.later.binding.publicKey));
  assert(!attempt.storage.active(durable.first.binding.node,durable.first.binding.publicKey));
  assert(!attempt.storage.revoked(durable.later.binding.node));
  assert(attempt.storage.revoked(durable.first.binding.node));
  Key unknown{177};assert(!attempt.storage.revoked(unknown.binding.node));
  uint8_t found[32]{};assert(attempt.storage.lookup(durable.later.binding.node,found));
  assert(memcmp(found,durable.later.binding.publicKey,32)==0);

  // Duplicate authenticated RX is still denied by the bounded RAM window;
  // no new permanent replay domain or append-journal program is introduced.
  constexpr uint32_t rxId=0x71000001u;
  assert(attempt.storage.seen(durable.later.binding.node,durable.later.binding.publicKey,rxId)==ReplayCheck::Fresh);
  assert(attempt.storage.commitAuthenticated(durable.later.binding.node,durable.later.binding.publicKey,rxId));
  assert(attempt.storage.seen(durable.later.binding.node,durable.later.binding.publicKey,rxId)==ReplayCheck::Seen);
  assert(!attempt.storage.commitAuthenticated(durable.later.binding.node,durable.later.binding.publicKey,rxId));
  assert(attempt.nor.bytes==immutableJournal&&attempt.nor.programs==beforeMigrationPrograms);

  // Real immutable legacy tombstones stay denied. Ordinary compact contacts
  // can churn through stock-style forget/relearn without permanent bans.
  const PkiPeerPin hardPin=pinFor(durable.first);
  assert(attempt.storage.addTrustedPeer(expected,hardPin)==-EEXIST);
  assert(attempt.storage.revoked(hardPin.node));
  Key normalFirst{202};const PkiPeerPin firstPin=pinFor(normalFirst);
  assert(attempt.storage.addTrustedPeer(expected,firstPin)==0);
  assert(attempt.storage.active(firstPin.node,firstPin.publicKey));
  assert(!attempt.storage.revoked(firstPin.node));
  constexpr uint32_t preCompactionRxId=0x7a110001u;
  assert(attempt.storage.seen(firstPin.node,firstPin.publicKey,preCompactionRxId)==ReplayCheck::Fresh);
  assert(attempt.storage.commitAuthenticated(firstPin.node,firstPin.publicKey,preCompactionRxId));
  assert(attempt.storage.seen(firstPin.node,firstPin.publicKey,preCompactionRxId)==ReplayCheck::Seen);
  assert(attempt.storage.revokeTrustedPeer(firstPin.node,firstPin.publicKey));
  assert(!attempt.storage.active(firstPin.node,firstPin.publicKey));
  assert(!attempt.storage.revoked(firstPin.node));
  assert(attempt.storage.addTrustedPeer(expected,firstPin)==0);
  assert(!attempt.storage.revoked(firstPin.node));
  Key churn{61};const PkiPeerPin churnPin=pinFor(churn);
  for(unsigned i=0;i<20;++i){
    assert(attempt.storage.addTrustedPeer(expected,churnPin)==0);
    assert(attempt.storage.revokeTrustedPeer(churnPin.node,churnPin.publicKey));
  }
  assert(!attempt.storage.active(churnPin.node,churnPin.publicKey));

  // Fill all 16 contact positions, then prove the seventeenth refuses without
  // a settings or append-journal mutation.
  assert(attempt.storage.revokeTrustedPeer(firstPin.node,firstPin.publicKey));
  assert(!attempt.storage.revoked(firstPin.node));
  assert(attempt.storage.revokeTrustedPeer(durable.later.binding.node,durable.later.binding.publicKey));
  assert(!attempt.storage.revoked(durable.later.binding.node));
  Key extras[16]={Key{101},Key{102},Key{103},Key{104},Key{105},Key{106},Key{107},
                  Key{108},Key{109},Key{110},Key{111},Key{112},Key{113},Key{114},Key{115},Key{116}};
  for(const Key &key:extras)assert(attempt.storage.addTrustedPeer(expected,pinFor(key))==0);
  assert(attempt.storage.preparedPeers(peers)&&peers.count()==PkiPeerSet::Limit);
  Key overflow{249};const PkiPeerPin overflowPin=pinFor(overflow);
  const unsigned beforeOverflowWrites=attempt.settings.writes;
  assert(attempt.storage.addTrustedPeer(expected,overflowPin)==-ENOSPC);
  assert(attempt.settings.writes==beforeOverflowWrites&&attempt.nor.bytes==immutableJournal&&
         attempt.nor.programs==beforeMigrationPrograms);
  for(const char *key:{"mtidentity/state0","mtidentity/state1","mtpki/nonces0","mtpki/nonces1"})
    assert(sameRow(immutableBefore,attempt.settings.rows,key));
  const uint32_t finalGeneration=attempt.storage.contactGeneration();
  assert(finalGeneration>42);
  const Values committed=attempt.settings.rows;
  attempt.context.storagePaused=false;

  // Cold reboot reloads only the compact effective set. The removed legacy
  // peer stays removed, all 16 current pins survive, and TX nonce allocation
  // resumes at the next value from the preserved durable counter.
  Attempt reboot(committed,immutableJournal);
  assert(reboot.storage.prepare()==0&&reboot.storage.state()==PkiStorage::State::Prepared);
  assert(reboot.storage.activate(durable.local.binding.node)==0);
  assert(reboot.storage.compactContacts()&&reboot.storage.contactGeneration()==finalGeneration);
  assert(!reboot.storage.active(churnPin.node,churnPin.publicKey));
  assert(!reboot.storage.active(durable.first.binding.node,durable.first.binding.publicKey));
  assert(reboot.storage.revoked(durable.first.binding.node));
  assert(!reboot.storage.active(durable.later.binding.node,durable.later.binding.publicKey));
  assert(!reboot.storage.revoked(durable.later.binding.node));
  assert(!reboot.storage.revoked(churnPin.node));
  assert(reboot.storage.preparedPeers(peers)&&peers.count()==PkiPeerSet::Limit);
  const unsigned beforeTxPrograms=reboot.nor.programs;
  reboot.context.storagePaused=true;auto *rebootIdentity=reboot.storage.runtimeIdentity();
  KeyRef rebootRef;IdentityBinding rebootBinding;
  assert(rebootIdentity&&rebootIdentity->binding(rebootBinding,rebootRef));
  uint32_t extraNonce=0;
  assert(reboot.storage.reserve(rebootRef,rebootBinding.publicKey,rebootBinding.node,0x72000001u,extraNonce));
  assert(extraNonce==PkiNonceCounter::DefaultBlockSize+1&&
         reboot.storage.nonceReservedThrough()==2*PkiNonceCounter::DefaultBlockSize);
  // Cold boot resets only the bounded RAM replay window. A newly learned
  // compact contact has no historical lifetime entry; immutable hard deny and
  // original history are still independently preserved above.
  assert(reboot.storage.revokeTrustedPeer(extras[0].binding.node,extras[0].binding.publicKey));
  assert(reboot.storage.addTrustedPeer(rebootBinding,firstPin)==0);
  assert(reboot.storage.seen(firstPin.node,firstPin.publicKey,preCompactionRxId)==ReplayCheck::Fresh);
  assert(reboot.storage.commitAuthenticated(firstPin.node,firstPin.publicKey,preCompactionRxId));
  assert(reboot.nor.bytes==immutableJournal&&reboot.nor.programs==beforeTxPrograms);
  puts("PASS_M154_INPLACE_STORAGE_LIFECYCLE");
}
