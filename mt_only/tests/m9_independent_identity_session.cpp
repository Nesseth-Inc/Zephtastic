// SPDX-License-Identifier: GPL-3.0-only
// Cross-lane lifecycle review: real identity journal/provider -> real PKI session.
// Synthetic public fixtures, memory-only persistence, OpenSSL crypto backend.
// This tests ownership/revocation/nonce/replay contracts, not target PSA execution.
#include "PkiIdentity.h"
#include "PkiSession.h"
#include "MtSignature.h"
#include "monocypher.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
using namespace mt::pki;
using R=SessionResult;
struct Memory:mt::SettingsBackend {
  std::map<std::string,std::vector<uint8_t>> data;unsigned writes=0;int fault=0;
  int read(const char *name,uint8_t *p,size_t cap)override {
    const auto found=data.find(name);if(found==data.end())return -ENOENT;
    if(found->second.size()>cap)return -E2BIG;
    memcpy(p,found->second.data(),found->second.size());return int(found->second.size());
  }
  int write(const char *name,const uint8_t *p,size_t n)override {
    ++writes;if(fault==1)return -EIO;data[name]={p,p+n};return fault==2?-EIO:0;
  }
};
struct Backend:mt::experimental::PkiBackend {
  bool fail=false;unsigned crypts=0;
  bool sha256(const uint8_t *p,size_t n,uint8_t out[32])override {
    unsigned len=0;return EVP_Digest(p,n,out,&len,EVP_sha256(),nullptr)==1&&len==32;
  }
  bool hmac256(const uint8_t key[32],const uint8_t *p,size_t n,uint8_t out[32])override {
    unsigned len=0;return HMAC(EVP_sha256(),key,32,p,n,out,&len)&&len==32;
  }
  bool ccm(bool encrypt,const uint8_t key[32],const uint8_t iv[13],const uint8_t *p,size_t n,uint8_t *out,uint8_t tag[8])override {
    ++crypts;if(fail){memset(out,0xe7,n);return false;}
    auto *ctx=EVP_CIPHER_CTX_new();if(!ctx)return false;int size=0;bool ok;
    if(encrypt) {
      ok=EVP_EncryptInit_ex(ctx,EVP_aes_256_ccm(),nullptr,nullptr,nullptr)==1&&
        EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_CCM_SET_IVLEN,13,nullptr)==1&&EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_CCM_SET_TAG,8,nullptr)==1&&
        EVP_EncryptInit_ex(ctx,nullptr,nullptr,key,iv)==1&&EVP_EncryptUpdate(ctx,nullptr,&size,nullptr,int(n))==1&&
        EVP_EncryptUpdate(ctx,out,&size,p,int(n))==1&&size==int(n)&&EVP_EncryptFinal_ex(ctx,out+size,&size)==1&&size==0&&
        EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_CCM_GET_TAG,8,tag)==1;
    }else {
      ok=EVP_DecryptInit_ex(ctx,EVP_aes_256_ccm(),nullptr,nullptr,nullptr)==1&&
        EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_CCM_SET_IVLEN,13,nullptr)==1&&EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_CCM_SET_TAG,8,tag)==1&&
        EVP_DecryptInit_ex(ctx,nullptr,nullptr,key,iv)==1&&EVP_DecryptUpdate(ctx,nullptr,&size,nullptr,int(n))==1&&
        EVP_DecryptUpdate(ctx,out,&size,p,int(n))==1&&size==int(n);
    }
    EVP_CIPHER_CTX_free(ctx);return ok;
  }
};
struct Authority:NonceAuthority {
  // A metadata generation is never a new nonce domain for the same key.
  std::set<std::tuple<std::array<uint8_t,32>,uint32_t,uint32_t>> used;unsigned calls=0;bool fail=false;
  bool reserve(KeyRef,const uint8_t publicKey[32],uint32_t local,uint32_t id,uint32_t &extra)override {
    std::array<uint8_t,32> stableKey;memcpy(stableKey.data(),publicKey,32);
    ++calls;const bool fresh=used.emplace(stableKey,local,id).second;
    extra=0x55667788;return fresh&&!fail; // Public fixture nonce, not a runtime RNG.
  }
};
struct Key {
  uint8_t scalar[32]{};IdentityBinding binding;
  explicit Key(unsigned seed) {
    for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed+i*31);
    scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;
    crypto_x25519_public_key(binding.publicKey,scalar);binding.node=bench::MtSignature::nodeId(binding.publicKey);binding.generation=1;
  }
  ~Key(){crypto_wipe(scalar,32);}
};
struct Fixture {
  Key local{11},peer{97};Memory memory;MonocypherIdentityDeriver derive;IdentityJournal identity;
  Backend backend;mt::experimental::PkiCrypto crypto{backend};Authority authority;PkiSession session{crypto,identity,authority};
  KeyRef ref;IdentityBinding binding;
  Fixture() {
    assert(!identity.load(memory,derive));IdentityCandidate candidate(local.binding,local.scalar);
    assert(!identity.store(memory,derive,candidate,true)&&identity.binding(binding,ref));
  }
  bool open(){return session.open(ref,binding.node,binding.publicKey,peer.binding.node,peer.binding.publicKey);}
  int rotate(int fault=0) {
    auto next=local.binding;next.generation=2;IdentityCandidate candidate(next,local.scalar);memory.fault=fault;
    return identity.store(memory,derive,candidate,true);
  }
};
static unsigned failed=0;
static void check(bool good,const char *name){failed+=!good;std::cout<<(good?"PASS ":"FAIL ")<<name<<"\n";}
static bool same(const PkiFrame &a,const PkiFrame &b){return a.from==b.from&&a.to==b.to&&a.id==b.id&&a.size==b.size&&!memcmp(a.bytes,b.bytes,sizeof(a.bytes));}
static constexpr uint8_t Data[]={8,1,18,4,'t','e','s','t'};
static void initialBindingAndRevocation() {
  bool good=true;
  for(int fault:{0,1,2}) {
    Fixture f;good&=f.open();PkiFrame packet;good&=f.session.send(12,Data,sizeof(Data),packet)==R::Ok;
    const auto count=f.backend.crypts;PkiFrame retry;good&=f.session.retry(12,retry)==R::Ok&&same(packet,retry)&&f.backend.crypts==count;
    const auto rc=f.rotate(fault);good&=(rc==0)==(fault==0)&&!f.identity.active(f.ref)&&!f.session.ready();
    const auto unchanged=retry;good&=f.session.retry(12,retry)==R::NotReady&&same(unchanged,retry)&&f.backend.crypts==count;
    good&=f.session.send(13,Data,sizeof(Data),retry)==R::NotReady;
  }
  check(good,"first_identity_slot_opens_and_rotation_or_write_uncertainty_revokes_cached_retry");
}
static void reservationFailures() {
  bool good=true;
  for(unsigned mode=0;mode<2;++mode) {
    Fixture f;good&=f.open();f.authority.fail=mode==0;f.backend.fail=mode==1;
    PkiFrame out;out.id=0xdeadbeef;out.bytes[0]=0xa5;const auto before=out;
    good&=f.session.send(30,Data,sizeof(Data),out)==(mode==0?R::NonceUnavailable:R::CryptoFailure)&&same(out,before);
    f.authority.fail=false;f.backend.fail=false;
    good&=f.session.send(30,Data,sizeof(Data),out)==R::NonceReused&&same(out,before)&&f.authority.calls==1;
    good&=f.session.send(31,Data,sizeof(Data),out)==R::Ok&&f.session.retire(31)&&f.session.send(31,Data,sizeof(Data),out)==R::NonceReused;
  }
  Fixture f;good&=f.open();PkiFrame packet;
  for(uint32_t i=1;i<=4;++i)good&=f.session.send(i,Data,sizeof(Data),packet)==R::Ok;
  const auto calls=f.authority.calls;good&=f.session.send(5,Data,sizeof(Data),packet)==R::NoSlot&&f.authority.calls==calls;
  good&=f.session.retire(1)&&f.session.send(5,Data,sizeof(Data),packet)==R::Ok;
  check(good,"failed_encrypt_and_entropy_burn_ids_but_full_queue_never_reserves");
}
static void authenticatedReplayAndBounds() {
  Fixture f;bool good=f.open();PkiFrame packet;packet.from=f.peer.binding.node;packet.to=f.binding.node;
  uint8_t output[227];size_t length=0;
  for(uint32_t i=1;i<=33;++i) {
    packet.id=i;assert(f.crypto.encrypt(f.peer.scalar,f.binding.publicKey,packet.from,i,55,Data,sizeof(Data),packet.bytes,sizeof(packet.bytes),packet.size));
    if(i==1) {
      auto wrong=packet;wrong.bytes[0]^=1;memset(output,0x5a,sizeof(output));length=999;
      good&=f.session.receive(wrong,output,sizeof(output),length)==R::CryptoFailure&&!length&&std::all_of(output,output+sizeof(output),[](uint8_t v){return v==0x5a;});
      wrong=packet;wrong.from=f.binding.node;good&=f.session.receive(wrong,output,sizeof(output),length)==R::PeerMismatch;
    }
    const auto result=f.session.receive(packet,output,sizeof(output),length);
    good&=result==(i<=32?R::Ok:R::RxExhausted);
    if(i<=32)good&=length==sizeof(Data)&&!memcmp(output,Data,length)&&f.session.receive(packet,output,sizeof(output),length)==R::Replay&&!length;
  }
  f.session.close();good&=!f.session.ready()&&!f.open();
  check(good,"auth_failure_cannot_poison_replay_and_receive_limit_never_evicts_old_ids");
}
static void absentAndInvalidTuple() {
  Memory m;IdentityJournal j;MonocypherIdentityDeriver derive;IdentityBinding out;out.node=123;KeyRef ref{99,99};
  bool good=!j.load(m,derive)&&!m.writes&&!j.binding(out,ref)&&out.node==123&&ref.slot==99;
  Key key(33);auto wrong=key.binding;wrong.node^=0x1234;IdentityCandidate bad(wrong,key.scalar);
  good&=j.store(m,derive,bad,true)<0&&!m.writes&&!j.active({0,1});
  IdentityCandidate valid(key.binding,key.scalar);good&=j.store(m,derive,valid,false)<0&&!m.writes;
  good&=!j.store(m,derive,valid,true)&&j.binding(out,ref)&&j.active(ref);
  check(good,"absent_identity_stays_disabled_and_invalid_or_nonquiescent_tuple_never_writes");
}
static void sameMaterialGenerationKeepsNonceDomain() {
  Fixture f;bool good=f.open();PkiFrame packet;good&=f.session.send(17,Data,sizeof(Data),packet)==R::Ok;
  good&=!f.rotate()&&f.identity.binding(f.binding,f.ref);
  PkiSession replacement(f.crypto,f.identity,f.authority);
  good&=replacement.open(f.ref,f.binding.node,f.binding.publicKey,f.peer.binding.node,f.peer.binding.publicKey);
  const auto before=packet;good&=replacement.send(17,Data,sizeof(Data),packet)==R::NonceUnavailable&&same(packet,before);
  good&=replacement.send(18,Data,sizeof(Data),packet)==R::Ok;
  check(good,"external_nonce_authority_keeps_same_material_domain_across_identity_generations");
}
int main(){initialBindingAndRevocation();reservationFailures();authenticatedReplayAndBounds();absentAndInvalidTuple();sameMaterialGenerationKeepsNonceDomain();return failed?1:0;}
