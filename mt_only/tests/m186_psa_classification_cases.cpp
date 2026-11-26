// SPDX-License-Identifier: GPL-3.0-only
// Real pinned PSA classification through PkiRuntime; public synthetic identities.
#include "pki_runtime_fixture.h"
#include <algorithm>
#include <set>

namespace {
struct FaultApi final:mt::pki::PsaApi {
  bool failImport=false,failAead=false,failDestroy=false;
  std::set<psa_key_id_t> live;
  psa_key_id_t lastImported=0;
  psa_status_t importKey(const psa_key_attributes_t *attributes,const uint8_t *data,
                         size_t size,psa_key_id_t *id)override {
    if(failImport)return PSA_ERROR_INSUFFICIENT_MEMORY;
    const auto result=PsaApi::importKey(attributes,data,size,id);
    if(result==PSA_SUCCESS){live.insert(*id);lastImported=*id;}
    return result;
  }
  psa_status_t aead(bool encrypt,psa_key_id_t id,const uint8_t *nonce,
                    const uint8_t *input,size_t size,uint8_t *output,
                    size_t capacity,size_t *length)override {
    if(failAead)return PSA_ERROR_HARDWARE_FAILURE;
    return PsaApi::aead(encrypt,id,nonce,input,size,output,capacity,length);
  }
  psa_status_t destroy(psa_key_id_t id)override {
    if(failDestroy)return PSA_ERROR_HARDWARE_FAILURE;
    const auto result=PsaApi::destroy(id);
    if(result==PSA_SUCCESS)live.erase(id);
    return result;
  }
  ~FaultApi()override {
    // Test harness only: after the backend/runtime have been destroyed, clean
    // IDs whose injected destroy failure intentionally left them allocated.
    for(const auto id:live)(void)PsaApi::destroy(id);
  }
};

struct DirectPause final:mt::pki::LedgerStorageGate {
  bool quiescent()const override{return true;}
};

struct ClassificationFixture {
  Key local{11},peer{97};Memory identityBytes,ledgerBytes;
  MonocypherIdentityDeriver deriver;mt::pki::IdentityJournal identity;
  mt::pki::PkiLedger ledger;DirectPause pause;Entropy entropy;
  FaultApi api;mt::pki::PsaPkiBackend backend{api};
  mt::experimental::PkiCrypto crypto{backend};
  mt::pki::LedgerNonceAuthority nonce{identity,ledger,ledgerBytes,entropy,pause};
  mt::pki::LedgerReplayAuthority replay{identity,ledger,ledgerBytes,pause};
  mt::pki::PkiRuntime runtime{crypto,backend,identity,nonce,replay};
  mt::Endpoint endpoint;bench::MtCodec codec;

  ClassificationFixture(){
    assert(backend.begin());
    assert(!identity.load(identityBytes,deriver));
    IdentityCandidate candidate(local.binding,local.scalar);
    assert(!identity.store(identityBytes,deriver,candidate,true));
    assert(!ledger.load(ledgerBytes));
    assert(!ledger.initialize(ledgerBytes,local.binding.publicKey,local.binding.node,true));
    assert(!ledger.addTrustedPeer(ledgerBytes,peer.binding.node,peer.binding.publicKey,true));
    assert(runtime.open(local.binding.node,peer.binding.node));
  }

  Bytes frame(uint32_t id,const Bytes &payload){
    uint8_t ciphertext[239]{};size_t encryptedSize=0;
    assert(crypto.encrypt(peer.scalar,local.binding.publicKey,peer.binding.node,id,
      0x12345678,payload.data(),payload.size(),ciphertext,sizeof(ciphertext),encryptedSize));
    Bytes wire(encryptedSize+16);put(wire.data(),local.binding.node);
    put(wire.data()+4,peer.binding.node);put(wire.data()+8,id);
    wire[12]=0;wire[13]=0;wire[15]=uint8_t(peer.binding.node);
    memcpy(wire.data()+16,ciphertext,encryptedSize);return wire;
  }
  mt::pki::ReceiveClassification classify(const Bytes &wire,uint32_t now=100){
    return runtime.receiveClassified(endpoint,codec,wire.data(),wire.size(),now,71,false);
  }
  bool event(const Bytes &wire,mt::pki::PrivateEvent &out){
    return runtime.event(0,wire.data(),wire.size(),out);
  }
};
}

int main(){
  using mt::pki::ReceiveClassification;
  {
    ClassificationFixture f;const auto payload=data("real PSA auth");auto wire=f.frame(901,payload);
    wire[16+payload.size()]^=1; // Corrupt the first byte of the CCM tag only.
    uint8_t output[227];memset(output,0x5a,sizeof(output));size_t outputSize=99;
    const auto cryptoResult=f.crypto.decryptClassified(f.peer.scalar,f.local.binding.publicKey,
      f.peer.binding.node,901,wire.data()+16,wire.size()-16,output,sizeof(output),outputSize);
    assert(cryptoResult==mt::experimental::DecryptResult::AuthenticationFailed&&outputSize==0);
    assert(std::all_of(output,output+sizeof(output),[](uint8_t value){return value==0x5a;}));
    const auto writes=f.ledgerBytes.writes;
    assert(f.classify(wire)==ReceiveClassification::Unauthenticated);
    mt::pki::PrivateEvent event{};assert(!f.event(wire,event));
    assert(f.ledgerBytes.writes==writes&&f.backend.healthy()&&f.api.live.empty());
    std::cout<<"PASS pinned PSA invalid tag is Unauthenticated, emits no event/plaintext, commits no replay and releases every volatile key\n";
  }
  {
    ClassificationFixture f;const auto payload=data("real PSA valid");auto wire=f.frame(902,payload);
    const auto writes=f.ledgerBytes.writes;
    assert(f.classify(wire)==ReceiveClassification::AuthenticatedConsumed);
    mt::pki::PrivateEvent event{};assert(f.event(wire,event));
    assert(event.from==f.peer.binding.node&&event.to==f.local.binding.node&&event.id==902&&
      event.size==payload.size()&&!memcmp(event.data,payload.data(),payload.size()));
    assert(f.ledgerBytes.writes>writes&&f.backend.healthy()&&f.api.live.empty());
    std::cout<<"PASS pinned PSA valid tag is consumed only after authenticated replay commit and bounded event admission\n";
  }
  {
    ClassificationFixture f;const auto payload=data("PSA backend fault");auto wire=f.frame(903,payload);
    const auto writes=f.ledgerBytes.writes;f.api.failImport=true;
    assert(f.classify(wire)==ReceiveClassification::Hold);
    mt::pki::PrivateEvent event{};assert(!f.event(wire,event));
    assert(f.ledgerBytes.writes==writes&&f.backend.healthy()&&f.api.live.empty());
    std::cout<<"PASS PSA key-import failure remains Hold without output, replay commit or leaked volatile key\n";
  }
  {
    ClassificationFixture f;const auto payload=data("PSA operation fault");auto wire=f.frame(905,payload);
    const auto writes=f.ledgerBytes.writes;f.api.failAead=true;
    assert(f.classify(wire)==ReceiveClassification::Hold);
    mt::pki::PrivateEvent event{};assert(!f.event(wire,event));
    assert(f.ledgerBytes.writes==writes&&f.backend.healthy()&&f.api.live.empty());
    std::cout<<"PASS PSA AEAD operation failure remains Hold without output, replay commit or leaked volatile key\n";
  }
  {
    ClassificationFixture f;const auto payload=data("PSA cleanup fault");auto wire=f.frame(904,payload);
    wire[16+payload.size()]^=1;const auto writes=f.ledgerBytes.writes;f.api.failDestroy=true;
    assert(f.classify(wire)==ReceiveClassification::Hold);
    mt::pki::PrivateEvent event{};assert(!f.event(wire,event));
    assert(f.ledgerBytes.writes==writes&&!f.backend.healthy()&&f.backend.hasQuarantinedKey());
    std::cout<<"PASS PSA cleanup failure remains Hold, quarantines backend and publishes no event or replay record\n";
  }
}
