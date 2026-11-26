// SPDX-License-Identifier: GPL-3.0-only
// All fixture private scalars are deterministic PUBLIC bytes, host-only.
#include "MtSignature.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "ErriezCRC32.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" size_t mtPinnedSigningBuffer(uint32_t,uint32_t,uint32_t,const bench::RouteMetadata *,uint8_t *,size_t);
extern "C" void mtPinnedCurveToEd(const uint8_t[32],uint8_t[32]);
extern "C" bool mtPinnedSignatureVerify(const uint8_t[32],const uint8_t[64],const uint8_t *,size_t);
extern "C" void mtPinnedFixtureKey(unsigned,uint8_t[32],uint8_t[32],uint8_t[32]);
extern "C" void mtPinnedFixtureSign(const uint8_t[32],const uint8_t[32],const uint8_t *,size_t,uint8_t[64]);
using bench::MtSignature;
using bench::RouteMetadata;
static unsigned checks=0,signatures=0;
#define CHECK(expr) do {++checks; if(!(expr)) {std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#expr); std::abort();}} while(0)

struct Fixture {
  uint8_t curve[32]{},edSecret[32]{},ed[32]{};
  explicit Fixture(unsigned seed) {mtPinnedFixtureKey(seed,curve,edSecret,ed);}
  ~Fixture() {crypto_wipe(edSecret,sizeof(edSecret));}
};
using Header=std::array<uint8_t,16>;
static void put32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static uint32_t get32(const uint8_t *p) {return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static Header header(uint32_t from,uint32_t id=0x10203040,uint32_t to=0xffffffffu) {
  Header h{};put32(h.data(),to);put32(h.data()+4,from);put32(h.data()+8,id);
  h[12]=0x23;h[13]=0xa7;h[14]=0x35;h[15]=0x63;return h;
}
static RouteMetadata nodeInfo(const Fixture &f) {
  RouteMetadata m;m.channelDecoded=true;m.portnum=4;m.nodeInfoValid=true;
  m.publicKeyLength=32;memcpy(m.publicKey,f.curve,32);
  // User(long_name="fixture",public_key). No private data in the payload.
  const uint8_t name[]={0x12,7,'f','i','x','t','u','r','e',0x42,32};
  memcpy(m.payload,name,sizeof(name));memcpy(m.payload+sizeof(name),f.curve,32);
  m.payloadLength=uint8_t(sizeof(name)+32);return m;
}
static RouteMetadata text() {
  RouteMetadata m;m.channelDecoded=true;m.portnum=1;m.payloadLength=4;
  memcpy(m.payload,"test",4);return m;
}
static void sign(RouteMetadata &m,const Header &h,const Fixture &f) {
  uint8_t ours[267]{},pinned[267]{};
  const size_t n=MtSignature::signingBuffer(get32(h.data()+4),get32(h.data()+8),get32(h.data()),m,ours,sizeof(ours));
  const size_t ref=mtPinnedSigningBuffer(get32(h.data()+4),get32(h.data()+8),get32(h.data()),&m,pinned,sizeof(pinned));
  CHECK(n==ref && n>=34 && !memcmp(ours,pinned,n));
  mtPinnedFixtureSign(f.edSecret,f.ed,pinned,n,m.signature);m.signatureLength=64;++signatures;
  CHECK(mtPinnedSignatureVerify(f.curve,m.signature,pinned,n));
  CHECK(crypto_ed25519_check(m.signature,f.ed,pinned,n)==0);
}
static MtSignature::Result apply(MtSignature &verifier,const Header &h,RouteMetadata &m,uint32_t now=10) {
  // Only the 16-byte header is read; caller's packet length includes payload.
  std::array<uint8_t,17> frame{};memcpy(frame.data(),h.data(),16);
  return verifier.apply(frame.data(),frame.size(),m,now);
}
static void varint(std::vector<uint8_t> &v,uint64_t x) {
  do {uint8_t b=uint8_t(x&127);x>>=7;v.push_back(uint8_t(b|(x?128:0)));}while(x);
}
static void fixed(std::vector<uint8_t> &v,uint8_t tag,uint32_t value) {
  v.push_back(uint8_t((tag<<3)|5));for(unsigned i=0;i<4;++i)v.push_back(uint8_t(value>>(8*i)));
}
static std::vector<uint8_t> serialize(const RouteMetadata &m) {
  std::vector<uint8_t> v={0x08};varint(v,m.portnum);v.push_back(0x12);
  varint(v,m.payloadLength);v.insert(v.end(),m.payload,m.payload+m.payloadLength);
  v.push_back(0x18);varint(v,m.wantResponse);
  fixed(v,6,m.request_id);fixed(v,7,m.reply_id);fixed(v,8,m.emoji);
  if(m.hasBitfield) {v.push_back(0x48);varint(v,m.bitfield);}
  v.push_back(0x52);varint(v,m.signatureLength);
  v.insert(v.end(),m.signature,m.signature+m.signatureLength);return v;
}

static void envelopeAndCrypto() {
  for(unsigned i=0;i<64;++i) {
    Fixture f(i+1);uint8_t converted[32]{},pinned[32]{};
    CHECK(MtSignature::admissiblePublicKey(f.curve,converted));
    mtPinnedCurveToEd(f.curve,pinned);
    CHECK(!memcmp(converted,pinned,32) && !memcmp(converted,f.ed,32));
    CHECK(MtSignature::nodeId(f.curve)==crc32Buffer(f.curve,32));
    RouteMetadata m=text();m.request_id=i*0x123456u;m.reply_id=~m.request_id;
    m.emoji=i*0x7654u;m.hasBitfield=(i&1)!=0;m.bitfield=uint8_t(i*3u);m.wantResponse=(i&2)!=0;
    m.payloadLength=uint8_t(i*3u);for(unsigned j=0;j<m.payloadLength;++j)m.payload[j]=uint8_t(i+j);
    const Header h=header(MtSignature::nodeId(f.curve),i+1);
    sign(m,h,f);
    MtSignature verifier;CHECK(verifier.trustPublicKey(get32(h.data()+4),f.curve,0));
    CHECK(apply(verifier,h,m)==MtSignature::Result::Verified && m.signatureVerified);
    // Every signed field independently changes the verified message.
    for(unsigned mutation=0;mutation<12;++mutation) {
      auto changed=m;auto changedHeader=h;
      switch(mutation) {
      case 0:changedHeader[0]^=2;break;
      case 1:changedHeader[4]^=2;break;
      case 2:changedHeader[8]^=2;break;
      case 3:changed.portnum=5;break;
      case 4:changed.request_id^=1;break;
      case 5:changed.reply_id^=1;break;
      case 6:changed.emoji^=1;break;
      case 7:changed.hasBitfield=!changed.hasBitfield;break;
      case 8:changed.hasBitfield=true;changed.bitfield^=0x80;break;
      case 9:changed.wantResponse=!changed.wantResponse;break;
      case 10:if(changed.payloadLength)changed.payload[0]^=1;else {changed.payloadLength=1;changed.payload[0]=1;}break;
      default:changed.signature[0]^=1;break;
      }
      const auto result=apply(verifier,changedHeader,changed);
      CHECK(result==MtSignature::Result::Rejected||result==MtSignature::Result::Quarantined);
      CHECK(!changed.signatureVerified);
      uint8_t data[267]{};const size_t n=mtPinnedSigningBuffer(get32(changedHeader.data()+4),get32(changedHeader.data()+8),
          get32(changedHeader.data()),&changed,data,sizeof(data));
      CHECK(!mtPinnedSignatureVerify(f.curve,changed.signature,data,n));
    }
    // Mutable relay/hop/channel fields are outside the signature by design.
    Header mutableHeader=h;mutableHeader[12]^=0x37;mutableHeader[13]^=2;mutableHeader[14]^=1;mutableHeader[15]^=1;
    auto unchanged=m;CHECK(apply(verifier,mutableHeader,unchanged)==MtSignature::Result::Verified);
  }
  std::puts("PASS 64 actual-source signing/conversion/XEdDSA vectors + 768 signed-field mutations");
}
static void nodeBootstrapAndCodec() {
  Fixture f(91),other(92);MtSignature verifier;
  const auto h=header(MtSignature::nodeId(f.curve));auto m=nodeInfo(f);sign(m,h,f);
  const auto encoded=serialize(m);RouteMetadata parsed;
  CHECK(bench::MtCodec::decodeData(encoded.data(),encoded.size(),parsed));
  CHECK(parsed.portnum==4&&parsed.nodeInfoValid&&parsed.publicKeyLength==32&&parsed.signatureLength==64);
  CHECK(!memcmp(parsed.payload,m.payload,m.payloadLength)&&!memcmp(parsed.signature,m.signature,64));
  CHECK(!parsed.channelDecoded&&!parsed.signatureVerified);
  // Public deterministic channel fixture, not a network key.
  const uint8_t channelKey[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
  bench::MtCodec codec;CHECK(codec.setKey(h[13],channelKey,sizeof(channelKey)));
  std::vector<uint8_t> frame(h.begin(),h.end());frame.insert(frame.end(),encoded.begin(),encoded.end());
  uint8_t iv[16]{};bench::MtCodec::nonce(get32(h.data()+4),get32(h.data()+8),iv);
  CHECK(bench::mtAesCtr(nullptr,channelKey,16,iv,frame.data()+16,encoded.size()));
  CHECK(codec.decode(frame.data(),frame.size(),parsed)&&parsed.channelDecoded);
  CHECK(verifier.apply(frame.data(),frame.size(),parsed,10)==MtSignature::Result::Verified);
  CHECK(verifier.keyCount(10)==1&&parsed.signatureVerified);

  auto bad=nodeInfo(other);memcpy(bad.publicKey,f.curve,32);sign(bad,h,other);
  CHECK(apply(verifier,h,bad)==MtSignature::Result::Rejected);CHECK(verifier.keyCount(10)==1);
  auto conflict=nodeInfo(f);memcpy(conflict.publicKey,other.curve,32);sign(conflict,h,f);
  CHECK(apply(verifier,h,conflict)==MtSignature::Result::Rejected);
  auto valid=text();sign(valid,h,f);CHECK(apply(verifier,h,valid)==MtSignature::Result::Verified);
  auto unsignedPacket=text();unsignedPacket.signatureVerified=true;unsignedPacket.signatureRejected=true;
  CHECK(apply(verifier,h,unsignedPacket)==MtSignature::Result::Unsigned);
  CHECK(!unsignedPacket.signatureVerified&&!unsignedPacket.signatureRejected&&!unsignedPacket.signatureQuarantined);
  MtSignature empty;auto unknown=text();sign(unknown,h,f);
  CHECK(apply(empty,h,unknown)==MtSignature::Result::Quarantined&&unknown.signatureQuarantined);
  auto wrongId=nodeInfo(f);auto badHeader=h;badHeader[4]^=1;sign(wrongId,badHeader,f);
  CHECK(apply(empty,badHeader,wrongId)==MtSignature::Result::Rejected&&empty.keyCount(10)==0);
  auto missing=nodeInfo(f);missing.publicKeyLength=31;sign(missing,h,f);
  CHECK(apply(empty,h,missing)==MtSignature::Result::Rejected);
  for(uint8_t len:{uint8_t(1),uint8_t(63),uint8_t(65)}) {
    auto partial=text();partial.signatureLength=len;
    CHECK(apply(empty,h,partial)==MtSignature::Result::Rejected);
  }
  // Retain original optional presence/response and last singular values.
  auto envelope=text();envelope.emoji=0xf1234567;envelope.wantResponse=true;envelope.hasBitfield=true;
  envelope.bitfield=2;envelope.request_id=0xeaf0aafe;envelope.reply_id=0xdedadada;
  sign(envelope,h,f);auto bytes=serialize(envelope);
  CHECK(bench::MtCodec::decodeData(bytes.data(),bytes.size(),parsed));
  CHECK(parsed.emoji==envelope.emoji&&parsed.wantResponse&&parsed.hasBitfield&&parsed.bitfield==2);
  CHECK(parsed.request_id==envelope.request_id&&parsed.reply_id==envelope.reply_id);
  parsed.channelDecoded=true;CHECK(apply(verifier,h,parsed)==MtSignature::Result::Verified);
  bytes.push_back(0x18);bytes.push_back(0); // duplicate want_response: last wins
  CHECK(bench::MtCodec::decodeData(bytes.data(),bytes.size(),parsed)&&!parsed.wantResponse);
  parsed.channelDecoded=true;CHECK(apply(verifier,h,parsed)==MtSignature::Result::Rejected);
  std::puts("PASS first-contact NodeInfo, encrypted codec envelope, conflicts and malformed/unknown signatures");
}
static void provisionAndBounds() {
  Fixture f(104),other(105);MtSignature verifier;
  const auto legacy=header(0x76543210);CHECK(MtSignature::nodeId(f.curve)!=get32(legacy.data()+4));
  CHECK(verifier.trustPublicKey(get32(legacy.data()+4),f.curve,0));
  auto node=nodeInfo(f);sign(node,legacy,f);CHECK(apply(verifier,legacy,node)==MtSignature::Result::Verified);
  RouteMetadata ack;ack.channelDecoded=true;ack.portnum=5;ack.payloadLength=2;ack.payload[0]=0x18;
  ack.request_id=0x88776655;sign(ack,legacy,f);CHECK(apply(verifier,legacy,ack)==MtSignature::Result::Verified);
  CHECK(!verifier.trustPublicKey(get32(legacy.data()+4),other.curve,10));
  CHECK(apply(verifier,legacy,ack)==MtSignature::Result::Verified);
  for(uint32_t invalid:{0u,1u,0xffffffffu})CHECK(!verifier.trustPublicKey(invalid,f.curve,10));
  CHECK(!verifier.trustPublicKey(2,nullptr,10));
  MtSignature wrong;CHECK(wrong.trustPublicKey(get32(legacy.data()+4),other.curve,0));
  CHECK(apply(wrong,legacy,ack)==MtSignature::Result::Rejected);
  CHECK(verifier.keyCount(10+MtSignature::KeyTtlMs-1)==1);
  CHECK(verifier.keyCount(10+MtSignature::KeyTtlMs)==0);
  CHECK(apply(verifier,legacy,ack,10+MtSignature::KeyTtlMs)==MtSignature::Result::Quarantined);
  CHECK(verifier.trustPublicKey(get32(legacy.data()+4),f.curve,10+MtSignature::KeyTtlMs));
  CHECK(apply(verifier,legacy,ack,11+MtSignature::KeyTtlMs)==MtSignature::Result::Verified);
  verifier.clear();CHECK(verifier.keyCount(11+MtSignature::KeyTtlMs)==0);
  CHECK(verifier.trustPublicKey(get32(legacy.data()+4),f.curve,0xfffffff0u));
  CHECK(verifier.keyCount(0x10)==1);
  CHECK(apply(verifier,legacy,ack,0x10)==MtSignature::Result::Verified);
  CHECK(verifier.keyCount(0x10+MtSignature::KeyTtlMs)==0);

  MtSignature full;
  for(unsigned i=0;i<MtSignature::Capacity;++i)CHECK(full.trustPublicKey(100+i,f.curve,0));
  CHECK(full.keyCount(1)==32&&!full.trustPublicKey(200,other.curve,1));
  auto bootstrap=nodeInfo(other);const auto fresh=header(MtSignature::nodeId(other.curve));sign(bootstrap,fresh,other);
  CHECK(apply(full,fresh,bootstrap,1)==MtSignature::Result::Rejected&&full.keyCount(1)==32);
  CHECK(!full.trustPublicKey(100,other.curve,1));
  CHECK(full.trustPublicKey(200,other.curve,MtSignature::KeyTtlMs));
  CHECK(full.keyCount(MtSignature::KeyTtlMs)==1);
  auto noDecode=ack;noDecode.channelDecoded=false;CHECK(apply(verifier,legacy,noDecode)==MtSignature::Result::Rejected);
  auto fatal=ack;fatal.backendFailed=true;CHECK(apply(verifier,legacy,fatal)==MtSignature::Result::Rejected);
  auto pki=legacy;pki[13]=0;put32(pki.data(),0x11112222);
  CHECK(apply(verifier,pki,ack)==MtSignature::Result::Rejected);
  std::puts("PASS explicit legacy public trust, receipts, capacity/no-eviction, TTL/wrap and PKI guards");
}
static void weakAndMalleable() {
  uint8_t ed[32]{},weak[32]{};
  CHECK(!MtSignature::admissiblePublicKey(weak,ed));weak[0]=1;
  CHECK(!MtSignature::admissiblePublicKey(weak,ed));
  memset(weak,0xff,32);weak[31]=0x7f;
  for(uint8_t low:{uint8_t(0xec),uint8_t(0xed),uint8_t(0xee)}) {
    weak[0]=low;CHECK(!MtSignature::admissiblePublicKey(weak,ed));
  }
  Fixture f(114);memcpy(weak,f.curve,32);weak[31]|=0x80;
  CHECK(!MtSignature::admissiblePublicKey(weak,ed));
  // Actual upstream conversion masks the high bit; admission deliberately does not.
  uint8_t a[32],b[32];mtPinnedCurveToEd(f.curve,a);mtPinnedCurveToEd(weak,b);
  CHECK(!memcmp(a,b,32));
  const uint8_t order8[2][32]={
    {0xe0,0xeb,0x7a,0x7c,0x3b,0x41,0xb8,0xae,0x16,0x56,0xe3,0xfa,0xf1,0x9f,0xc4,0x6a,
     0xda,0x09,0x8d,0xeb,0x9c,0x32,0xb1,0xfd,0x86,0x62,0x05,0x16,0x5f,0x49,0xb8,0x00},
    {0x5f,0x9c,0x95,0xbc,0xa3,0x50,0x8c,0x24,0xb1,0xd0,0xb1,0x55,0x9c,0x83,0xef,0x5b,
     0x04,0x44,0x5c,0xc4,0x58,0x1c,0x8e,0x86,0xd8,0x22,0x4e,0xdd,0xd0,0x9f,0x11,0x57}};
  for(const auto &key:order8)CHECK(!MtSignature::admissiblePublicKey(key,ed));
  // Twist points are valid X25519 inputs but cannot be Ed25519 signing keys.
  unsigned rejectedTwist=0;
  for(unsigned u=2;u<32;++u) {
    uint8_t curve[32]{},converted[32]{};curve[0]=uint8_t(u);
    crypto_x25519_to_eddsa(converted,curve);
    const uint8_t eq[64]={1},challenge[32]={0};
    if(crypto_eddsa_check_equation(eq,converted,challenge)!=0) {
      ++rejectedTwist;CHECK(!MtSignature::admissiblePublicKey(curve,ed));
    }
  }
  CHECK(rejectedTwist>0);
  // S+L is the same group element but not a canonical signature scalar.
  auto m=text();const auto h=header(MtSignature::nodeId(f.curve));sign(m,h,f);
  const uint8_t order[32]={0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
                         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10};
  unsigned carry=0;for(unsigned i=0;i<32;++i) {const unsigned x=m.signature[32+i]+order[i]+carry;
    m.signature[32+i]=uint8_t(x);carry=x>>8;}
  uint8_t message[267]{};const size_t len=MtSignature::signingBuffer(get32(h.data()+4),get32(h.data()+8),get32(h.data()),m,message,sizeof(message));
  CHECK(mtPinnedSignatureVerify(f.curve,m.signature,message,len));
  CHECK(crypto_ed25519_check(m.signature,f.ed,message,len)!=0);
  MtSignature verifier;CHECK(verifier.trustPublicKey(get32(h.data()+4),f.curve,0));
  CHECK(apply(verifier,h,m)==MtSignature::Result::Rejected);
  std::puts("PASS weak/noncanonical public keys and documented upstream S+L verifier difference");
}
static void targetedForget() {
  Fixture a(17),b(18);MtSignature verifier;
  const auto node=MtSignature::nodeId(a.curve),other=MtSignature::nodeId(b.curve);
  CHECK(verifier.trustPublicKey(node,a.curve,0)&&verifier.trustPublicKey(other,b.curve,0));
  verifier.forgetPublicKey(node);CHECK(verifier.keyCount(1)==1);
  auto data=text();auto h=header(other);sign(data,h,b);
  CHECK(apply(verifier,h,data)==MtSignature::Result::Verified); // Unrelated authority remains.
  data=text();h=header(node);sign(data,h,a);
  CHECK(apply(verifier,h,data)==MtSignature::Result::Quarantined); // No authority from plain text.
  auto info=nodeInfo(a);sign(info,h,a);auto broken=info;broken.signature[0]^=1;
  CHECK(apply(verifier,h,broken)==MtSignature::Result::Rejected&&verifier.keyCount(10)==1);
  auto wrong=nodeInfo(b);sign(wrong,h,b);
  CHECK(apply(verifier,h,wrong)==MtSignature::Result::Rejected&&verifier.keyCount(10)==1);
  CHECK(apply(verifier,h,info)==MtSignature::Result::Verified&&verifier.keyCount(10)==2);
  CHECK(apply(verifier,h,data)==MtSignature::Result::Verified);
  std::puts("PASS targeted forget preserves unrelated authority; re-learning still requires key-derived ID and valid NodeInfo signature");
}
int main() {
  envelopeAndCrypto();nodeBootstrapAndCodec();provisionAndBounds();weakAndMalleable();targetedForget();
  std::printf("PASS %u checks, %u exact upstream XEdDSA signatures; metadata=%zuB cache=%zuB\n",
              checks,signatures,sizeof(RouteMetadata),sizeof(MtSignature));
}
