// SPDX-License-Identifier: GPL-3.0-only
// Independent public synthetic identity, memory-backed restart/fault probes.
#include "pki/PkiLedger.h"
#include "MtSignature.h"
#include "monocypher.h"
#include <array>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using namespace mt::pki;
using Bytes=std::vector<uint8_t>;
struct Memory:mt::SettingsBackend {
  std::map<std::string,Bytes> rows;unsigned writes=0;int fault=0;bool afterWrite=false;
  std::function<void()> hook;
  int read(const char *key,uint8_t *out,size_t cap)override {
    if(fault==3&&afterWrite)return -EIO;
    auto i=rows.find(key);if(i==rows.end())return -ENOENT;
    if(i->second.size()>cap)return -E2BIG;
    memcpy(out,i->second.data(),i->second.size());
    if(fault==4&&afterWrite&&i->second.size())out[0]^=1;
    return int(i->second.size());
  }
  int write(const char *key,const uint8_t *p,size_t n)override {
    ++writes;if(hook)hook();if(fault==1)return -EIO;
    rows[key]=Bytes(p,p+n);afterWrite=true;
    if(fault==2)return -EIO;
    if(fault==5)rows[key].resize(n/2);
    return 0;
  }
};
struct Key {
  uint8_t scalar[32]{};IdentityBinding binding;
  explicit Key(unsigned seed){for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed+23*i);scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;
    crypto_x25519_public_key(binding.publicKey,scalar);binding.node=bench::MtSignature::nodeId(binding.publicKey);binding.generation=1;}
  IdentityCandidate candidate(){return IdentityCandidate(binding,scalar);}
};
struct Gate:LedgerStorageGate {bool allow=true;bool quiescent()const override{return allow;}};
struct Entropy:EntropySource {unsigned calls=0;bool fail=false;Memory *storage=nullptr;unsigned expectedWrites=0;
  bool fill(uint8_t *p,size_t n)override{++calls;assert(n==4);if(storage)assert(storage->writes>=expectedWrites);memset(p,0x29,n);return !fail;}};
void pass(const char *name){std::cout<<"PASS "<<name<<'\n';}
void put(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
uint32_t get(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
void seal(Bytes &v){uint32_t c=~0u;for(size_t i=0;i<v.size()-4;++i){c^=v[i];for(unsigned b=0;b<8;++b)c=(c>>1)^((0u-(c&1))&0xedb88320u);}put(v.data()+v.size()-4,~c);}
struct Fixture {
  Memory memory;Key local{19},remote{71};MonocypherIdentityDeriver derive;IdentityJournal identity;PkiLedger ledger;Gate gate;Entropy entropy;
  Fixture(){assert(!identity.load(memory,derive));assert(!identity.store(memory,derive,local.candidate(),true));assert(!ledger.load(memory));
    assert(!ledger.initialize(memory,local.binding.publicKey,local.binding.node,true));assert(!ledger.addTrustedPeer(memory,remote.binding.node,remote.binding.publicKey,true));}
  KeyRef ref(){IdentityBinding b;KeyRef r;assert(identity.binding(b,r));return r;}
};
int main(){
  {
    Fixture f;LedgerNonceAuthority authority(f.identity,f.ledger,f.memory,f.entropy,f.gate);uint32_t extra=0;
    f.entropy.storage=&f.memory;f.entropy.expectedWrites=f.memory.writes+1;
    assert(authority.reserve(f.ref(),f.local.binding.publicKey,f.local.binding.node,101,extra)&&extra==0x29292929);
    const auto saved=f.memory.rows;
    PkiLedger restarted;assert(!restarted.load(f.memory));assert(restarted.sent(101));
    LedgerNonceAuthority again(f.identity,restarted,f.memory,f.entropy,f.gate);extra=0xdeadbeef;
    assert(!again.reserve(f.ref(),f.local.binding.publicKey,f.local.binding.node,101,extra)&&extra==0xdeadbeef&&f.memory.rows==saved);
    ++f.local.binding.generation;auto old=f.ref();assert(!f.identity.store(f.memory,f.derive,f.local.candidate(),true));
    assert(!again.reserve(old,f.local.binding.publicKey,f.local.binding.node,102,extra));
    assert(!again.reserve(f.ref(),f.local.binding.publicKey,f.local.binding.node,101,extra));
    assert(again.reserve(f.ref(),f.local.binding.publicKey,f.local.binding.node,102,extra));
    Key different(143);different.binding.generation=3;assert(!f.identity.store(f.memory,f.derive,different.candidate(),true));
    const auto writes=f.memory.writes;assert(!again.reserve(f.ref(),different.binding.publicKey,different.binding.node,103,extra)&&f.memory.writes==writes);
    pass("restart_and_same_scalar_generation_preserve_nonce_domain_different_identity_disables");
  }
  {
    Fixture f;LedgerNonceAuthority a(f.identity,f.ledger,f.memory,f.entropy,f.gate);uint32_t extra=0x5a5a5a5a;
    f.gate.allow=false;auto writes=f.memory.writes;assert(!a.reserve(f.ref(),f.local.binding.publicKey,f.local.binding.node,201,extra)&&f.memory.writes==writes&&!f.entropy.calls);
    f.gate.allow=true;f.entropy.fail=true;assert(!a.reserve(f.ref(),f.local.binding.publicKey,f.local.binding.node,201,extra)&&extra==0x5a5a5a5a);
    f.entropy.fail=false;assert(!a.reserve(f.ref(),f.local.binding.publicKey,f.local.binding.node,201,extra));
    PkiLedger restart;assert(!restart.load(f.memory)&&restart.sent(201));
    pass("quiescence_blocks_without_reservation_entropy_failure_irreversibly_burns_id");
  }
  for(int fault:{1,2,3,4,5}){
    Fixture f;LedgerNonceAuthority a(f.identity,f.ledger,f.memory,f.entropy,f.gate);uint32_t extra=0x5a;
    f.memory.afterWrite=false;f.memory.fault=fault;assert(!a.reserve(f.ref(),f.local.binding.publicKey,f.local.binding.node,301,extra));
    assert(!f.ledger.ready()&&extra==0x5a&&!f.entropy.calls);const auto writes=f.memory.writes;
    f.memory.fault=0;assert(!a.reserve(f.ref(),f.local.binding.publicKey,f.local.binding.node,302,extra)&&f.memory.writes==writes);
    PkiLedger restart;const auto result=restart.load(f.memory);
    if(fault==5)assert(result<0&&!restart.ready());else {assert(!result);assert(restart.sent(301)==(fault!=1));}
  }
  pass("write_before_after_readback_and_torn_faults_freeze_no_retry_correct_restart");
  {
    Fixture f;LedgerReplayAuthority a(f.identity,f.ledger,f.memory,f.gate);const auto peer=f.remote.binding.node;const auto *key=f.remote.binding.publicKey;
    assert(a.seen(peer,key,401)==ReplayCheck::Fresh);assert(a.commitAuthenticated(peer,key,401));
    const auto writes=f.memory.writes;assert(!a.commitAuthenticated(peer,key,401)&&f.memory.writes==writes);
    PkiLedger restart;assert(!restart.load(f.memory));LedgerReplayAuthority b(f.identity,restart,f.memory,f.gate);
    assert(b.seen(peer,key,401)==ReplayCheck::Seen);
    auto wrong=f.remote;wrong.binding.publicKey[2]^=1;
    assert(b.seen(peer,wrong.binding.publicKey,402)==ReplayCheck::WrongKey&&!b.commitAuthenticated(peer,wrong.binding.publicKey,402));
    assert(b.seen(peer+1,key,402)==ReplayCheck::UnknownPeer&&!b.commitAuthenticated(peer+1,key,402));
    f.gate.allow=false;assert(!b.commitAuthenticated(peer,key,402)&&f.memory.writes==writes);
    pass("durable_replay_is_bound_to_exact_pinned_peer_and_quiescence");
  }
  {
    Fixture f;for(uint32_t id=1;id<=PkiLedger::SendLimit;++id)assert(!f.ledger.reserve(f.memory,id,true));
    const auto writes=f.memory.writes;assert(f.ledger.reserve(f.memory,999,true)==-ENOSPC&&f.memory.writes==writes);
    PkiLedger restart;assert(!restart.load(f.memory));for(uint32_t id=1;id<=PkiLedger::SendLimit;++id)assert(restart.sent(id));
    for(uint32_t id=1;id<=PkiLedger::ReceiveLimit;++id)assert(!restart.commitAuthenticated(f.memory,f.remote.binding.node,f.remote.binding.publicKey,id,true));
    assert(restart.seen(f.remote.binding.node,f.remote.binding.publicKey,999)==ReplayCheck::Full);
    assert(restart.seen(f.remote.binding.node,f.remote.binding.publicKey,1)==ReplayCheck::Seen);
    assert(restart.commitAuthenticated(f.memory,f.remote.binding.node,f.remote.binding.publicKey,999,true)==-ENOSPC);
    pass("full_send_receive_ledgers_preserve_all_prior_ids_without_eviction");
  }
  {
    Fixture f;assert(!f.ledger.reserve(f.memory,501,true));assert(!f.ledger.reserve(f.memory,502,true));
    auto base=f.memory.rows;
    for(unsigned variant=0;variant<5;++variant){Memory m;m.rows=base;auto &a=m.rows["mtpki/ledger0"],&b=m.rows["mtpki/ledger1"];
      Bytes &newest=get(a.data()+8)>get(b.data()+8)?a:b;
      if(variant==0)put(newest.data()+PkiLedger::HeaderSize,503); // Prefix ID replacement, still unique and CRC-valid.
      if(variant==1){newest[48]=1;put(newest.data()+PkiLedger::HeaderSize,503);put(newest.data()+PkiLedger::HeaderSize+4,0);put(newest.data()+8,get(newest.data()+8)-1);} // Same-gen conflicting state.
      if(variant==2){newest[49]=0;memset(newest.data()+PkiLedger::PeerOffset,0,PkiLedger::PeerSize);put(newest.data()+8,get(newest.data()+8)-1);}
      if(variant==3){auto &peer=newest[PkiLedger::PeerOffset];put(&peer,f.remote.binding.node+1);}
      if(variant==4){m.rows.erase(get(a.data()+8)>get(b.data()+8)?"mtpki/ledger1":"mtpki/ledger0");}
      if(variant<4)seal(newest);
      PkiLedger probe;assert(probe.load(m)<0&&!probe.ready());
    }
    pass("crc_valid_conflicting_history_peer_rebind_and_missing_predecessor_refused");
    Memory rollback;rollback.rows=base;auto &a=rollback.rows["mtpki/ledger0"],&b=rollback.rows["mtpki/ledger1"];
    if(get(a.data()+8)<get(b.data()+8))b=a;else a=b;
    PkiLedger restored;assert(!restored.load(rollback)&&restored.sent(501)&&!restored.sent(502));
    pass("coherent_older_snapshot_rollback_is_outside_crc_journal_security_claim");
  }
  {
    Fixture f;bool nested=false;f.memory.hook=[&]{nested=true;assert(f.ledger.reserve(f.memory,701,true)==-EBUSY);assert(!f.ledger.close());};
    assert(!f.ledger.reserve(f.memory,700,true)&&nested&&f.ledger.sent(700)&&!f.ledger.sent(701));
    uint8_t out[32];memset(out,0x5a,32);assert(!f.ledger.lookup(0xeeeeeeee,out));for(auto v:out)assert(v==0x5a);
    assert(f.ledger.close());assert(f.ledger.load(f.memory)==-EACCES);assert(f.ledger.initialize(f.memory,f.local.binding.publicKey,f.local.binding.node,true)==-EACCES);
    pass("reentry_close_lookup_output_and_terminal_lifecycle_boundaries");
  }
  {
    Fixture f;LedgerReplayAuthority authority(f.identity,f.ledger,f.memory,f.gate);const auto node=f.remote.binding.node;const auto *key=f.remote.binding.publicKey;
    assert(authority.commitAuthenticated(node,key,801));f.gate.allow=false;const auto writes=f.memory.writes;
    assert(!authority.revokeTrustedPeer(node,key)&&f.memory.writes==writes&&authority.active(node,key));
    f.gate.allow=true;assert(authority.revokeTrustedPeer(node,key));assert(!authority.active(node,key));
    uint8_t out[32];memset(out,0x5a,32);assert(!authority.lookup(node,out));for(auto b:out)assert(b==0x5a);
    f.gate.allow=false;assert(authority.revokeTrustedPeer(node,key)&&f.memory.writes==writes+1);
    assert(!authority.commitAuthenticated(node,key,802));PkiLedger restart;assert(!restart.load(f.memory));
    assert(!restart.active(node,key)&&restart.seen(node,key,801)==ReplayCheck::UnknownPeer);
    assert(restart.addTrustedPeer(f.memory,node,key,true)==-EEXIST);
    Key different(117);assert(restart.addTrustedPeer(f.memory,node,different.binding.publicKey,true)==-EEXIST);
    assert(restart.addTrustedPeer(f.memory,node+1,key,true)==-EEXIST);
    pass("revoked_peer_tombstone_is_durable_idempotent_and_cannot_rebind_node_or_key");
    assert(!restart.reserve(f.memory,901,true));const auto base=f.memory.rows;
    for(unsigned variant=0;variant<2;++variant){Memory m;m.rows=base;auto &a=m.rows["mtpki/ledger0"],&b=m.rows["mtpki/ledger1"];
      auto &latest=get(a.data()+8)>get(b.data()+8)?a:b;auto *peer=latest.data()+PkiLedger::PeerOffset;
      if(variant==0){peer[37]=0;latest[48]=2;put(latest.data()+PkiLedger::HeaderSize+4,902);} // Same operation count, illicit reactivation.
      else {latest[48]=0;put(latest.data()+PkiLedger::HeaderSize,0);peer[36]=2;put(peer+44,802);} // Illicit post-tombstone RX growth.
      seal(latest);PkiLedger invalid;assert(invalid.load(m)<0&&!invalid.ready());
    }
    pass("crc_valid_tombstone_reactivation_and_post_revocation_receive_growth_refused");
  }
  for(int fault:{1,2,3,4,5}){
    Fixture f;LedgerReplayAuthority authority(f.identity,f.ledger,f.memory,f.gate);
    f.memory.afterWrite=false;f.memory.fault=fault;assert(!authority.revokeTrustedPeer(f.remote.binding.node,f.remote.binding.publicKey));
    assert(!authority.active(f.remote.binding.node,f.remote.binding.publicKey)&&!f.ledger.ready());
    const auto writes=f.memory.writes;f.memory.fault=0;assert(!authority.revokeTrustedPeer(f.remote.binding.node,f.remote.binding.publicKey)&&f.memory.writes==writes);
    PkiLedger restart;auto result=restart.load(f.memory);
    if(fault==5)assert(result<0);else{assert(!result);assert(restart.active(f.remote.binding.node,f.remote.binding.publicKey)==(fault==1));}
  }
  pass("uncertain_revocation_freezes_live_authority_without_false_durable_success");
}
