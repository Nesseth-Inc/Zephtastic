// SPDX-License-Identifier: GPL-3.0-only
// Disabled legacy-admin behavior over the preserved synthetic fixture only.
#if !defined(MT_PKI_LEGACY_TAIL_TOOLS_NATIVE_DISABLED)
#error "Compile every translation unit with MT_PKI_LEGACY_TAIL_TOOLS_NATIVE_DISABLED=1"
#endif
#define main m154_unused_preserved_boot_fixture_main
#include "m154_preserved_bounded_boot_tests.cpp"
#undef main
#include "pki/PkiPeerAdministration.h"

namespace {
static_assert(MT_PKI_LEGACY_TAIL_TOOLS_ENABLED==0,
              "the fixture and production declarations must use one disabled-profile ABI");

struct AdminStorageContext final : PkiStorageContext {
  unsigned depth=0;bool radioReady=true,storagePaused=false;
  bool tryEnter()override{++depth;return true;}
  void leave()override{assert(depth);--depth;}
  bool bootQuiescent()const override{return depth&&radioReady;}
  bool quiescent()const override{return depth&&storagePaused;}
};

struct AdminContext final : PeerAdministrationContext {
  AdminStorageContext &storageContext;
  unsigned enters=0,leaves=0,pauses=0,resumes=0,holds=0;
  explicit AdminContext(AdminStorageContext &c):storageContext(c){}
  bool tryEnter()override{++enters;++storageContext.depth;return true;}
  void leave()override{++leaves;assert(storageContext.depth);--storageContext.depth;}
  bool current(uint32_t epoch)const override{return epoch==17;}
  bool ready()const override{return true;}
  bool peerAllowed(const IdentityBinding &,const PkiPeerPin &)const override{return true;}
  bool pause()override{++pauses;if(storageContext.storagePaused)return false;storageContext.storagePaused=true;return true;}
  bool resume()override{++resumes;if(!storageContext.storagePaused)return false;storageContext.storagePaused=false;return true;}
  void hold()override{++holds;}
};

struct AdminAttempt {
  Settings settings;Boot boot{settings};AdminStorageContext context;Entropy entropy;Nor nor;
  PkiAppendHistory append{nor};PkiStorage storage{boot,settings,context,entropy,&append};
  explicit AdminAttempt(const Values &rows,const Bytes &flash){settings.rows=rows;nor.bytes=flash;}
};

void disabledLegacyEnableIsSideEffectFreeAndCompactContactsStillWorks(){
  Durable durable;
  AdminAttempt attempt(durable.settings.rows,durable.nor.bytes);
  const Values originalRows=attempt.settings.rows;
  const Bytes originalNor=attempt.nor.bytes;
  const unsigned originalNonceThrough=PkiNonceCounter::DefaultBlockSize;
  AdminContext admission(attempt.context);
  PkiPeerAdministration admin(attempt.storage,admission);
  UsbStorageCommand command{};
  command.localNode=durable.local.binding.node;
  memcpy(command.localPublic,durable.local.binding.publicKey,sizeof(command.localPublic));

  const unsigned writesBefore=attempt.settings.writes;
  assert(admin.enableStorage(command,17)==-ENOTSUP);
  assert(admin.enableStorage(command,17,true)==-ENOTSUP);
  assert(attempt.settings.writes==writesBefore&&attempt.settings.rows==originalRows&&
         attempt.nor.bytes==originalNor&&attempt.storage.state()==PkiStorage::State::New);
  assert(admission.enters==0&&admission.leaves==0&&admission.pauses==0&&
         admission.resumes==0&&admission.holds==0);

  assert(attempt.storage.prepare()==0&&attempt.storage.state()==PkiStorage::State::Prepared);
  assert(attempt.storage.activate(durable.local.binding.node)==0);
  assert(attempt.storage.boundedHistory()&&!attempt.storage.compactContacts());
  assert(attempt.storage.nonceReservedThrough()==originalNonceThrough);
  PkiPeerSet beforePeers;assert(attempt.storage.preparedPeers(beforePeers));
  assert(beforePeers.count()==1&&beforePeers.find(durable.later.binding.node)&&
         !beforePeers.find(durable.first.binding.node));
  IdentityBinding beforeIdentity;assert(attempt.storage.preparedBinding(beforeIdentity));


  assert(admin.enableContacts(command,17)==0);
  assert(attempt.storage.compactContacts()&&attempt.storage.contactGeneration()==2);
  assert(attempt.storage.nonceReservedThrough()==originalNonceThrough);
  // Initial provisioning legitimately has only one identity slot. Preserve
  // exact presence/absence and bytes for every non-ledger setting.
  assert(attempt.settings.rows.size()==originalRows.size());
  for(const auto &row:originalRows){
    if(row.first=="mtpki/ledger0"||row.first=="mtpki/ledger1")continue;
    const auto actual=attempt.settings.rows.find(row.first);
    assert(actual!=attempt.settings.rows.end()&&actual->second==row.second);
  }
  assert(attempt.nor.bytes==originalNor);
  PkiPeerSet afterPeers;assert(attempt.storage.preparedPeers(afterPeers));
  assert(afterPeers.count()==1&&afterPeers.find(durable.later.binding.node)&&
         !afterPeers.find(durable.first.binding.node));
  assert(admission.enters==1&&admission.leaves==1&&admission.pauses==1&&
         admission.resumes==1&&admission.holds==0&&!attempt.context.storagePaused);

  const auto migratedRows=attempt.settings.rows;const unsigned migratedWrites=attempt.settings.writes;
  assert(admin.enableStorage(command,17)==-ENOTSUP&&admin.enableStorage(command,17,true)==-ENOTSUP);
  assert(attempt.settings.rows==migratedRows&&attempt.settings.writes==migratedWrites&&
         attempt.nor.bytes==originalNor&&attempt.storage.compactContacts()&&
         attempt.storage.contactGeneration()==2);
  assert(admission.enters==1&&admission.leaves==1&&admission.pauses==1&&
         admission.resumes==1&&admission.holds==0);
  assert(attempt.storage.close());
  puts("PASS_M154_DISABLED_LEGACY_ENABLE_WITH_COMPACT_CONTACTS_ADMIN");
}
}

int main(){disabledLegacyEnableIsSideEffectFreeAndCompactContactsStillWorks();return 0;}
