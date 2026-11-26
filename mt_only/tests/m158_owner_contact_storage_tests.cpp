// SPDX-License-Identifier: GPL-3.0-only
// Real compact storage with synthetic identity and the immutable full journal.
#define main m154_preserved_bounded_boot_fixture_main
#include "m154_preserved_bounded_boot_tests.cpp"
#undef main

static PkiPeerPin ownerPin(uint32_t node,const Key &key){
  PkiPeerPin pin{};pin.node=node;memcpy(pin.publicKey,key.binding.publicKey,32);return pin;
}
static bool sameSetting(const Values &a,const Values &b,const char *name){
  auto x=a.find(name),y=b.find(name);
  return (x==a.end())==(y==b.end())&&(x==a.end()||x->second==y->second);
}
int main(){
  Durable durable;Attempt a(durable.settings.rows,durable.nor.bytes);
  assert(!a.storage.prepare()&&!a.storage.activate(durable.local.binding.node));
  IdentityBinding local;KeyRef ref;assert(a.storage.runtimeIdentity()->binding(local,ref));
  Key other{61},replacement{62};const uint32_t alias=0x20112233u;
  auto pin=ownerPin(alias,other);PkiPeerSet checked;
  assert(alias!=other.binding.node&&!checked.assign(local,&pin,1));
  assert(checked.assignTrusted(local,&pin,1));
  const auto before=a.settings.rows;const Bytes journal=a.nor.bytes;
  assert(a.storage.putOwnerPeer(local,pin)==-EACCES);
  a.context.storagePaused=true;
  assert(a.storage.putOwnerPeer(local,pin)==-ENOTSUP);
  assert(!a.storage.enableCompactContacts(local));
  assert(a.storage.addTrustedPeer(local,pin)==-EINVAL); // RF/legacy strict factory unchanged.
  assert(!a.storage.putOwnerPeer(local,pin)&&a.storage.active(alias,pin.publicKey));
  const auto writes=a.settings.writes;const auto generation=a.storage.contactGeneration();
  assert(!a.storage.putOwnerPeer(local,pin)&&a.settings.writes==writes&&a.storage.contactGeneration()==generation);
  auto duplicate=pin;duplicate.node++;
  assert(a.storage.putOwnerPeer(local,duplicate)==-EEXIST&&a.settings.writes==writes);
  auto self=ownerPin(local.node,replacement);assert(a.storage.putOwnerPeer(local,self)==-EINVAL);
  auto wrong=local;wrong.generation++;assert(a.storage.putOwnerPeer(wrong,pin)==-EINVAL);
  auto hard=ownerPin(durable.first.binding.node,durable.first);
  assert(a.storage.revoked(hard.node)&&a.storage.putOwnerPeer(local,hard)==-EEXIST);
  assert(a.storage.addTrustedPeer(local,hard)==-EEXIST&&a.storage.revoked(hard.node));
  // An active legacy row is forgotten, not turned into a synthetic hard ban.
  auto legacy=ownerPin(durable.later.binding.node,durable.later);
  assert(a.storage.revokeTrustedPeer(legacy.node,legacy.publicKey));
  assert(!a.storage.revoked(legacy.node)&&!a.storage.active(legacy.node,legacy.publicKey));
  assert(!a.storage.addTrustedPeer(local,legacy)&&a.storage.active(legacy.node,legacy.publicKey));
  // Owner replacement uses the same node with a different admitted public key.
  constexpr uint32_t oldIncoming=0x77000001u;
  assert(a.storage.commitAuthenticated(alias,pin.publicKey,oldIncoming));
  assert(a.storage.seen(alias,pin.publicKey,oldIncoming)==ReplayCheck::Seen);
  auto changed=ownerPin(alias,replacement);assert(!a.storage.putOwnerPeer(local,changed));
  assert(!a.storage.active(alias,pin.publicKey)&&a.storage.active(alias,changed.publicKey));
  constexpr uint32_t incoming=0x78000001u;
  assert(a.storage.seen(alias,changed.publicKey,incoming)==ReplayCheck::Fresh);
  assert(a.storage.commitAuthenticated(alias,changed.publicKey,incoming));
  assert(a.storage.seen(alias,changed.publicKey,incoming)==ReplayCheck::Seen);
  assert(!a.storage.commitAuthenticated(alias,changed.publicKey,incoming));
  // Trusted replay bind is explicit; the general API retains CRC binding.
  PkiReplayWindow window;
  assert(!window.bind(0,alias,changed.publicKey,local.node));
  assert(window.bindTrusted(0,alias,changed.publicKey,local.node));
  assert(a.nor.bytes==journal&&a.nor.programs==0);
  for(const char *key:{"mtidentity/state0","mtidentity/state1","mtpki/nonces0","mtpki/nonces1"})
    assert(sameSetting(before,a.settings.rows,key));
  const Values committed=a.settings.rows;
  Attempt reboot(committed,journal);assert(!reboot.storage.prepare()&&!reboot.storage.activate(local.node));
  uint8_t found[32]{};assert(reboot.storage.lookup(alias,found)&&!memcmp(found,changed.publicKey,32));
  assert(reboot.storage.preparedPeers(checked)&&checked.count()==2&&checked.find(alias));
  assert(reboot.storage.revoked(hard.node));
  assert(reboot.storage.seen(alias,changed.publicKey,incoming)==ReplayCheck::Fresh); // Stock bounded window resets.
  assert(reboot.storage.nonceReservedThrough()==a.storage.nonceReservedThrough());
  assert(reboot.nor.bytes==journal&&reboot.nor.programs==0);
  puts("PASS_M158_OWNER_CONTACT_STORAGE_ALIAS_REPLACE_REOPEN_FORGET_HARD_DENY_AND_NONCE_PRESERVATION");return 0;
}
