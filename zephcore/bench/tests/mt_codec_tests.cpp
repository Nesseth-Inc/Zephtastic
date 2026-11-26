// SPDX-License-Identifier: GPL-3.0-only
// Host-only: no serial ports, hardware, network keys or RF. Build with
// MT_CODEC_NANOPB_REFERENCE to compare against the exact pinned generated
// Data/Routing/User schemas + nanopb 0.4.9.1 in upstream-differential/source.
#include "../../adapters/bench/vendor/MtCodec.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
#if defined(MT_CODEC_NANOPB_REFERENCE)
#include <pb_decode.h>
#include <pb_encode.h>
#include <meshtastic/mesh.pb.h>
#endif
#if defined(MT_CODEC_UPSTREAM_CRYPTO_REFERENCE)
// The runner mechanically extracts unchanged initNonce/encryptPacket methods
// and compiles the unchanged pinned ESP32CryptoEngine.cpp with a host CTR shim.
extern "C" void mtPinnedNonce(uint32_t,uint32_t,uint8_t[16]);
extern "C" bool mtPinnedCrypt(const uint8_t *,size_t,uint32_t,uint32_t,uint8_t *,size_t);
#endif
using bench::MtCodec;
using bench::RouteMetadata;
using Bytes=std::vector<uint8_t>;
static unsigned cases=0;
static void put32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static Bytes hex(const char *s) {
  Bytes b;
  auto v=[](char c){return c<='9'?c-'0':c-'a'+10;};
  for(size_t i=0;s[i];i+=2)b.push_back(uint8_t(v(s[i])*16+v(s[i+1])));
  return b;
}
static void var(Bytes &b,uint64_t v) {do{uint8_t c=uint8_t(v&127);v>>=7;b.push_back(c|(v?128:0));}while(v);}
static void blob(Bytes &b,unsigned tag,const Bytes &p) {var(b,uint64_t(tag)<<3|2);var(b,p.size());b.insert(b.end(),p.begin(),p.end());}
static void fixed(Bytes &b,unsigned tag,uint32_t v) {var(b,uint64_t(tag)<<3|5);for(unsigned i=0;i<4;++i)b.push_back(uint8_t(v>>(8*i)));}
static Bytes data(uint32_t port,const Bytes &payload,uint32_t req=0,uint32_t reply=0) {
  Bytes b{0x08};var(b,port);blob(b,2,payload);if(req)fixed(b,6,req);if(reply)fixed(b,7,reply);return b;
}
static Bytes frame(const Bytes &plain,uint8_t channel=8,uint32_t to=0xffffffffu,
                   uint32_t from=0x10203040,uint32_t id=0x50607080) {
  Bytes b(16,0);put32(b.data(),to);put32(b.data()+4,from);put32(b.data()+8,id);
  b[12]=0x63;b[13]=channel;b[15]=uint8_t(from);b.insert(b.end(),plain.begin(),plain.end());return b;
}
static Bytes syntheticKey(size_t n) {Bytes k(n);for(size_t i=0;i<n;++i)k[i]=uint8_t(i*7+3);return k;}
static void encryptFrame(Bytes &p,const Bytes &key) {
  uint8_t nonce[16];
  auto le=[](const uint8_t *b){return uint32_t(b[0])|uint32_t(b[1])<<8|uint32_t(b[2])<<16|uint32_t(b[3])<<24;};
  MtCodec::nonce(le(p.data()+4),le(p.data()+8),nonce);
  assert(bench::mtAesCtr(nullptr,key.data(),key.size(),nonce,p.data()+16,p.size()-16));
}
static void knownVectors() {
  // Public NIST SP 800-38A, F.5 AES-CTR vectors (not network configuration).
  // https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38a.pdf
  const Bytes iv=hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
  const Bytes plain=hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
  const char *keys[]={"2b7e151628aed2a6abf7158809cf4f3c","603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"};
  const char *cipher[]={"874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee","601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c52b0930daa23de94ce87017ba2d84988ddfc9c58db67aada613c2dd08457941a6"};
  for(unsigned k=0;k<2;++k) {
    const Bytes key=hex(keys[k]);Bytes p=plain;
    assert(bench::mtAesCtr(nullptr,key.data(),key.size(),iv.data(),p.data(),p.size()));
    assert(p==hex(cipher[k]));
    assert(bench::mtAesCtr(nullptr,key.data(),key.size(),iv.data(),p.data(),p.size()));assert(p==plain);
    for(size_t n=1;n<plain.size();++n) {
      p=plain;p.resize(n);assert(bench::mtAesCtr(nullptr,key.data(),key.size(),iv.data(),p.data(),n));
      assert(std::equal(p.begin(),p.end(),hex(cipher[k]).begin()));
    }
  }
  uint8_t n[16];MtCodec::nonce(0x12345678,0x90abcdef,n);
  assert(Bytes(n,n+16)==hex("efcdab90000000007856341200000000"));
  ++cases;
}
struct BackendState {unsigned calls=0;bool fail=false;};
static bool counted(void *ctx,const uint8_t *k,size_t kn,const uint8_t iv[16],uint8_t *p,size_t n) {
  auto &s=*static_cast<BackendState *>(ctx);++s.calls;
  return !s.fail&&bench::mtAesCtr(nullptr,k,kn,iv,p,n);
}
static void configuredBoundaries() {
  BackendState state;MtCodec c(counted,&state);RouteMetadata m;
  auto k=syntheticKey(16);auto p=frame(data(1,Bytes{'h','i'}));
  assert(!c.configured());assert(!c.decode(p.data(),p.size(),m));assert(!m.channelDecoded&&!m.decodeRejected&&state.calls==0);
  for(size_t n: {size_t(0),size_t(1),size_t(15),size_t(17),size_t(24),size_t(31),size_t(33)}) {
    Bytes invalid(n);assert(!c.setKey(8,invalid.data(),n));assert(!c.configured());
  }
  assert(!c.setKey(8,nullptr,16));assert(c.setKey(8,k.data(),k.size()));assert(c.configured()&&c.channelHash()==8);
  encryptFrame(p,k);const auto unchanged=p;
  assert(c.decode(p.data(),p.size(),m));assert(m.channelDecoded&&!m.decodeRejected&&m.portnum==1);
  assert(m.encodedDataSize==p.size()-16);assert(p==unchanged);
  state.fail=true;assert(!c.decode(p.data(),p.size(),m));assert(m.decodeRejected&&m.backendFailed&&!m.channelDecoded);state.fail=false;
  auto wrong=k;wrong[0]^=0xff;assert(c.setKey(8,wrong.data(),wrong.size()));
  assert(!c.decode(p.data(),p.size(),m)&&m.decodeRejected&&!m.backendFailed&&!m.channelDecoded);
  assert(c.setKey(8,k.data(),k.size()));
  p[13]=9;const unsigned before=state.calls;assert(!c.decode(p.data(),p.size(),m));assert(!m.decodeRejected&&state.calls==before);
  assert(c.setKey(0,k.data(),k.size()));p[13]=0;put32(p.data(),0xabcdef01);
  assert(!c.decode(p.data(),p.size(),m));assert(!m.decodeRejected&&state.calls==before);
  // Same channel hash zero is decodable for an explicitly configured broadcast.
  p=frame(data(4,Bytes{0x12,0x01,'x'}),0);encryptFrame(p,k);
  assert(c.decode(p.data(),p.size(),m)&&m.nodeInfoValid);
  c.clearKey();assert(!c.configured());assert(!c.decode(p.data(),p.size(),m));assert(!m.decodeRejected);
  assert(c.setKey(8,k.data(),k.size()));assert(!c.setKey(8,k.data(),15)&&!c.configured());
  MtCodec unavailable(nullptr);assert(!unavailable.setKey(8,k.data(),k.size()));
  ++cases;
}
static void protocolFields() {
  RouteMetadata m;Bytes ack{0x18,0x00};Bytes p=data(5,ack,0x89abcdef,0x10203040);
  assert(MtCodec::decodeData(p.data(),p.size(),m));
  assert(!m.channelDecoded&&m.portnum==5&&m.routingPresent&&m.routingErrorPresent&&m.routingError==0);
  assert(m.request_id==0x89abcdef&&m.reply_id==0x10203040);
  ack={0x18,0x23};p=data(5,ack,17);assert(MtCodec::decodeData(p.data(),p.size(),m)&&m.routingError==35);
  ack={0x18,0x00};blob(ack,4,Bytes(8,0x5a));p=data(5,ack,17);blob(p,10,Bytes(64,0x55));
  assert(MtCodec::decodeData(p.data(),p.size(),m)&&m.ackProofLength==8&&m.signatureLength==64);
  // The parser supports last-value-wins protobuf scalars and oneof semantics.
  ack={0x18,0x01,0x18,0x00};p=data(5,ack,17);fixed(p,6,23);
  assert(MtCodec::decodeData(p.data(),p.size(),m)&&m.request_id==23&&m.routingError==0);
  ack={0x18,0x01,0x0a,0x00};p=data(5,ack);assert(MtCodec::decodeData(p.data(),p.size(),m)&&!m.routingErrorPresent);
  // Unknown future fields are skipped, never exposed as metadata.
  p=data(1,Bytes{'a'});blob(p,200,Bytes{1,2,3});var(p,201u<<3);var(p,0xffffffffffffffffull);
  assert(MtCodec::decodeData(p.data(),p.size(),m)&&m.portnum==1);
  ++cases;
}
static void malformed() {
  RouteMetadata m;
  const Bytes bad[]={Bytes{},Bytes{0},Bytes{8},Bytes{8,0},Bytes{8,0x80},Bytes{8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,2},
    Bytes{8,1,0x30,1},Bytes{8,1,0x3a,4,1,2,3,4},Bytes{8,1,0x0b},Bytes{8,1,0x12,0xff,0xff,0xff,0xff,0x0f},
    Bytes{8,1,0x48,0x80,2}};
  for(const auto &p:bad) {assert(!MtCodec::decodeData(p.data(),p.size(),m));assert(!m.channelDecoded&&m.portnum==0);}
  auto p=data(1,Bytes(234));assert(!MtCodec::decodeData(p.data(),p.size(),m));
  p=data(1,{});blob(p,10,Bytes(65));assert(!MtCodec::decodeData(p.data(),p.size(),m));
  p=data(5,Bytes{0x18,0x80});assert(!MtCodec::decodeData(p.data(),p.size(),m));
  Bytes ack{0x18,0};blob(ack,4,Bytes(9));p=data(5,ack);assert(!MtCodec::decodeData(p.data(),p.size(),m));
  Bytes route;blob(route,1,Bytes(36));Bytes routing;blob(routing,1,route);p=data(5,routing);
  assert(!MtCodec::decodeData(p.data(),p.size(),m));
  route.clear();blob(route,1,Bytes(3));routing.clear();blob(routing,1,route);p=data(5,routing);
  assert(!MtCodec::decodeData(p.data(),p.size(),m));
  route={0x10,0x80,0x01};routing.clear();blob(routing,1,route);p=data(5,routing);
  assert(!MtCodec::decodeData(p.data(),p.size(),m)); // int8_t SNR overflow
  p=Bytes(240);assert(!MtCodec::decodeData(p.data(),p.size(),m));
  ++cases;
}
static void nodeEvidence() {
  RouteMetadata m;auto check=[&](const Bytes &u,bool known){auto p=data(4,u);assert(MtCodec::decodeData(p.data(),p.size(),m));assert(m.nodeInfoValid==known);};
  check({},false);check(Bytes{0x12,1,'x'},true);
  check(Bytes{0x12,1,'x',0x30,1},true);assert(m.nodeInfoLicensed);
  check(Bytes{0x12,1,'x',0x30,0},true);assert(!m.nodeInfoLicensed);
  Bytes u;blob(u,2,Bytes(40,'x'));check(u,false);u.clear();blob(u,8,Bytes(33));check(u,false);
  check(Bytes{0x12,1,'x',0x12,0},false);check(Bytes{0x12,1,'x',0x08,1},false);
  u.clear();blob(u,8,Bytes(32,0x44));check(u,true);
  ++cases;
}
static void controlEncoding() {
  for(size_t kn:{size_t(16),size_t(32)}) {
    auto key=syntheticKey(kn);MtCodec c;assert(c.setKey(8,key.data(),key.size()));
    uint8_t out[64];memset(out,0xa5,sizeof(out));size_t n=0;
    assert(c.encodeZeroHopAck(0x10203040,0x50607080,0x12345678,0x90abcdef,out,sizeof(out),n));assert(n==27);
    assert(out[12]==0&&out[13]==8&&out[14]==0&&out[15]==0x40);
    RouteMetadata m;assert(c.decode(out,n,m));assert(m.channelDecoded&&m.routingErrorPresent&&m.routingError==0&&m.request_id==0x90abcdef);
    const Bytes unchanged(out,out+sizeof(out));
    assert(!c.encodeZeroHopAck(0,3,4,5,out,sizeof(out),n)&&n==0);assert(Bytes(out,out+sizeof(out))==unchanged);
    assert(!c.encodeZeroHopAck(1,2,3,4,out,26,n)&&n==0);assert(Bytes(out,out+sizeof(out))==unchanged);
    c.clearKey();assert(!c.encodeZeroHopAck(1,2,3,4,out,sizeof(out),n)&&n==0);
  }
  ++cases;
}
#if defined(MT_CODEC_UPSTREAM_CRYPTO_REFERENCE)
static void pinnedAesDifferential() {
  std::mt19937 rng(0x43545232);
  for(unsigned i=0;i<5000;++i) {
    Bytes key((i&1)?16:32),plain(1+rng()%239);for(auto &b:key)b=uint8_t(rng());for(auto &b:plain)b=uint8_t(rng());
    const uint32_t from=rng(),id=rng();uint8_t iv[16],upstreamIv[16];MtCodec::nonce(from,id,iv);mtPinnedNonce(from,id,upstreamIv);
    assert(memcmp(iv,upstreamIv,16)==0);
    auto a=plain,b=plain;assert(mtPinnedCrypt(key.data(),key.size(),from,id,a.data(),a.size()));
    assert(bench::mtAesCtr(nullptr,key.data(),key.size(),iv,b.data(),b.size()));assert(a==b);
    assert(bench::mtAesCtr(nullptr,key.data(),key.size(),iv,b.data(),b.size()));assert(b==plain);
  }
  std::puts("exact-pinned Meshtastic ESP32 CTR/nonce adapter + native CTR shim: 5000 lengths/nonces/keys PASS");
  ++cases;
}
#endif
#if defined(MT_CODEC_NANOPB_REFERENCE)
static Bytes encode(const pb_msgdesc_t *desc,const void *message) {
  Bytes b(512);auto stream=pb_ostream_from_buffer(b.data(),b.size());assert(pb_encode(&stream,desc,message));b.resize(stream.bytes_written);return b;
}
static bool reference(const Bytes &p,meshtastic_Data &d) {
  d=meshtastic_Data_init_zero;auto stream=pb_istream_from_buffer(p.data(),p.size());return pb_decode(&stream,&meshtastic_Data_msg,&d);
}
static void compare(const Bytes &p) {
  RouteMetadata m;meshtastic_Data d;const bool ours=MtCodec::decodeData(p.data(),p.size(),m),ref=reference(p,d);
  if(ours&&!ref) {std::fprintf(stderr,"codec accepted nanopb-invalid Data size=%zu\n",p.size());for(auto b:p)std::fprintf(stderr,"%02x",b);std::fprintf(stderr,"\n");}
  assert(!ours||ref);
  if(ours) {
    // Proto3 permits unknown enum numbers. nanopb's C decoder writes their
    // representation, so do not perform an invalid C++ enum lvalue load while
    // fuzzing future values; compare the exact decoded uint32 representation.
    uint32_t refPort=0;static_assert(sizeof(d.portnum)==sizeof(refPort),"pinned enum width");
    memcpy(&refPort,&d.portnum,sizeof(refPort));
    assert(m.portnum==refPort&&m.request_id==d.request_id&&m.reply_id==d.reply_id);
    assert(m.signatureLength==d.xeddsa_signature.size);
    if(m.routingPresent) {
      meshtastic_Routing r=meshtastic_Routing_init_zero;auto s=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
      assert(pb_decode(&s,&meshtastic_Routing_msg,&r));
      assert(m.routingErrorPresent==(r.which_variant==meshtastic_Routing_error_reason_tag));
      uint32_t refError=0;static_assert(sizeof(r.error_reason)==sizeof(refError),"pinned enum width");
      memcpy(&refError,&r.error_reason,sizeof(refError));
      if(m.routingErrorPresent) assert(m.routingError==refError);
      assert(m.ackProofLength==r.ack_proof.size);
    }
  }
}
static void nanopbDifferential() {
  std::mt19937 rng(0x4d544331);unsigned valid=0;
  for(unsigned i=0;i<5000;++i) {
    meshtastic_Data d=meshtastic_Data_init_zero;
    d.portnum=(i%3==0)?meshtastic_PortNum_ROUTING_APP:((i%3==1)?meshtastic_PortNum_NODEINFO_APP:meshtastic_PortNum_TEXT_MESSAGE_APP);
    d.request_id=rng();d.reply_id=rng();d.source=rng();d.dest=rng();d.want_response=i&1;
    d.has_bitfield=true;d.bitfield=uint8_t(rng());d.emoji=rng();
    Bytes payload;
    if(d.portnum==meshtastic_PortNum_ROUTING_APP) {
      meshtastic_Routing r=meshtastic_Routing_init_zero;r.which_variant=meshtastic_Routing_error_reason_tag;r.error_reason=meshtastic_Routing_Error(i%40);
      r.ack_proof.size=i%9;for(unsigned j=0;j<r.ack_proof.size;++j)r.ack_proof.bytes[j]=uint8_t(rng());
      payload=encode(&meshtastic_Routing_msg,&r);
    } else if(d.portnum==meshtastic_PortNum_NODEINFO_APP) {
      meshtastic_User u=meshtastic_User_init_zero;strcpy(u.long_name,"Synthetic codec fixture");strcpy(u.short_name,"T");u.public_key.size=32;
      memset(u.public_key.bytes,0x42,32);payload=encode(&meshtastic_User_msg,&u);
    } else {payload.resize(i%151);for(auto &b:payload)b=uint8_t(rng());}
    d.payload.size=payload.size();if(!payload.empty())memcpy(d.payload.bytes,payload.data(),payload.size());
    d.xeddsa_signature.size=i%65;for(unsigned j=0;j<d.xeddsa_signature.size;++j)d.xeddsa_signature.bytes[j]=uint8_t(rng());
    auto b=encode(&meshtastic_Data_msg,&d);
    if(b.size()>239)continue;
    RouteMetadata m;assert(MtCodec::decodeData(b.data(),b.size(),m));compare(b);++valid;
    const auto key=syntheticKey((i&1)?16:32);MtCodec c;assert(c.setKey(8,key.data(),key.size()));auto f=frame(b,8,0xffffffffu,rng()|1,rng()|1);
    encryptFrame(f,key);assert(c.decode(f.data(),f.size(),m));assert(m.channelDecoded&&m.encodedDataSize==b.size());
    if(i%11==0) {for(auto &v:b)if((rng()%11)==0)v=uint8_t(rng());compare(b);}
  }
  for(unsigned i=0;i<100000;++i) {Bytes b(rng()%240);for(auto &v:b)v=uint8_t(rng());compare(b);}
  // Proto3 explicitly encodes fixed32 fields with wire type 5, not varints.
  // The maximum payload that fits the LoRa envelope remains fully bounded.
  auto maximum=data(1,Bytes(233,0x55));compare(maximum);assert(maximum.size()==238);
  RouteMetadata maximumMetadata;assert(MtCodec::decodeData(maximum.data(),maximum.size(),maximumMetadata));
  // Verify our wire encoder against the actual upstream nanopb oneof encoding.
  const auto key=syntheticKey(32);MtCodec c;assert(c.setKey(8,key.data(),key.size()));uint8_t wire[64];size_t n=0;
  assert(c.encodeZeroHopAck(0x10203040,0x50607080,0x12345678,0x90abcdef,wire,sizeof(wire),n));
  Bytes decoded(wire+16,wire+n);uint8_t iv[16];MtCodec::nonce(0x10203040,0x12345678,iv);
  assert(bench::mtAesCtr(nullptr,key.data(),key.size(),iv,decoded.data(),decoded.size()));
  meshtastic_Routing r=meshtastic_Routing_init_zero;r.which_variant=meshtastic_Routing_error_reason_tag;r.error_reason=meshtastic_Routing_Error_NONE;
  meshtastic_Data d=meshtastic_Data_init_zero;d.portnum=meshtastic_PortNum_ROUTING_APP;const auto payload=encode(&meshtastic_Routing_msg,&r);
  d.payload.size=payload.size();memcpy(d.payload.bytes,payload.data(),payload.size());d.request_id=0x90abcdef;
  assert(decoded==encode(&meshtastic_Data_msg,&d));
  std::printf("nanopb exact-pinned differential: %u valid + 100000 malformed/random + control encoding PASS\n",valid);
  ++cases;
}
#endif
int main() {
  knownVectors();configuredBoundaries();protocolFields();malformed();nodeEvidence();controlEncoding();
#if defined(MT_CODEC_UPSTREAM_CRYPTO_REFERENCE)
  pinnedAesDifferential();
#endif
#if defined(MT_CODEC_NANOPB_REFERENCE)
  nanopbDifferential();
#endif
  std::printf("MT codec: %u groups PASS (host only)\n",cases);
}
