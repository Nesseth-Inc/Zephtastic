// SPDX-License-Identifier: GPL-3.0-only
// Synthetic public keys only. Exercises the real signed NodeInfo and channel
// encryption path through Service::rx; no trust verifier is stubbed.
#include "Service.h"
#include "known_channel_fixture.h"
#include "pki/PkiSigner.h"
#include "pki/PsaPkiBackend.h"
#include "monocypher.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <vector>
extern "C" psa_status_t mbedtls_psa_external_get_random(
    mbedtls_psa_external_random_context_t *,uint8_t *,size_t,size_t *length) {
  *length=0;return PSA_ERROR_INSUFFICIENT_ENTROPY;
}
using namespace mt::pki;
struct Provider final:KeyProvider {
  uint8_t scalar[32]{};IdentityBinding binding{};KeyRef ref{0,1};
  explicit Provider(unsigned seed) {
    for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed*29+i*17+3);
    scalar[0]&=0xf8;scalar[31]=(scalar[31]&0x7f)|0x40;
    crypto_x25519_public_key(binding.publicKey,scalar);
    binding.node=bench::MtSignature::nodeId(binding.publicKey);binding.generation=1;
  }
  ~Provider(){crypto_wipe(scalar,sizeof(scalar));}
  bool active(KeyRef r)const override{return r.slot==ref.slot&&r.generation==ref.generation;}
  bool withPrivate(KeyRef r,PrivateKeyOperation &op)override{return active(r)&&op.apply(scalar);}
};
struct Entropy final:SigningEntropy {
  uint8_t seed=0x5b;
  bool hedge(uint8_t out[32])override{for(unsigned i=0;i<32;++i)out[i]=uint8_t(seed+13*i);++seed;return true;}
};
struct Radio final:mt::Radio {
  int healthValue=0;bool busyValue=false;
  bool ready()override{return true;}int start()override{healthValue=1;return 0;}
  int restore()override{healthValue=1;return 0;}int stop()override{healthValue=0;return 0;}
  int health()override{return healthValue;}bool busy()override{return busyValue;}
  uint32_t airtime(size_t)override{return 100;}int send(uint8_t *,size_t)override{return 0;}
  bool completion(int &r)override{r=0;return false;}
};
struct Receiver {
  Provider self;Radio radio;mt::test::KnownChannelFixtureClock clock;
  mt::Service service;
  explicit Receiver(unsigned seed):self(seed),service(radio,true,self.binding.node,81,true,false) {
    clock.bind(service);mt::Config config;config.bytes[4]=1;auto node=config.node();node.setPublic();config.node(node);
    assert(service.apply(config));assert(service.start(false,0));
  }
  ~Receiver(){service.stop();}
  void receive(const std::vector<uint8_t> &frame,uint32_t now=1000,bool radioRx=true) {
    service.rx(frame.data(),frame.size(),-42,now,0x1234,radioRx);
  }
  bool candidate(uint32_t &node,uint8_t key[32]) {return service.peekVerifiedPeerCandidate(node,key);}
  bool take(uint32_t &node,uint8_t key[32]) {return service.takeVerifiedPeerCandidate(node,key);}
};
static SignedData signedNode(Provider &provider,Entropy &entropy,uint32_t to,uint32_t id) {
  NodeInfoProfile profile;strcpy(profile.longName,"Synthetic verified peer");strcpy(profile.shortName,"M155");profile.hardwareModel=81;
  SigningEnvelope envelope;envelope.hasBitfield=true;envelope.bitfield=0;
  SignedData out;assert(PkiSigner::bootstrap(provider,provider.ref,provider.binding,entropy,id,to,profile,envelope,out)==SigningResult::Ok);return out;
}
static std::vector<uint8_t> encryptedNode(Receiver &rx,const SignedData &signedData,uint32_t fromOverride=0) {
  uint8_t frame[255]{};size_t size=0;const uint32_t from=fromOverride?fromOverride:signedData.from;
  assert(rx.service.codec.encodeSignedNodeInfo(from,signedData.to,signedData.id,signedData.bytes,signedData.size,frame,sizeof(frame),size));
  return {frame,frame+size};
}
static std::vector<uint8_t> encryptedUnsignedNode(Receiver &rx,Provider &provider,Entropy &entropy,uint32_t id) {
  auto signedData=signedNode(provider,entropy,0xffffffffu,id);meshtastic_Data data{};
  auto in=pb_istream_from_buffer(signedData.bytes,signedData.size);assert(pb_decode(&in,meshtastic_Data_fields,&data));
  uint8_t frame[255]{};size_t size=0;
  assert(rx.service.codec.encodeZeroHopNodeInfo(signedData.from,id,data.payload.bytes,data.payload.size,frame,sizeof(frame),size));
  return {frame,frame+size};
}
static std::vector<uint8_t> badSignatureNode(Receiver &rx,Provider &provider,Entropy &entropy,uint32_t to,uint32_t id) {
  auto signedData=signedNode(provider,entropy,to,id);meshtastic_Data data{};
  auto in=pb_istream_from_buffer(signedData.bytes,signedData.size);assert(pb_decode(&in,meshtastic_Data_fields,&data));
  assert(data.xeddsa_signature.size==64);data.xeddsa_signature.bytes[17]^=0x80;
  auto out=pb_ostream_from_buffer(signedData.bytes,sizeof(signedData.bytes));assert(pb_encode(&out,meshtastic_Data_fields,&data));signedData.size=out.bytes_written;
  return encryptedNode(rx,signedData);
}
static std::vector<uint8_t> signedText(Receiver &rx,Provider &provider,Entropy &entropy,uint32_t id) {
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;data.payload.size=4;memcpy(data.payload.bytes,"test",4);data.has_bitfield=true;
  SignedData signedData;assert(PkiSigner::sign(provider,provider.ref,provider.binding,entropy,id,0xffffffffu,data,signedData)==SigningResult::Ok);
  uint8_t frame[255]{};size_t size=0;assert(rx.service.codec.encodeSignedApplication(signedData.from,id,signedData.bytes,signedData.size,frame,sizeof(frame),size));
  return {frame,frame+size};
}
static void noCandidate(Receiver &rx) {
  uint32_t node=0xdeadbeef;uint8_t key[32];memset(key,0xa5,sizeof(key));
  assert(!rx.candidate(node,key)&&node==0);for(uint8_t b:key)assert(!b);
  node=0xdeadbeef;memset(key,0xa5,sizeof(key));assert(!rx.take(node,key)&&node==0);for(uint8_t b:key)assert(!b);
}
int main() {
  {Receiver rx(11);Provider peer(21);Entropy entropy;const auto wire=encryptedNode(rx,signedNode(peer,entropy,rx.self.binding.node,1));
   rx.receive(wire);uint32_t node=0;uint8_t key[32]{};assert(rx.candidate(node,key));assert(node==peer.binding.node&&!memcmp(key,peer.binding.publicKey,32));
   memset(key,0,sizeof(key));assert(rx.take(node,key));assert(node==peer.binding.node&&!memcmp(key,peer.binding.publicKey,32));noCandidate(rx);
   puts("PASS actual Service RX stages only signed+channel-encrypted NodeInfo key proof");}
  {Receiver rx(12);Provider unsignedPeer(22),badPeer(23),mismatchPeer(24),quarantinePeer(25);Entropy e1,e2,e3,e4;
   auto unsignedWire=encryptedUnsignedNode(rx,unsignedPeer,e1,2);rx.receive(unsignedWire);
   auto badWire=badSignatureNode(rx,badPeer,e2,rx.self.binding.node,3);rx.receive(badWire);
   // Keep signed bytes/key A, but encrypt under a different frame origin B.
   auto mismatchSigned=signedNode(mismatchPeer,e3,rx.self.binding.node,4);
   auto mismatchWire=encryptedNode(rx,mismatchSigned,badPeer.binding.node);rx.receive(mismatchWire);
   auto quarantineWire=signedText(rx,quarantinePeer,e4,5);rx.receive(quarantineWire);
   noCandidate(rx);}
  {Receiver rx(13);Provider first(31),other(32);Entropy e1,e2;auto one=signedNode(first,e1,rx.self.binding.node,6);auto repeat=signedNode(first,e1,rx.self.binding.node,7);auto conflict=signedNode(other,e2,rx.self.binding.node,8);
   rx.receive(encryptedNode(rx,one));rx.receive(encryptedNode(rx,repeat));rx.receive(encryptedNode(rx,conflict));
   uint32_t node=0;uint8_t key[32]{};assert(rx.candidate(node,key)&&node==first.binding.node&&!memcmp(key,first.binding.publicKey,32));
   puts("PASS repeated proof coalesces and conflicting candidate cannot replace first");}
  {Receiver rx(14);Provider ignored(41);Entropy e;const auto wire=encryptedNode(rx,signedNode(ignored,e,rx.self.binding.node,9));
   assert(rx.service.nodes.edit(ignored.binding.node,rx.self.binding.node,mt::NodeCache::Edit::Ignore)==1);rx.receive(wire);noCandidate(rx);}
  {Receiver rx(15);Entropy e;const auto wire=encryptedNode(rx,signedNode(rx.self,e,0xffffffffu,10));rx.receive(wire);noCandidate(rx);}
  {Receiver rx(16);Provider peer(51);Entropy e;rx.receive(encryptedNode(rx,signedNode(peer,e,rx.self.binding.node,11)));rx.service.clearVerifiedPeerCandidate();noCandidate(rx);
   rx.receive(encryptedNode(rx,signedNode(peer,e,rx.self.binding.node,12)));rx.radio.healthValue=-EIO;rx.service.rx(nullptr,0,0,2000,0);assert(rx.service.faulted());noCandidate(rx);}
  {Receiver rx(17);Provider peer(61);Entropy e;rx.receive(encryptedNode(rx,signedNode(peer,e,rx.self.binding.node,13)));rx.service.stop();noCandidate(rx);}
  puts("PASS candidate rejection, first-proof retention, clear/fault/stop wiping; no hardware");
}
