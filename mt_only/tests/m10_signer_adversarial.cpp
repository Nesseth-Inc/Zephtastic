// SPDX-License-Identifier: GPL-3.0-only
// Independent stock first-contact admission with actual cryptographic verification.
#include "pki/PkiSigner.h"
#include "MtSignature.h"
#include "monocypher.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include <cassert>
#include <cstring>
#include <iostream>
extern "C" size_t mtPinnedSigningBuffer(uint32_t,uint32_t,uint32_t,const bench::RouteMetadata *,uint8_t *,size_t);
extern "C" bool mtPinnedSignatureVerify(const uint8_t *,const uint8_t *,const uint8_t *,size_t);
extern "C" void mtPinnedSignWithScalar(const uint8_t *,const uint8_t *,const uint8_t *,size_t,uint8_t *);
extern "C" int reviewedStockBootstrap(const uint8_t *,size_t,uint32_t,uint32_t,uint32_t,bool,uint8_t *);
using namespace mt::pki;
struct Keys:KeyProvider {
  uint8_t scalar[32]{};IdentityBinding binding;KeyRef ref{7,3};bool enabled=true,fail=false;unsigned calls=0;
  explicit Keys(unsigned seed){for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed*31+i*19);scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;
    binding.generation=ref.generation;crypto_x25519_public_key(binding.publicKey,scalar);binding.node=bench::MtSignature::nodeId(binding.publicKey);}
  bool active(KeyRef r)const override{return enabled&&r.slot==ref.slot&&r.generation==ref.generation;}
  bool withPrivate(KeyRef r,PrivateKeyOperation &operation)override{
    ++calls;if(!active(r))return false;uint8_t scratch[32];memcpy(scratch,scalar,32);
    const bool result=operation.apply(scratch);crypto_wipe(scratch,32);return result&&!fail;
  }
};
struct Entropy:SigningEntropy{uint8_t bytes[32]{};bool fail=false;unsigned calls=0;
  bool hedge(uint8_t out[32])override{++calls;memcpy(out,bytes,32);return !fail;}};
void pass(const char *name){std::cout<<"PASS "<<name<<'\n';}
meshtastic_Data decoded(const SignedData &out){meshtastic_Data d{};auto s=pb_istream_from_buffer(out.bytes,out.size);assert(pb_decode(&s,meshtastic_Data_fields,&d));return d;}
SignedData encoded(const SignedData &out,const meshtastic_Data &d){auto changed=out;auto s=pb_ostream_from_buffer(changed.bytes,sizeof(changed.bytes));assert(pb_encode(&s,meshtastic_Data_fields,&d));changed.size=s.bytes_written;return changed;}
bench::RouteMetadata metadata(const meshtastic_Data &d){bench::RouteMetadata m;assert(bench::MtCodec::decodeData(nullptr,0,m)==false);m={};m.portnum=d.portnum;
  m.request_id=d.request_id;m.reply_id=d.reply_id;m.emoji=d.emoji;m.hasBitfield=d.has_bitfield;m.bitfield=d.bitfield;m.wantResponse=d.want_response;
  m.payloadLength=d.payload.size;memcpy(m.payload,d.payload.bytes,d.payload.size);return m;}
int main(){
  unsigned admitted=0,denied=0;
  for(unsigned seed=9;seed<25;++seed){
    Keys keys(seed);Entropy entropy;for(unsigned i=0;i<32;++i)entropy.bytes[i]=uint8_t(seed+31*i);
    NodeInfoProfile profile;strcpy(profile.longName,"Independent TEST contact");strcpy(profile.shortName,"TEST");profile.hardwareModel=(seed&1)?81:127;
    SigningEnvelope envelope;envelope.hasBitfield=true;envelope.bitfield=2; // Modern want_response merge must follow signature verification.
    envelope.replyId=seed*13;envelope.emoji=seed;SignedData out;
    assert(PkiSigner::bootstrap(keys,keys.ref,keys.binding,entropy,seed,0xffffffffu,profile,envelope,out)==SigningResult::Ok);
    uint8_t stored[32]{};assert(reviewedStockBootstrap(out.bytes,out.size,out.from,out.id,out.to,true,stored)==1);
    assert(!memcmp(stored,keys.binding.publicKey,32));++admitted;
    const auto d=decoded(out);const auto m=metadata(d);uint8_t buffer[267],signature[64];
    const auto n=mtPinnedSigningBuffer(out.from,out.id,out.to,&m,buffer,sizeof(buffer));
    mtPinnedSignWithScalar(keys.scalar,entropy.bytes,buffer,n,signature);
    assert(!memcmp(signature,d.xeddsa_signature.bytes,64));assert(mtPinnedSignatureVerify(keys.binding.publicKey,signature,buffer,n));
    for(unsigned variant=0;variant<6;++variant){auto changed=d;auto from=out.from,id=out.id,to=out.to;
      if(variant==0)from^=1;
      if(variant==1)id^=1;
      if(variant==2)to=0x12345678;
      if(variant==3)changed.want_response=true; // Same effective request after merge, different signed envelope.
      if(variant==4)changed.has_bitfield=false;
      if(variant==5)changed.reply_id^=1;
      auto wire=encoded(out,changed);memset(stored,0x5a,32);
      assert(reviewedStockBootstrap(wire.bytes,wire.size,from,id,to,true,stored)==2);for(auto v:stored)assert(v==0x5a);++denied;
    }
    assert(reviewedStockBootstrap(out.bytes,out.size,out.from,out.id,out.to,false,stored)==2);++denied;
  }
  pass("16_exact_stock_signatures_and_112_actual_first_contact_negative_branches");
  assert(admitted==16&&denied==112);
  Keys keys(55);Entropy entropy;NodeInfoProfile profile;strcpy(profile.longName,"Synthetic independent");strcpy(profile.shortName,"TEST");profile.hardwareModel=81;
  SigningEnvelope envelope;envelope.hasBitfield=true;SignedData signedNode;
  assert(PkiSigner::bootstrap(keys,keys.ref,keys.binding,entropy,101,0xffffffffu,profile,envelope,signedNode)==SigningResult::Ok);
  {
    auto d=decoded(signedNode);meshtastic_User user{};auto in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
    assert(pb_decode(&in,meshtastic_User_fields,&user));
    assert(user.has_is_unmessagable&&!user.is_unmessagable);
    auto unavailable=profile;unavailable.ownerUnmessagable=true;SignedData marked;
    assert(PkiSigner::bootstrap(keys,keys.ref,keys.binding,entropy,104,0xffffffffu,unavailable,envelope,marked)==SigningResult::Ok);
    d=decoded(marked);user={};in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
    assert(pb_decode(&in,meshtastic_User_fields,&user));
    assert(user.has_is_unmessagable&&user.is_unmessagable);
    auto unsupported=profile;unsupported.role=10;SignedData rejected;
#ifdef CONFIG_MT_TAK_TRACKER
    assert(PkiSigner::bootstrap(keys,keys.ref,keys.binding,entropy,105,0xffffffffu,unsupported,envelope,rejected)==SigningResult::Ok);
#else
    assert(PkiSigner::bootstrap(keys,keys.ref,keys.binding,entropy,105,0xffffffffu,unsupported,envelope,rejected)==SigningResult::InvalidArgument);
#endif
    pass("owner_unmessagable_defaults_false_and_is_signed_when_enabled;role10_is_build_gated");
  }
  {
    auto d=decoded(signedNode);meshtastic_User u{};auto in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&in,meshtastic_User_fields,&u));
    for(unsigned variant=0;variant<3;++variant){auto user=u;auto malformed=d;
      if(variant==0)user.public_key.size=0;
      if(variant==1)user.public_key.bytes[3]^=1;
      if(variant==2)strcpy(user.long_name,"Spoofed name");
      auto s=pb_ostream_from_buffer(malformed.payload.bytes,sizeof(malformed.payload.bytes));assert(pb_encode(&s,meshtastic_User_fields,&user));malformed.payload.size=s.bytes_written;
      auto wire=encoded(signedNode,malformed);uint8_t ignored[32];assert(reviewedStockBootstrap(wire.bytes,wire.size,wire.from,wire.id,wire.to,true,ignored)==2);
    }
    pass("unsigned_mutation_of_nodeinfo_key_presence_key_bytes_or_name_fails_stock_bootstrap");
  }
  {
    bench::MtSignature receiver;Keys other(69);assert(receiver.trustPublicKey(keys.binding.node,other.binding.publicKey,100));
    auto d=decoded(signedNode);bench::RouteMetadata m;assert(bench::MtCodec::decodeData(signedNode.bytes,signedNode.size,m));m.channelDecoded=true;
    uint8_t frame[17]{};auto put=[](uint8_t *p,uint32_t x){for(unsigned i=0;i<4;++i)p[i]=uint8_t(x>>(i*8));};
    put(frame,signedNode.to);put(frame+4,signedNode.from);put(frame+8,signedNode.id);
    assert(receiver.apply(frame,sizeof(frame),m,200)==bench::MtSignature::Result::Rejected&&receiver.keyCount(200)==1);
    (void)d;pass("self_certifying_first_contact_cannot_replace_an_existing_explicit_signature_key");
  }
  {
    auto data=decoded(signedNode);data.xeddsa_signature.size=0;SignedData sentinel;
    for(size_t i=0;i<sizeof(sentinel);++i)reinterpret_cast<uint8_t *>(&sentinel)[i]=0x5a;
    auto out=sentinel;
    keys.enabled=false;auto entropyCalls=entropy.calls;
    assert(PkiSigner::sign(keys,keys.ref,keys.binding,entropy,102,0xffffffffu,data,out)==SigningResult::NotReady);
    assert(entropy.calls==entropyCalls&&!memcmp(&out,&sentinel,sizeof(out)));keys.enabled=true;
    keys.fail=true;assert(PkiSigner::sign(keys,keys.ref,keys.binding,entropy,102,0xffffffffu,data,out)!=SigningResult::Ok&&!memcmp(&out,&sentinel,sizeof(out)));
    keys.fail=false;entropy.fail=true;assert(PkiSigner::sign(keys,keys.ref,keys.binding,entropy,102,0xffffffffu,data,out)==SigningResult::EntropyUnavailable&&!memcmp(&out,&sentinel,sizeof(out)));
    pass("inactive_identity_failed_borrow_and_entropy_failure_leave_output_unchanged");
  }
  {
    auto data=decoded(signedNode);data.xeddsa_signature.size=0;entropy.fail=false;SignedData a,b;
    assert(PkiSigner::sign(keys,keys.ref,keys.binding,entropy,103,0xffffffffu,data,a)==SigningResult::Ok);
    entropy.bytes[31]^=1;assert(PkiSigner::sign(keys,keys.ref,keys.binding,entropy,103,0xffffffffu,data,b)==SigningResult::Ok);
    assert(a.size==b.size&&memcmp(a.bytes,b.bytes,a.size));
    pass("hedge_changes_output_so_retry_must_cache_original_signed_ciphertext");
  }
}
