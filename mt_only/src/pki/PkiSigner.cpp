// SPDX-License-Identifier: GPL-3.0-only
#if defined(CONFIG_MT_REPLY_EXAMPLE)
#include "../stock/optional/ReplyWire.h"
#endif
#include "PkiSigner.h"
#include "../BoardIdentity.h"
#include "../DeviceRolePolicy.h"
#include "MtSignature.h"
#include "MtModulePorts.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "PrivateApplication.h"
#include "../PublicApplication.h"
#include "../TraceRoute.h"
#include <pb_encode.h>
#include <pb_decode.h>
#include <stdio.h>
#include <string.h>

namespace mt::pki {
static_assert(bench::ModuleNodeStatusPort==meshtastic_PortNum_NODE_STATUS_APP);
static_assert(bench::ModuleRangeTestPort==meshtastic_PortNum_RANGE_TEST_APP);
static_assert(bench::ModuleTelemetryPort==meshtastic_PortNum_TELEMETRY_APP);
static_assert(bench::ModuleNeighborInfoPort==meshtastic_PortNum_NEIGHBORINFO_APP);
static_assert(bench::ModuleMeshBeaconPort==meshtastic_PortNum_MESH_BEACON_APP);
namespace {
template<typename T> struct Scratch {
  T value{};
  Scratch()=default;
  Scratch(const Scratch &)=delete;
  Scratch &operator=(const Scratch &)=delete;
  ~Scratch(){crypto_wipe(&value,sizeof(value));}
};
template<size_t N> using Bytes=Scratch<uint8_t[N]>;
bool node(uint32_t n){return n>1&&n<0xfffffffeu;}
void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
uint32_t port(const meshtastic_Data &d){
  __underlying_type(meshtastic_PortNum) raw;
  memcpy(&raw,&d.portnum,sizeof(raw));return uint32_t(raw);
}
bool bindingValid(KeyRef ref,const IdentityBinding &b){
  Bytes<32> ed;
  return ref.generation&&ref.generation==b.generation&&b.version==1&&b.algorithm==1&&
    node(b.node)&&bench::MtSignature::nodeId(b.publicKey)==b.node&&
    bench::MtSignature::admissiblePublicKey(b.publicKey,ed.value);
}
size_t signingBuffer(const IdentityBinding &b,uint32_t id,uint32_t to,
                     const meshtastic_Data &d,uint8_t *out){
  out[0]=1;
  const uint32_t fields[8]={b.node,id,to,port(d),d.request_id,d.reply_id,d.emoji,
                            d.has_bitfield?uint32_t(d.bitfield):0u};
  for(unsigned i=0;i<8;++i)put32(out+1+4*i,fields[i]);
  out[33]=uint8_t((d.want_response?1:0)|(d.has_bitfield?2:0));
  memcpy(out+34,d.payload.bytes,d.payload.size);return 34+d.payload.size;
}
bool nodeInfoMatches(const IdentityBinding &b,const meshtastic_Data &d){
  Scratch<meshtastic_User> user;auto input=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
  if(!pb_decode(&input,meshtastic_User_fields,&user.value))return false;
  char expected[10];snprintf(expected,sizeof(expected),"!%08x",unsigned(b.node));
  return user.value.public_key.size==32&&
    crypto_verify32(user.value.public_key.bytes,b.publicKey)==0&&
    memchr(user.value.id,0,sizeof(user.value.id))&&strcmp(user.value.id,expected)==0;
}
bool canonicalPositionBroadcast(const meshtastic_Data &input) {
  if(port(input)!=meshtastic_PortNum_POSITION_APP||!input.has_bitfield||input.bitfield>1||
     input.want_response||input.request_id||input.reply_id||input.emoji||input.dest||input.source||
     input.xeddsa_signature.size||!input.payload.size||input.payload.size>sizeof(input.payload.bytes))return false;
  Scratch<meshtastic_Data> canonicalData;canonicalData.value=input;
  // PositionQuery's canonical local broadcast predicate currently models the
  // stock zero bitfield. Check the independent allowed-value rule above, then
  // use its exact payload/envelope serializer check without changing the bytes
  // that will actually be signed when bitfield is one.
  canonicalData.value.bitfield=0;
  Bytes<239> wire;auto output=pb_ostream_from_buffer(wire.value,sizeof(wire.value));
  if(!pb_encode(&output,meshtastic_Data_fields,&canonicalData.value))return false;
  return mt::PositionQuery::canonicalBroadcast(wire.value,output.bytes_written);
}
[[gnu::noinline]] bool licensedNodeInfoCanonical(const IdentityBinding &binding,
                                                  const meshtastic_Data &data) {
  if(!nodeInfoMatches(binding,data)||data.want_response||!data.has_bitfield||
     data.bitfield>1||data.reply_id||data.emoji)return false;
  Scratch<meshtastic_User> user;auto input=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
  if(!pb_decode(&input,meshtastic_User_fields,&user.value)||input.bytes_left)return false;
  Bytes<239> canonical;auto output=pb_ostream_from_buffer(canonical.value,sizeof(canonical.value));
  return pb_encode(&output,meshtastic_User_fields,&user.value)&&
    output.bytes_written==data.payload.size&&
    !memcmp(canonical.value,data.payload.bytes,data.payload.size);
}
[[gnu::noinline]] bool routingUnicastResponse(const bench::RouteMetadata &m) {
  return m.portnum==meshtastic_PortNum_ROUTING_APP&&m.routingPresent&&
    m.routingErrorPresent&&!m.ackProofLength&&m.hasBitfield&&m.bitfield<=1&&
    !m.wantResponse&&m.request_id&&!m.reply_id&&!m.emoji&&!m.signatureLength&&
    m.payloadLength==2&&m.payload[0]==0x18&&m.payload[1]<=127&&
    m.routingError==m.payload[1]&&
    (m.routingError<=9||(m.routingError>=32&&m.routingError<=39));
}
[[gnu::noinline]] bool traceUnicastCanonical(const uint8_t *wire,size_t length,
                                              const bench::RouteMetadata &m) {
  meshtastic_RouteDiscovery route{};
  if(!mt::TraceRoute::semantic(m,route))return false;
  route=meshtastic_RouteDiscovery_init_zero;
  auto input=pb_istream_from_buffer(m.payload,m.payloadLength);
  if(!pb_decode(&input,meshtastic_RouteDiscovery_fields,&route)||input.bytes_left)return false;
  Bytes<233> payload;auto payloadOut=pb_ostream_from_buffer(payload.value,sizeof(payload.value));
  if(!pb_encode(&payloadOut,meshtastic_RouteDiscovery_fields,&route)||
     payloadOut.bytes_written!=m.payloadLength||memcmp(payload.value,m.payload,m.payloadLength))return false;
  Bytes<239> canonical;size_t size=0;
  const bool encoded=mt::TraceRoute::requestShape(m)?
    mt::TraceRoute::requestData(m.payload,m.payloadLength,canonical.value,sizeof(canonical.value),size):
    mt::TraceRoute::responseData(m.payload,m.payloadLength,m.request_id,canonical.value,sizeof(canonical.value),size);
  return encoded&&size==length&&!memcmp(canonical.value,wire,length);
}
[[gnu::noinline]] bool licensedUnicastShape(const IdentityBinding &binding,uint32_t id,uint32_t to,
                                             const meshtastic_Data &input) {
  if(!id||!node(to)||to==binding.node||
     input.payload.size>sizeof(input.payload.bytes)||input.xeddsa_signature.size||
     input.dest||input.source)return false;
  Bytes<239> wire;auto output=pb_ostream_from_buffer(wire.value,sizeof(wire.value));
  if(!pb_encode(&output,meshtastic_Data_fields,&input)||!output.bytes_written)return false;
  const size_t length=output.bytes_written;
  Scratch<bench::RouteMetadata> metadata;auto &m=metadata.value;
  if(!bench::MtCodec::decodeData(wire.value,length,m))return false;
  const uint32_t app=port(input);
  if(app==meshtastic_PortNum_TEXT_MESSAGE_APP||app==meshtastic_PortNum_POSITION_APP||
     app==meshtastic_PortNum_TELEMETRY_APP||app==meshtastic_PortNum_NEIGHBORINFO_APP)
    return mt::pki::PrivateApplication::allowed(wire.value,length);
  if(app==meshtastic_PortNum_NODEINFO_APP)
    return licensedNodeInfoCanonical(binding,input);
  if(app==meshtastic_PortNum_ROUTING_APP)return routingUnicastResponse(m);
  if(app==meshtastic_PortNum_TRACEROUTE_APP)return traceUnicastCanonical(wire.value,length,m);
#if defined(CONFIG_MT_REPLY_EXAMPLE)
  if(app==meshtastic_PortNum_REPLY_APP&&input.request_id)return mt::stock::optional::moduleReply(input);
#endif
  return mt::PublicApplication::canonical(wire.value,length);
}
// Stock Crypto@1c817c2 XEdDSA, expressed using Monocypher 4.0.3 primitives.
// This is not Ed25519(seed): the private input is an already-clamped X25519
// scalar. Its normalized scalar bytes themselves supply the SHA512 prefix.
bool xeddsa(const uint8_t scalar[32],const IdentityBinding &b,const uint8_t hedge[32],
            const uint8_t *message,size_t length,uint8_t signature[64]){
  if(!scalar||(scalar[0]&7)||(scalar[31]&0xc0)!=0x40)return false;
  Bytes<32> curve,ed,normalized,negated,r,k;
  Bytes<64> digest;
  Scratch<crypto_sha512_ctx> hash;
  crypto_x25519_public_key(curve.value,scalar);
  if(crypto_verify32(curve.value,b.publicKey))return false;
  crypto_eddsa_scalarbase(ed.value,scalar);
  // -a mod L, with constant-time selection if the public Edwards sign is one.
  const uint8_t minusOne[32]={0xec,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,
    0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10};
  const uint8_t zero[32]={};
  crypto_eddsa_mul_add(negated.value,minusOne,scalar,zero);
  const uint8_t mask=uint8_t(0u-(ed.value[31]>>7));
  for(unsigned i=0;i<32;++i)normalized.value[i]=uint8_t((scalar[i]&~mask)|(negated.value[i]&mask));
  ed.value[31]&=0x7f;
  crypto_sha512(digest.value,normalized.value,32);
  crypto_sha512_init(&hash.value);
  crypto_sha512_update(&hash.value,digest.value+32,32);
  crypto_sha512_update(&hash.value,message,length);
  crypto_sha512_update(&hash.value,hedge,32);
  crypto_sha512_final(&hash.value,digest.value);
  crypto_eddsa_reduce(r.value,digest.value);
  crypto_eddsa_scalarbase(signature,r.value);
  crypto_sha512_init(&hash.value);
  crypto_sha512_update(&hash.value,signature,32);
  crypto_sha512_update(&hash.value,ed.value,32);
  crypto_sha512_update(&hash.value,message,length);
  crypto_sha512_final(&hash.value,digest.value);
  crypto_eddsa_reduce(k.value,digest.value);
  crypto_eddsa_mul_add(signature+32,k.value,normalized.value,r.value);
  return crypto_ed25519_check(signature,ed.value,message,length)==0;
}
struct SignOperation:PrivateKeyOperation {
  const IdentityBinding &binding;const uint8_t *hedge,*message;size_t length;
  uint8_t *signature;unsigned calls=0;bool succeeded=false;
  SignOperation(const IdentityBinding &b,const uint8_t *h,const uint8_t *m,size_t n,uint8_t *s):
    binding(b),hedge(h),message(m),length(n),signature(s){}
  bool apply(const uint8_t scalar[32])override{
    if(++calls!=1)return false;
    succeeded=xeddsa(scalar,binding,hedge,message,length,signature);return succeeded;
  }
};
bool boundedName(const char *s,size_t n){return s&&memchr(s,0,n)&&s[0];}
}
bool PkiSigner::ready(KeyProvider &keys,KeyRef ref,const IdentityBinding &binding){
  if(!bindingValid(ref,binding)||!keys.active(ref))return false;
  struct Check:PrivateKeyOperation {
    const uint8_t *expected;unsigned calls=0;bool matched=false;
    explicit Check(const uint8_t *p):expected(p){}
    bool apply(const uint8_t scalar[32])override{
      if(++calls!=1||!scalar||(scalar[0]&7)||(scalar[31]&0xc0)!=0x40)return false;
      Bytes<32> publicKey;crypto_x25519_public_key(publicKey.value,scalar);
      matched=crypto_verify32(publicKey.value,expected)==0;return matched;
    }
  } check(binding.publicKey);
  return keys.withPrivate(ref,check)&&check.calls==1&&check.matched&&keys.active(ref);
}
SigningResult PkiSigner::signPrepared(KeyProvider &keys,KeyRef ref,const IdentityBinding &binding,SigningEntropy &entropy,
                                      uint32_t id,uint32_t to,const meshtastic_Data &input,SignedData &out,bool attachSignature){
  if(!keys.active(ref))return SigningResult::NotReady;
  Scratch<meshtastic_Data> data;data.value=input;data.value.xeddsa_signature.size=attachSignature?64:0;
  size_t encoded=0;
  if(!pb_get_encoded_size(&encoded,meshtastic_Data_fields,&data.value)||encoded>SignedData::Capacity)return SigningResult::TooLarge;
  if(attachSignature){
    Bytes<267> message;const size_t length=signingBuffer(binding,id,to,input,message.value);
    Bytes<32> hedge;
    if(!entropy.hedge(hedge.value))return SigningResult::EntropyUnavailable;
    if(!keys.active(ref))return SigningResult::NotReady;
    SignOperation op(binding,hedge.value,message.value,length,data.value.xeddsa_signature.bytes);
    if(!keys.withPrivate(ref,op)||op.calls!=1||!op.succeeded)return SigningResult::KeyUnavailable;
    if(!keys.active(ref))return SigningResult::NotReady;
  }
  Scratch<SignedData> result;result.value.from=binding.node;result.value.to=to;result.value.id=id;
  auto output=pb_ostream_from_buffer(result.value.bytes,sizeof(result.value.bytes));
  if(!pb_encode(&output,meshtastic_Data_fields,&data.value)||output.bytes_written!=encoded)return SigningResult::CryptoFailure;
  result.value.size=output.bytes_written;out=result.value;return SigningResult::Ok;
}
SigningResult PkiSigner::sign(KeyProvider &keys,KeyRef ref,const IdentityBinding &binding,SigningEntropy &entropy,
                              uint32_t id,uint32_t to,const meshtastic_Data &input,SignedData &out,bool attachSignature){
  const uint32_t app=port(input);
  if(!id||(!node(to)&&to!=0xffffffffu)||to==binding.node||
     input.payload.size>sizeof(input.payload.bytes)||input.xeddsa_signature.size||input.dest||input.source||
     (app==meshtastic_PortNum_POSITION_APP?
       (to!=0xffffffffu||!canonicalPositionBroadcast(input)):
       (app!=1&&app!=4&&!bench::moduleBroadcastPort(app)))||
     (bench::publicSignedModulePort(port(input))&&(to!=0xffffffffu||!input.has_bitfield||
       input.bitfield>1||input.want_response||input.request_id))||
     (app==4&&!nodeInfoMatches(binding,input))||!bindingValid(ref,binding))return SigningResult::InvalidArgument;
  return signPrepared(keys,ref,binding,entropy,id,to,input,out,attachSignature);
}
[[gnu::noinline]] SigningResult PkiSigner::signPublicApplicationBroadcast(
  KeyProvider &keys,KeyRef ref,const IdentityBinding &binding,SigningEntropy &entropy,
  uint32_t id,const uint8_t *wire,size_t size,SignedData &out,bool attachSignature) {
  if(!id||!wire||!size||size>239||!bindingValid(ref,binding)||
     !mt::PublicApplication::canonical(wire,size))return SigningResult::InvalidArgument;
  Scratch<meshtastic_Data> input;auto stream=pb_istream_from_buffer(wire,size);
  if(!pb_decode(&stream,meshtastic_Data_fields,&input.value)||stream.bytes_left)
    return SigningResult::InvalidArgument;
  const uint32_t app=port(input.value);
  // These ports have dedicated typed module producers and must retain their
  // existing canonical payload admission instead of entering the opaque app path.
  if(bench::moduleBroadcastPort(app))return SigningResult::InvalidArgument;
  return signPrepared(keys,ref,binding,entropy,id,0xffffffffu,input.value,out,attachSignature);
}
SigningResult PkiSigner::signLicensedUnicast(KeyProvider &keys,KeyRef ref,const IdentityBinding &binding,
  SigningEntropy &entropy,uint32_t id,uint32_t to,const meshtastic_Data &input,SignedData &out,bool attachSignature){
  if(!bindingValid(ref,binding)||!licensedUnicastShape(binding,id,to,input))return SigningResult::InvalidArgument;
  return signPrepared(keys,ref,binding,entropy,id,to,input,out,attachSignature);
}
SigningResult PkiSigner::bootstrap(KeyProvider &keys,KeyRef ref,const IdentityBinding &binding,SigningEntropy &entropy,
                                   uint32_t id,uint32_t to,const NodeInfoProfile &profile,
                                   const SigningEnvelope &envelope,SignedData &out,bool attachSignature){
  // An unknown board can still publish its authenticated owner and public key.
  const bool validModel=profile.hardwareModel==meshtastic_HardwareModel_UNSET || board::supportedModel(profile.hardwareModel);
  if(!boundedName(profile.longName,sizeof(profile.longName))||!boundedName(profile.shortName,sizeof(profile.shortName))||
     !validModel||!DeviceRolePolicy::supportedRole(profile.role))return SigningResult::InvalidArgument;
  Scratch<meshtastic_User> user;
  snprintf(user.value.id,sizeof(user.value.id),"!%08x",unsigned(binding.node));
  memcpy(user.value.long_name,profile.longName,sizeof(profile.longName));
  memcpy(user.value.short_name,profile.shortName,sizeof(profile.shortName));
  user.value.hw_model=static_cast<meshtastic_HardwareModel>(profile.hardwareModel);
  user.value.role=static_cast<meshtastic_Config_DeviceConfig_Role>(profile.role);
  user.value.is_licensed=profile.licensed;
  user.value.public_key.size=32;memcpy(user.value.public_key.bytes,binding.publicKey,32);
  user.value.has_is_unmessagable=true;user.value.is_unmessagable=profile.ownerUnmessagable;
  Scratch<meshtastic_Data> data;data.value.portnum=meshtastic_PortNum_NODEINFO_APP;
  auto payload=pb_ostream_from_buffer(data.value.payload.bytes,sizeof(data.value.payload.bytes));
  if(!pb_encode(&payload,meshtastic_User_fields,&user.value))return SigningResult::TooLarge;
  data.value.payload.size=pb_size_t(payload.bytes_written);
  data.value.request_id=envelope.requestId;data.value.reply_id=envelope.replyId;data.value.emoji=envelope.emoji;
  data.value.want_response=envelope.wantResponse;data.value.has_bitfield=envelope.hasBitfield;data.value.bitfield=envelope.bitfield;
  Scratch<SignedData> result;
  const auto status=sign(keys,ref,binding,entropy,id,to,data.value,result.value,attachSignature);
  if(status!=SigningResult::Ok)return status;
  if(result.value.size>227)return SigningResult::TooLarge;
  out=result.value;return SigningResult::Ok;
}
}
