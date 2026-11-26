// SPDX-License-Identifier: GPL-3.0-only
// Exercises public identity traffic while the private contact directory is
// empty. Synthetic keys and RAM journals only; no firmware/NVS fixture.
#include "pki_runtime_fixture.h"
#include "pki/MultiPeerRuntime.h"
#include "pki/PkiSigner.h"
#include "NodeAdmin.h"
#include "NodeJournal.h"
#include "monocypher.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace mt::pki;

namespace {
struct PeerKey final : KeyProvider {
  uint8_t scalar[32]{};
  IdentityBinding identity{};
  KeyRef ref{0, 1};
  explicit PeerKey(unsigned seed) {
    for (unsigned i=0;i<sizeof(scalar);++i) scalar[i]=uint8_t(seed+19*i+7);
    scalar[0]&=0xf8; scalar[31]=(scalar[31]&0x7f)|0x40;
    crypto_x25519_public_key(identity.publicKey,scalar);
    identity.node=bench::MtSignature::nodeId(identity.publicKey);
    identity.generation=1;
  }
  ~PeerKey() override { crypto_wipe(scalar,sizeof(scalar)); }
  bool active(KeyRef k) const override { return k.slot==ref.slot&&k.generation==ref.generation; }
  bool withPrivate(KeyRef k,PrivateKeyOperation &op) override { return active(k)&&op.apply(scalar); }
};

struct TestRadio final : mt::Radio {
  int state=0;
  bool isBusy=false,completed=false;
  unsigned sends=0,stops=0,restores=0;
  std::vector<Bytes> frames;
  bool ready() override { return true; }
  int configure(const mt::Config &) override { return 0; }
  int start() override { state=1; return 0; }
  int stop() override { ++stops;state=0; return 0; }
  int restore() override { ++restores;state=1; return 0; }
  int stopForStorage() override { ++stops;state=0; return 0; }
  int health() override { return state; }
  bool busy() override { return isBusy; }
  uint32_t airtime(size_t n) override { return n?100:0; }
  int send(uint8_t *p,size_t n) override { ++sends; frames.emplace_back(p,p+n); completed=false; return 0; }
  bool completion(int &result) override { result=0; return completed; }
};

struct ZeroPeerFixture {
  Key local{31};
  Memory identityMemory,ledgerMemory;
  MonocypherIdentityDeriver deriver;
  IdentityJournal identity;
  PkiLedger ledger;
  Gate gate;
  Entropy entropy;
  SignEntropy signing;
  Backend backend;
  mt::experimental::PkiCrypto crypto{backend};
  LedgerNonceAuthority nonce{identity,ledger,ledgerMemory,entropy,gate};
  LedgerReplayAuthority replay{identity,ledger,ledgerMemory,gate};
  PkiPeerSet empty;
  MultiPeerRuntime runtime{crypto,backend,identity,nonce,replay,&signing};
  mt::test::KnownChannelFixtureClock channelClock;
  TestRadio radio;
  mt::Service service{radio,true,local.binding.node,81,true,true,&runtime};

  ZeroPeerFixture() {
    // Match the stock-trial main boot latch before provider activation.
    assert(service.requirePrivateRuntime());
    assert(!identity.load(identityMemory,deriver));
    IdentityCandidate candidate(local.binding,local.scalar);
    assert(!identity.store(identityMemory,deriver,candidate,true));
    assert(!ledger.load(ledgerMemory));
    assert(!ledger.initialize(ledgerMemory,local.binding.publicKey,local.binding.node,true));
    assert(empty.assignEmpty(local.binding));
    assert(runtime.open(local.binding.node,empty));
    gate.service=&service;
    channelClock.bind(service);
    mt::Config config; config.bytes[4]=1;
    auto node=config.node(); node.setPublic(); config.node(node);
    assert(service.apply(config));
    assert(runtime.publicReady(local.binding.node)&&!runtime.ready(local.binding.node)&&service.privateDormant());
    assert(service.start(false,0));
    assert(service.armEndpoint(true));
  }
  ~ZeroPeerFixture() { service.stop(); }
};

static SignedData makeSignedNode(PeerKey &peer,SignEntropy &signing,uint32_t to,uint32_t id) {
  NodeInfoProfile profile;
  strcpy(profile.longName,"Zero-peer discovery");
  strcpy(profile.shortName,"M155");
  profile.hardwareModel=81;
  SigningEnvelope envelope; envelope.hasBitfield=true; envelope.bitfield=0;
  SignedData signedData;
  assert(PkiSigner::bootstrap(peer,peer.ref,peer.identity,signing,id,to,profile,envelope,signedData)==SigningResult::Ok);
  return signedData;
}

static Bytes signedNodeFrame(ZeroPeerFixture &f,PeerKey &peer,SignEntropy &signing,uint32_t id) {
  const auto data=makeSignedNode(peer,signing,f.local.binding.node,id);
  uint8_t frame[255]{}; size_t size=0;
  assert(f.service.codec.encodeSignedNodeInfo(data.from,data.to,data.id,data.bytes,data.size,
                                               frame,sizeof(frame),size));
  return Bytes(frame,frame+size);
}

static Bytes privateText(ZeroPeerFixture &f,PeerKey &peer,uint32_t id) {
  meshtastic_Data message{};
  message.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
  message.payload.size=10; memcpy(message.payload.bytes,"unadopted",10);
  message.has_bitfield=true;
  uint8_t plain[64]{}; auto stream=pb_ostream_from_buffer(plain,sizeof(plain));
  assert(pb_encode(&stream,meshtastic_Data_fields,&message));
  uint8_t cipher[239]{};size_t cipherSize=0;
  assert(f.crypto.encrypt(peer.scalar,f.local.binding.publicKey,peer.identity.node,id,0x12345678,
                          plain,stream.bytes_written,cipher,sizeof(cipher),cipherSize));
  Bytes frame(cipherSize+16);
  put(frame.data(),f.local.binding.node); put(frame.data()+4,peer.identity.node);
  put(frame.data()+8,id); frame[12]=0; frame[13]=0; frame[15]=uint8_t(peer.identity.node);
  memcpy(frame.data()+16,cipher,cipherSize);
  crypto_wipe(plain,sizeof(plain));crypto_wipe(cipher,sizeof(cipher));
  return frame;
}

}

int main() {
  {
    ZeroPeerFixture f;
    f.service.stop();auto cfg=f.service.config;cfg.setTxEnabled(true);
    // A real keyed primary. No public/default channel and no enrolled contacts.
    uint8_t key[16]{};key[0]=0x71;key[15]=0xc3;
    assert(cfg.setPrimaryName("Fixture"));
    mt::PrimaryChannel::Selection selected{};
    assert(mt::PrimaryChannel::normalize(key,sizeof(key),selected,cfg.effectivePrimaryName()));
    auto record=cfg.node();record.setKey(selected.hash,selected.bytes,selected.length);cfg.node(record);
    mt::PrimaryChannel::clear(selected);
    assert(f.service.apply(cfg)&&f.service.start(false,0)&&f.service.armEndpoint(true));
    assert(!f.service.phoneNodeInfoPing(900,0x123456));
    assert(f.service.phoneNodeInfoPing(901,0x123457)==-EAGAIN);
    f.service.poll(1200,2);assert(f.radio.frames.size()==1);
    const auto &frame=f.radio.frames.front();bench::RouteMetadata actual{};
    assert(f.service.codec.decode(frame.data(),frame.size(),actual)&&actual.portnum==meshtastic_PortNum_NODEINFO_APP);
    assert(bench::le32(frame.data())==UINT32_MAX&&bench::le32(frame.data()+4)==f.local.binding.node);
    assert(actual.signatureLength==64&&actual.publicKeyLength==32&&mt::Discovery::wantsResponse(actual));
    assert(!memcmp(actual.publicKey,f.local.binding.publicKey,32));
    bench::MtSignature verifier;assert(verifier.trustPublicKey(f.local.binding.node,f.local.binding.publicKey,900));
    assert(verifier.apply(frame.data(),frame.size(),actual,900)==bench::MtSignature::Result::Verified);
    puts("PASS interactive NodeInfo advertises only the enrolled public key on an isolated keyed primary with zero contacts");
  }
  {
    ZeroPeerFixture f;PeerKey peer(89);
    assert(f.service.beginPrivateAdministration());
    assert(f.ledger.addTrustedPeer(f.ledgerMemory,peer.identity.node,peer.identity.publicKey,true)==0);
    assert(f.service.endPrivateAdministration());
    PkiPeerPin pin{};pin.node=peer.identity.node;memcpy(pin.publicKey,peer.identity.publicKey,32);
    assert(f.runtime.adoptPersistedPeer(pin)&&f.runtime.ready(f.local.binding.node));
    Memory nodeMemory;mt::NodeJournal journal;
    assert(journal.load(nodeMemory,f.service.nodes,f.local.binding.node)==0);
    mt::NodeCache::Entry contact{};contact.identity(peer.identity.node,81,1);
    assert(mt::NodeCache::Entry::text(contact.bytes+mt::NodeCache::Entry::Name,40,"Public peer",12));
    assert(mt::NodeCache::Entry::text(contact.bytes+mt::NodeCache::Entry::Short,5,"PUB",4));
    contact.bytes[mt::NodeCache::Entry::KeyLength]=32;
    memcpy(contact.bytes+mt::NodeCache::Entry::Key,peer.identity.publicKey,32);
    assert(f.service.nodes.accept(contact,f.local.binding.node,-20,1)>=0);
    assert(f.service.receiving()&&f.service.endpointArmed());
    assert(mt::NodeAdmin::edit(f.service,f.radio,journal,nodeMemory,peer.identity.node,
                               mt::NodeCache::Edit::Remove,10)==0);
    assert(journal.generation()==1&&f.service.nodes.find(peer.identity.node)<0&&
           f.runtime.dormant(f.local.binding.node)&&f.runtime.publicReady(f.local.binding.node)&&
           !f.runtime.ready(f.local.binding.node)&&f.service.receiving()&&
           f.service.endpointArmed()&&f.radio.health()==1&&!f.service.faulted());
    puts("PASS final peer removal preserves healthy signed public-discovery RX and endpoint arm");
  }
  {
    ZeroPeerFixture f;
    assert(f.runtime.dormant(f.local.binding.node));
    assert(f.runtime.publicReady(f.local.binding.node));
    assert(!f.runtime.ready(f.local.binding.node));

    // Public NodeInfo remains signed and can transmit while the contact set is
    // empty. The endpoint's persistent TX nonce rejects reuse of that ID.
    assert(f.channelClock.set(0));
    f.service.poll(0,0x3456);
    assert(f.channelClock.set(30000));
    f.service.poll(30000,0x4567);
    assert(f.radio.frames.empty()); // Endpoint's node-info job is due 250 ms later.
    assert(f.channelClock.set(30250));
    f.service.poll(30250,0x4567);
    assert(f.radio.frames.size()==1);
    const Bytes tx=f.radio.frames.front();
    assert(tx.size()>=17 && bench::le32(tx.data()+4)==f.local.binding.node &&
           bench::le32(tx.data())==0xffffffffu && tx[13]!=0);
    bench::RouteMetadata decoded;
    assert(f.service.codec.decode(tx.data(),tx.size(),decoded));
    assert(decoded.channelDecoded&&decoded.portnum==meshtastic_PortNum_NODEINFO_APP&&
           decoded.signatureLength==64&&decoded.publicKeyLength==32&&
           !memcmp(decoded.publicKey,f.local.binding.publicKey,32));
    bench::MtSignature verifier;
    assert(verifier.trustPublicKey(f.local.binding.node,f.local.binding.publicKey,30000));
    assert(verifier.apply(tx.data(),tx.size(),decoded,30000)==bench::MtSignature::Result::Verified);
    const uint32_t publicId=bench::le32(tx.data()+8);
    // Finish the only in-flight RF send and enter the established storage
    // barrier before asking the nonce authority to reject a duplicate ID.
    assert(f.channelClock.set(30350));
    f.radio.completed=true;
    f.service.poll(30350,0x4567);
    assert(f.radio.sends==1&&f.service.receiving()&&f.service.endpointArmed());
    assert(f.service.beginPrivateAdministration());
    const unsigned writesAfterPublicTx=f.ledgerMemory.writes;
    bench::Frame duplicate{};
    auto node=f.service.config.node();
    assert(!f.runtime.bootstrap(f.service.codec,node,81,0xffffffffu,publicId,false,0,duplicate));
    assert(f.ledgerMemory.writes==writesAfterPublicTx);
    assert(f.service.endPrivateAdministration());
    assert(f.service.receiving()&&f.service.endpointArmed()&&!f.service.faulted());
    puts("PASS dormant runtime emits authenticated public NodeInfo and refuses reused global TX ID");
  }
  {
    ZeroPeerFixture f; PeerKey peer(89); SignEntropy peerSigning;
    const Bytes proof=signedNodeFrame(f,peer,peerSigning,44);
    f.service.rx(proof.data(),proof.size(),-35,1000,91,true);
    uint32_t candidateNode=0;uint8_t candidateKey[32]{};
    assert(f.service.peekVerifiedPeerCandidate(candidateNode,candidateKey));
    assert(candidateNode==peer.identity.node&&!memcmp(candidateKey,peer.identity.publicKey,32));
    assert(!f.ledger.lookup(peer.identity.node,candidateKey));
    assert(f.runtime.publicReady(f.local.binding.node)&&!f.runtime.ready(f.local.binding.node));
    const auto replayBefore=f.ledger.seen(peer.identity.node,peer.identity.publicKey,600);
    const unsigned writesBefore=f.ledgerMemory.writes;
    const Bytes encrypted=privateText(f,peer,600);
    f.service.rx(encrypted.data(),encrypted.size(),-30,1100,92,true);
    PrivateEvent event{};
    assert(!f.service.takePrivateEvent(0,encrypted.data(),encrypted.size(),event));
    assert(f.ledger.seen(peer.identity.node,peer.identity.publicKey,600)==ReplayCheck::UnknownPeer);
    assert(f.ledgerMemory.writes==writesBefore);
    PkiPeerPin notDurable{};notDurable.node=peer.identity.node;
    assert(!f.runtime.adoptPersistedPeer(notDurable));
    assert(f.runtime.publicReady(f.local.binding.node)&&!f.runtime.ready(f.local.binding.node));
    assert(replayBefore==ReplayCheck::UnknownPeer);

    // Proof is not trust. Only an explicit durable pin followed by runtime
    // publication enables authenticated private delivery.
    assert(f.service.beginPrivateAdministration());
    assert(f.ledger.addTrustedPeer(f.ledgerMemory,peer.identity.node,peer.identity.publicKey,true)==0);
    assert(f.service.endPrivateAdministration());
    PkiPeerPin pin{};pin.node=peer.identity.node;memcpy(pin.publicKey,peer.identity.publicKey,32);
    assert(f.runtime.adoptPersistedPeer(pin));
    assert(f.runtime.ready(f.local.binding.node)&&f.runtime.publicReady(f.local.binding.node));
    uint8_t out[32]{};assert(f.runtime.peerPublicIdentity(peer.identity.node,out));
    assert(!memcmp(out,peer.identity.publicKey,32));
    assert(f.service.peekVerifiedPeerCandidate(candidateNode,candidateKey));
    assert(candidateNode==peer.identity.node&&!memcmp(candidateKey,peer.identity.publicKey,32));

    f.service.rx(encrypted.data(),encrypted.size(),-30,1200,93,true);
    assert(f.service.takePrivateEvent(0,encrypted.data(),encrypted.size(),event));
    assert(event.from==peer.identity.node&&event.to==f.local.binding.node&&event.id==600&&
           event.size>0&&!memcmp(event.publicKey,peer.identity.publicKey,32));
    assert(f.ledger.seen(peer.identity.node,peer.identity.publicKey,600)==ReplayCheck::Seen);
    const unsigned committedWrites=f.ledgerMemory.writes;
    assert(!f.runtime.adoptPersistedPeer(pin));
    assert(f.ledgerMemory.writes==committedWrites);
    puts("PASS signed discovery stays non-authoritative until durable pin and explicit live adoption");
  }
  {
    ZeroPeerFixture f;
    f.signing.fail=true;
    f.service.poll(0,0x5566);
    f.service.poll(30000,0x6677);
    assert(f.radio.sends==0&&f.service.receiving()&&f.radio.health()==1&&
           f.service.endpointArmed()&&!f.service.faulted()&&f.runtime.publicReady(f.local.binding.node)&&
           !f.runtime.ready(f.local.binding.node)&&f.radio.stops>=1&&f.radio.restores>=1);
    puts("PASS failed public signature production restores dormant RX and does not open private trust");
  }
  {
    ZeroPeerFixture f;
    MultiPeerRuntime noSigner{f.crypto,f.backend,f.identity,f.nonce,f.replay,nullptr};
    assert(noSigner.open(f.local.binding.node,f.empty));
    assert(noSigner.dormant(f.local.binding.node));
    assert(!noSigner.publicReady(f.local.binding.node)&&!noSigner.ready(f.local.binding.node));
    const unsigned writes=f.ledgerMemory.writes;
    uint8_t identity[32]{};
    assert(!noSigner.publicIdentity(identity));
    assert(!noSigner.peerPublicIdentity(f.local.binding.node,identity));
    const auto node=f.service.config.node();
    bench::Frame frame{};
    assert(!noSigner.bootstrap(f.service.codec,node,81,0xffffffffu,77,false,0,frame));
    const uint8_t payload[]={'p','u','b','l','i','c'};
    assert(!noSigner.broadcast(f.service.codec,78,payload,sizeof(payload),frame));
    TestRadio blockedRadio;
    mt::Service blockedService{blockedRadio,true,f.local.binding.node,81,true,true,&noSigner};
    assert(!blockedService.start(false,0));
    assert(blockedRadio.sends==0&&f.ledgerMemory.writes==writes);
    crypto_wipe(identity,sizeof(identity));
    puts("PASS dormant zero-peer runtime without signer cannot start or publish");
  }
  {
    ZeroPeerFixture f;
    assert(f.runtime.publicReady(f.local.binding.node));
    // A corrupt persisted identity cannot be used to construct publicReady.
    Memory broken=f.identityMemory;
    auto &record=broken.values.at("mtidentity/state0");
    assert(!record.empty());record.back()^=0x80;
    IdentityJournal badIdentity;MonocypherIdentityDeriver badDeriver;
    assert(badIdentity.load(broken,badDeriver)<0);
    LedgerNonceAuthority badNonce{badIdentity,f.ledger,f.ledgerMemory,f.entropy,f.gate};
    MultiPeerRuntime badRuntime(f.crypto,f.backend,badIdentity,badNonce,f.replay,&f.signing);
    assert(!badRuntime.open(f.local.binding.node,f.empty));
    assert(!badRuntime.publicReady(f.local.binding.node));
    TestRadio badRadio;mt::Service badService(badRadio,true,f.local.binding.node,81,true,true,&badRuntime);
    assert(!badRuntime.publicReady(f.local.binding.node)&&!badService.start(false,0));
    puts("PASS corrupt identity journal never qualifies as public-ready");
  }
  puts("PASS M155 zero-peer public discovery and private-adoption boundary");
}
