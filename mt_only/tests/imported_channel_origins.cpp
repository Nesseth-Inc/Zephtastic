// SPDX-License-Identifier: GPL-3.0-only
// Channel restrictions are driven by the production boot hook. The radio,
// persisted fixture identity, contacts and encrypted packets are synthetic.
#include "pki_runtime_fixture.h"
#include "pki/MultiPeerRuntime.h"
#include "modules/BeaconTargetRuntime.h"
#include <algorithm>

namespace {
constexpr uint32_t Other=0x31415926;
const uint8_t privateChannelKey[16]={0x42,0x19};
void configure(Fixture &f,bool imported,bool secret,bool aead,bool relay=false) {
  f.s.stop();
  auto cfg=f.s.config;
  auto n=cfg.node();
  if(secret) {
    mt::PrimaryChannel::Selection key{};
    assert(mt::PrimaryChannel::normalize(privateChannelKey,sizeof(privateChannelKey),key));
    n.setKey(key.hash,key.bytes,key.length);
    mt::PrimaryChannel::clear(key);
  }else n.setPublic();
  cfg.node(n);assert(cfg.setPrimaryAead(aead));
  if(relay)assert(cfg.setDeviceRole(2,0));
  if(imported)f.s.restrictImportedChannelOrigins();
  assert(f.s.apply(cfg)&&f.s.start(relay,0)&&f.s.armEndpoint(true));
}
Bytes sharedFrame(bench::MtCodec &encoder,uint32_t from,uint32_t to,uint32_t id,uint8_t flags=0) {
  const auto payload=data("channel origin");
  uint8_t bytes[255]{};size_t size=0;
  assert(encoder.encodeZeroHopData(from,to,id,payload.data(),payload.size(),bytes,sizeof(bytes),size));
  bytes[12]=flags;return {bytes,bytes+size};
}
void selectedKey(const Fixture &f,bench::MtCodec &encoder) {
  mt::PrimaryChannel::Selection s;
  assert(f.s.config.channelSelection(0,s)&&encoder.setKey(s.hash,s.bytes,s.length,s.aead));
  mt::PrimaryChannel::clear(s);
}
void blockedOrigins(bool aead) {
  Fixture f(true,true);
  configure(f,true,true,aead);
  bench::MtCodec wire;selectedKey(f,wire);
  const auto local=f.local.binding.node,peer=f.peer.binding.node;
  uint8_t out[255]{};size_t size=1;
  assert(!f.s.codec.encodePositionQuery(local,peer,7,out,sizeof(out),size)&&!size);
  const auto raw=sharedFrame(wire,local,UINT32_MAX,17);
  const auto writes=f.lm.writes,stops=f.radio.stops;
  assert(f.s.submitMessage(0,raw.data(),raw.size(),false,50)==-EACCES);
  auto broadcast=f.request(18);broadcast.packet.to=UINT32_MAX;broadcast.packet.want_ack=false;
  input(f.c,broadcast,100);input(f.bc,broadcast,110);
  auto incoming=sharedFrame(wire,peer,local,91,8);
  bench::RouteMetadata m{};
  assert(f.s.codec.decode(incoming.data(),incoming.size(),m)&&m.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP);
  f.rx(incoming,1500);
  f.poll(2000);f.poll(31000);f.poll(61000);
  assert(f.radio.frames.empty()&&f.lm.writes==writes&&f.radio.stops==stops);
  mt::modules::BeaconTargetRuntime beacons(f.s);
  const auto target=beacons.runningProfile(f.s.config);
  assert(!beacons.targetSupported(target));
}
void allowedPublic() {
  Fixture f(true);configure(f,true,false,false);
  assert(f.s.codec.originAllowed());
  mt::modules::BeaconTargetRuntime beacons(f.s);
  assert(beacons.targetSupported(beacons.runningProfile(f.s.config)));
  const auto plain=data("open");
  auto good=f.request(33);good.packet.to=UINT32_MAX;good.packet.want_ack=false;
  input(f.bc,good,100);f.poll();assert(f.radio.frames.size()==1);
  bench::RouteMetadata m{};const auto &onAir=f.radio.frames.front();
  assert(f.s.codec.decode(onAir.data(),onAir.size(),m)&&m.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP);
  // Deliberately collide with the active default's hash and change the key.
  const auto hash=f.s.codec.channelHash();
  assert(f.s.codec.setKey(hash,privateChannelKey,sizeof(privateChannelKey))&&!f.s.codec.originAllowed());
  assert(f.s.codec.setLicensedPlaintext(hash)&&f.s.codec.originAllowed());
  uint8_t out[255]{};size_t size=0;
  assert(f.s.codec.encodeZeroHopData(f.local.binding.node,UINT32_MAX,34,plain.data(),plain.size(),out,sizeof(out),size));
  assert(size==16+plain.size()&&std::equal(plain.begin(),plain.end(),out+16));
  f.runtime.freeze();
  assert(!f.s.codec.originAllowed());
  assert(f.s.codec.setKey(hash,bench::MtNodeRecord::PublicKey,sizeof(bench::MtNodeRecord::PublicKey))&&!f.s.codec.originAllowed());
}
void publicChannelWithNoTrustedContacts() {
  Fixture fixture(true);
  // A separate, empty authority models the first successful imported boot.
  Memory memory;
  PkiLedger ledger;
  Gate gate;
  assert(ledger.load(memory) == 0);
  const auto &local = fixture.local.binding;
  assert(ledger.initialize(memory, local.publicKey, local.node, true) == 0);
  LedgerNonceAuthority nonces(fixture.identity, ledger, memory, fixture.entropy, gate);
  LedgerReplayAuthority peers(fixture.identity, ledger, memory, gate);
  MultiPeerRuntime runtime(fixture.crypto, fixture.backend, fixture.identity, nonces, peers, &fixture.signing);
  PkiPeerSet empty;
  assert(empty.assignEmpty(local) && runtime.open(local.node, empty));
  assert(!runtime.ready(local.node) && runtime.publicReady(local.node));
  Radio radio;
  mt::Service service(radio, true, local.node, 81, true, false, &runtime);
  gate.service = &service;
  service.restrictImportedChannelOrigins();
  assert(service.apply(fixture.s.config) && service.start(false, 0) && service.armEndpoint(true));
  assert(service.codec.originAllowed());
  mt::modules::BeaconTargetRuntime beacons(service);
  assert(beacons.targetSupported(beacons.runningProfile(service.config)));
  const auto text = data("public before peer enrollment");
  uint8_t frame[255]{};
  size_t size = 0;
  assert(service.codec.encodeZeroHopData(local.node, UINT32_MAX, 123, text.data(), text.size(),
                                        frame, sizeof(frame), size));
  assert(service.codec.setKey(0, privateChannelKey, sizeof(privateChannelKey)) && !service.codec.originAllowed());
  assert(service.keyVerificationStageCandidate(fixture.peer.binding.node, fixture.peer.binding.publicKey) == -EPERM);
}

void signedDiscovery(bool imported) {
  Fixture f(true);configure(f,imported,false,false);
  Key unknown(131);Memory storage;
  IdentityJournal identity;MonocypherIdentityDeriver deriver;
  IdentityCandidate candidate(unknown.binding,unknown.scalar);
  assert(!identity.load(storage,deriver)&&!identity.store(storage,deriver,candidate,true));
  KeyRef ref;IdentityBinding binding;
  assert(identity.binding(binding,ref));
  NodeInfoProfile profile;profile.hardwareModel=81;
  strcpy(profile.longName,"Synthetic test");strcpy(profile.shortName,"TEST");
  SigningEnvelope envelope;envelope.hasBitfield=true;
  SignedData signedData;
  const auto to=f.local.binding.node;
  assert(PkiSigner::bootstrap(identity,ref,binding,f.signing,57,to,profile,envelope,signedData)==SigningResult::Ok);
  bench::MtCodec emitter;selectedKey(f,emitter);
  uint8_t frame[255]{};size_t n=0;
  assert(emitter.encodeSignedNodeInfo(binding.node,to,57,signedData.bytes,signedData.size,frame,sizeof(frame),n));
  f.s.rx(frame,n,2,100,0);
  uint8_t verified[32]{};
  assert(f.s.signature.publicIdentity(binding.node,100,verified)&&!memcmp(verified,binding.publicKey,32));
  uint32_t pending=0;
  assert(f.s.peekVerifiedPeerCandidate(pending,verified)==!imported);
  if(!imported)assert(pending==binding.node&&!memcmp(verified,binding.publicKey,32));
}

void privateAndTransit() {
  Fixture f(true);configure(f,true,true,false,true);
  bench::MtCodec wire;selectedKey(f,wire);
  const auto incoming=sharedFrame(wire,f.peer.binding.node,Other,201,0x63);
  f.rx(incoming,1500);assert(f.s.route.pending()==1);f.poll(3000);
  assert(f.radio.frames.size()==1);
  const auto &forwarded=f.radio.frames.back();
  assert(forwarded.size()==incoming.size()&&std::equal(incoming.begin(),incoming.begin()+12,forwarded.begin())&&
         std::equal(incoming.begin()+16,incoming.end(),forwarded.begin()+16));
  f.complete(3200);
  auto dm=f.request(210);dm.packet.pki_encrypted=true;
  input(f.c,dm,4000);f.poll(4300);
  assert(f.radio.frames.size()==2);
  const auto &air=f.radio.frames.back();
  assert(bench::le32(air.data())==f.peer.binding.node&&bench::le32(air.data()+8)==210&&air[13]==0);
  uint8_t clear[227]{};size_t length=0;
  assert(f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,f.local.binding.node,210,air.data()+16,air.size()-16,
                          clear,sizeof(clear),length));
  bench::RouteMetadata m{};
  assert(bench::MtCodec::decodeData(clear,length,m)&&m.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP);
  f.complete(4500);
  assert(f.s.keyVerificationStageCandidate(Other,f.peer.binding.publicKey)==-EPERM);
  assert(f.s.keyVerificationStageCandidate(f.peer.binding.node,f.local.binding.publicKey)==-EPERM);
  assert(f.s.submitKeyVerification(0,Other,0,nullptr,0,true,nullptr,0,1,false,5000,11)==-EPERM);
  assert(f.s.submitKeyVerification(0,Other,0,nullptr,0,false,nullptr,0,1,false,5000,12)==-EPERM);
}
}
int main() {
  for(bool aead:{false,true})blockedOrigins(aead);
  std::cout<<"PASS imported confidential CTR/CCM refuse both companion transports, automatic responses, NodeInfo, raw admission and alternate beacons without nonce writes\n";
  allowedPublic();
  publicChannelWithNoTrustedContacts();
  std::cout<<"PASS public channels need a usable local signing identity, even with no contacts; a colliding secret key is denied\n";
  signedDiscovery(false);signedDiscovery(true);
  std::cout<<"PASS the same signed NodeInfo is verified in both modes and eligible for automatic pinning only under an ordinary identity\n";
  privateAndTransit();
  std::cout<<"PASS byte-preserving transit and a decryptable pinned private DM survive the import restriction; unpinned control is refused\n";
  Fixture ordinary(true);configure(ordinary,false,true,false);
  uint8_t out[255]{};size_t size=0;
  assert(ordinary.s.encodePositionQuery(ordinary.peer.binding.node,501,out,sizeof(out),size)&&size>16);
  std::cout<<"PASS ordinary identity retains confidential channel origination\n";
}
