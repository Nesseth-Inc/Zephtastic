// SPDX-License-Identifier: GPL-3.0-only
// Independent source-derived protocol hazard probes; deterministic PUBLIC keys.
#include "CryptoEngine.h"
#include "PkiAck.h"
#include "MtSignature.h"
#include "monocypher.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <cassert>
#include <iostream>
#include <vector>
extern "C" bool stockEncrypt(const uint8_t *,const uint8_t *,uint32_t,uint32_t,uint32_t,const uint8_t *,size_t,uint8_t *);
extern "C" bool stockDecrypt(const uint8_t *,const uint8_t *,uint32_t,uint32_t,const uint8_t *,size_t,uint8_t *);
extern "C" bool stockAckAttach(const uint8_t *,const uint8_t *,uint32_t,uint32_t,uint32_t,const uint8_t *,size_t,uint8_t *,size_t *);
extern "C" int stockAckVerify(const uint8_t *,const uint8_t *,uint32_t,uint32_t,uint32_t,const uint8_t *,size_t);
extern size_t stockSigningBuffer(uint8_t *,size_t,uint32_t,uint32_t,uint32_t,const meshtastic_Data *);
extern "C" void mt_pki_test_wipe(void *p,size_t n){crypto_wipe(p,n);}
namespace {
using namespace mt::experimental;
struct Backend:PkiBackend {
  unsigned calls=0;
  bool sha256(const uint8_t *p,size_t n,uint8_t out[32])override {
    ++calls;unsigned size=0;return EVP_Digest(p,n,out,&size,EVP_sha256(),nullptr)==1&&size==32;
  }
  bool hmac256(const uint8_t key[32],const uint8_t *p,size_t n,uint8_t out[32])override {
    ++calls;unsigned size=0;return HMAC(EVP_sha256(),key,32,p,n,out,&size)&&size==32;
  }
  bool ccm(bool,const uint8_t[32],const uint8_t[13],const uint8_t *,size_t,uint8_t *,uint8_t[8])override{assert(false);return false;}
};
void pass(const char *name){std::cout<<"PASS "<<name<<'\n';}
void put(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(i*8));}
}
int main(){
  constexpr uint32_t Alice=0x12345678,Bob=0x87654321,Carol=0x22334455,Id=0x77665544;
  uint8_t a[32],b[32],c[32],A[32],B[32],C[32];
  for(unsigned i=0;i<32;++i){a[i]=uint8_t(13+17*i);b[i]=uint8_t(87+29*i);c[i]=uint8_t(31+11*i);}
  for(auto key:{a,b,c}){key[0]&=248;key[31]=(key[31]&127)|64;}
  crypto_x25519_public_key(A,a);crypto_x25519_public_key(B,b);crypto_x25519_public_key(C,c);
  // This calls the exact mechanically extracted stock function, including zero-extra behavior.
  for(uint32_t extra:{0u,1u,0x12345678u,0xffffffffu})for(uint32_t from:{Alice,Bob})for(uint32_t id:{1u,Id,0xffffffffu}){
    CryptoEngine stock;stock.initNonce(from,id,extra);uint8_t expected[16]{};
    put(expected,id);put(expected+4,extra);put(expected+8,from);
    assert(!memcmp(stock.nonce,expected,16));
  }
  pass("24_exact_stock_nonce_layouts_including_extra_zero");
  const uint8_t text[]{8,1,18,4,'t','e','s','t',72,0};uint8_t wire[64],plain[64],changed[64];
  assert(stockEncrypt(a,B,Alice,Id,0x10203040,text,sizeof(text),wire));
  assert(stockDecrypt(b,A,Alice,Id,wire,sizeof(text)+12,plain)&&!memcmp(plain,text,sizeof(text)));
  // Receiver key + from + id are crypto inputs; destination/channel/hops are absent.
  for(unsigned bit=0;bit<32;++bit){
    assert(!stockDecrypt(b,A,Alice^(1u<<bit),Id,wire,sizeof(text)+12,plain));
    assert(!stockDecrypt(b,A,Alice,Id^(1u<<bit),wire,sizeof(text)+12,plain));
  }
  assert(!stockDecrypt(c,A,Alice,Id,wire,sizeof(text)+12,plain));
  for(unsigned i=0;i<sizeof(text)+12;++i){memcpy(changed,wire,sizeof(text)+12);changed[i]^=1;assert(!stockDecrypt(b,A,Alice,Id,changed,sizeof(text)+12,plain));}
  pass("stock_cipher_binds_sender_id_recipient_key_and_all_cipher_tag_extra_bytes");
  // Demonstrate WHY outer-address gates and explicit trusted-peer lookup are required.
  uint8_t ack[233],out[233];size_t n=0,len=0;const uint8_t routing[]{0x18,0};
  assert(stockAckAttach(c,A,Carol,Alice,Id,routing,2,ack,&n));
  assert(stockAckVerify(a,C,Carol,Alice,Id,ack,n)==1); // Carol's own pairwise MAC is valid.
  Backend backend;PkiCrypto crypto(backend);PkiAck proof(crypto);
  PendingAck pending{true,Bob,Alice,Id};AckEnvelope envelope{true,Carol,Alice,Id};
  memset(out,0x5a,sizeof(out));
  assert(proof.verify(a,B,pending,envelope,ack,n,out,sizeof(out),len)==AckProofVerdict::ContextMismatch);
  assert(!backend.calls&&!len);
  envelope.from=Bob;
  assert(proof.verify(a,B,pending,envelope,ack,n,out,sizeof(out),len)==AckProofVerdict::InvalidProof);
  for(auto x:out)assert(x==0x5a);
  pass("valid_third_peer_pairwise_proof_is_not_recipient_receipt");
  assert(stockAckAttach(b,A,Bob,Alice,Id,routing,2,ack,&n));
  for(unsigned change=0;change<5;++change){
    auto p=pending;auto e=envelope;backend.calls=0;
    if(change==0)e.from=Carol;
    if(change==1)e.to=Bob;
    if(change==2)e.request=Id+1;
    if(change==3)p.active=false;
    if(change==4)e.decodedRouting=false;
    assert(proof.verify(a,B,p,e,ack,n,out,sizeof(out),len)==AckProofVerdict::ContextMismatch);
    assert(!backend.calls&&!len);
  }
  pass("cheap_pending_direction_port_and_id_gate_precedes_crypto");
  assert(proof.verify(a,B,pending,envelope,ack,n,out,sizeof(out),len)==AckProofVerdict::ValidProof&&len==2);
  for(uint8_t error:{uint8_t(0),uint8_t(5),uint8_t(32),uint8_t(35)}){
    const uint8_t r[]{0x18,error};assert(stockAckAttach(b,A,Bob,Alice,Id,r,2,ack,&n));
    assert(proof.verify(a,B,pending,envelope,ack,n,out,sizeof(out),len)==AckProofVerdict::ValidProof&&len==2&&out[1]==error);
  }
  const uint8_t routeVariant[]{0x0a,0};assert(stockAckAttach(b,A,Bob,Alice,Id,routeVariant,2,ack,&n));
  assert(proof.verify(a,B,pending,envelope,ack,n,out,sizeof(out),len)==AckProofVerdict::ValidProof&&out[0]==0x0a);
  pass("valid_proof_alone_does_not_distinguish_success_nak_or_route_variant");
  // The metadata signing envelope is compared with the exact stock builder.
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_NODEINFO_APP;data.payload.size=3;
  data.payload.bytes[0]=0x42;data.payload.bytes[1]=1;data.payload.bytes[2]=7;
  data.request_id=1;data.reply_id=2;data.emoji=3;data.has_bitfield=true;data.bitfield=2;data.want_response=true;
  bench::RouteMetadata m{};m.portnum=4;m.payloadLength=3;memcpy(m.payload,data.payload.bytes,3);
  m.request_id=1;m.reply_id=2;m.emoji=3;m.hasBitfield=true;m.bitfield=2;m.wantResponse=true;
  uint8_t expected[267],candidate[267];const auto size=stockSigningBuffer(expected,sizeof(expected),Alice,Id,Bob,&data);
  const auto got=bench::MtSignature::signingBuffer(Alice,Id,Bob,m,candidate,sizeof(candidate));
  assert(size==37&&got==size&&!memcmp(expected,candidate,size));
  for(unsigned field=0;field<11;++field){
    auto altered=data;auto from=Alice,to=Bob,id=Id;
    switch(field){case 0:++from;break;case 1:++id;break;case 2:++to;break;case 3:altered.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;break;
      case 4:++altered.request_id;break;case 5:++altered.reply_id;break;case 6:++altered.emoji;break;case 7:altered.has_bitfield=false;break;
      case 8:altered.bitfield^=1;break;case 9:altered.want_response=false;break;case 10:altered.payload.bytes[2]^=1;break;}
    assert(stockSigningBuffer(candidate,sizeof(candidate),from,id,to,&altered)==size&&memcmp(expected,candidate,size));
  }
  pass("actual_stock_signature_builder_matches_candidate_and_binds_11_metadata_payload_mutations");
}
