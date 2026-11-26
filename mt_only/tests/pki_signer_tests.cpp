// SPDX-License-Identifier: GPL-3.0-only
// Deterministic PUBLIC synthetic fixtures only. No device/credential backend.
#include "pki/PkiSigner.h"
#include "pki/PrivateApplication.h"
#include "MtSignature.h"
#include "monocypher.h"
#include "meshtastic/telemetry.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
extern "C" size_t mtPinnedSigningBuffer(uint32_t,uint32_t,uint32_t,const bench::RouteMetadata *,uint8_t *,size_t);
extern "C" bool mtPinnedSignatureVerify(const uint8_t[32],const uint8_t[64],const uint8_t *,size_t);
extern "C" void mtPinnedSignWithScalar(const uint8_t[32],const uint8_t[32],const uint8_t *,size_t,uint8_t[64]);
using namespace mt::pki;
static unsigned checks=0,vectors=0,mutations=0,wipes=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
// Only the candidate producer's wipe calls are redirected. Real Monocypher
// still clears the bytes; verify them before the scratch lifetime ends.
extern "C" void mtSignerObservedWipe(void *p,size_t n){
  crypto_wipe(p,n);const auto *bytes=static_cast<const uint8_t *>(p);
  for(size_t i=0;i<n;++i)CHECK(bytes[i]==0);
  ++wipes;
}
struct Provider:KeyProvider {
  uint8_t scalar[32]{};IdentityBinding binding;KeyRef ref{0,1};
  bool alive=true;int fault=0;unsigned borrows=0;
  explicit Provider(unsigned seed=1){
    for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed*29+i*17+3);
    scalar[0]&=0xf8;scalar[31]&=0x7f;scalar[31]|=0x40;
    crypto_x25519_public_key(binding.publicKey,scalar);binding.node=bench::MtSignature::nodeId(binding.publicKey);binding.generation=1;
  }
  ~Provider(){crypto_wipe(scalar,32);}
  bool active(KeyRef r)const override{return alive&&r.slot==ref.slot&&r.generation==ref.generation;}
  bool withPrivate(KeyRef r,PrivateKeyOperation &op)override{
    ++borrows;if(!active(r)||fault==1)return false;
    if(fault==4)return true;
    uint8_t wrong[32];memcpy(wrong,scalar,32);wrong[1]^=8;
    const bool ok=op.apply(fault==6?wrong:scalar);crypto_wipe(wrong,32);
    if(fault==2)return false;
    if(fault==3){(void)op.apply(scalar);return true;}
    if(fault==5)alive=false;
    return ok;
  }
};
struct Entropy:SigningEntropy {
  uint8_t bytes[32]{};bool fail=false;unsigned calls=0;Provider *revoke=nullptr;
  explicit Entropy(unsigned seed=0){for(unsigned i=0;i<32;++i)bytes[i]=uint8_t(seed+i*11+9);}
  bool hedge(uint8_t out[32])override{++calls;memcpy(out,bytes,32);if(revoke)revoke->alive=false;return !fail;}
};
static meshtastic_Data decode(const SignedData &out){
  meshtastic_Data d{};auto input=pb_istream_from_buffer(out.bytes,out.size);
  CHECK(out.size<=239&&pb_decode(&input,meshtastic_Data_fields,&d));return d;
}
static bench::RouteMetadata metadata(const meshtastic_Data &d){
  bench::RouteMetadata m;m.portnum=uint32_t(d.portnum);m.payloadLength=uint8_t(d.payload.size);
  memcpy(m.payload,d.payload.bytes,d.payload.size);m.wantResponse=d.want_response;
  m.request_id=d.request_id;m.reply_id=d.reply_id;m.emoji=d.emoji;
  m.hasBitfield=d.has_bitfield;m.bitfield=d.bitfield;return m;
}
static void compareStock(const Provider &p,const Entropy &e,const SignedData &out){
  const auto d=decode(out);CHECK(d.xeddsa_signature.size==64);
  auto m=metadata(d);uint8_t message[267],signature[64];
  const auto n=mtPinnedSigningBuffer(out.from,out.id,out.to,&m,message,sizeof(message));
  CHECK(n==size_t(34)+d.payload.size);
  mtPinnedSignWithScalar(p.scalar,e.bytes,message,n,signature);
  CHECK(!memcmp(signature,d.xeddsa_signature.bytes,64));
  CHECK(mtPinnedSignatureVerify(p.binding.publicKey,d.xeddsa_signature.bytes,message,n));++vectors;
}
static meshtastic_Data text(unsigned seed=0){
  meshtastic_Data d{};d.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
  d.payload.size=pb_size_t(seed%120);
  for(unsigned i=0;i<d.payload.size;++i)d.payload.bytes[i]=uint8_t(seed+i*7);
  d.request_id=0x12345678u*seed;d.reply_id=~d.request_id;d.emoji=seed*0x11223344;
  d.want_response=seed&1;d.has_bitfield=seed&2;d.bitfield=uint8_t(seed*3);return d;
}
static void differential(){
  unsigned sign0=0,sign1=0;
  for(unsigned i=0;i<96;++i){
    Provider p(i+1);Entropy e(i);SignedData out;const auto input=text(i);
    uint8_t ed[32];crypto_eddsa_scalarbase(ed,p.scalar);(ed[31]&128?sign1:sign0)++;
    CHECK(PkiSigner::ready(p,p.ref,p.binding));
    CHECK(PkiSigner::sign(p,p.ref,p.binding,e,i+1,0xffffffffu,input,out)==SigningResult::Ok);
    CHECK(out.from==p.binding.node&&out.to==0xffffffffu&&out.id==i+1);compareStock(p,e,out);
    const auto d=decode(out);
    CHECK(d.request_id==input.request_id&&d.reply_id==input.reply_id&&d.emoji==input.emoji);
    CHECK(d.has_bitfield==input.has_bitfield&&(!d.has_bitfield||d.bitfield==input.bitfield)&&d.want_response==input.want_response);
    for(unsigned change=0;change<11;++change){
      auto m=metadata(d);auto from=out.from,to=out.to,id=out.id;
      switch(change){
      case 0:from^=8;break;case 1:to^=8;break;case 2:id^=8;break;case 3:m.portnum=4;break;
      case 4:m.request_id^=8;break;case 5:m.reply_id^=8;break;case 6:m.emoji^=8;break;
      case 7:m.hasBitfield=!m.hasBitfield;break;
      case 8:m.hasBitfield=true;m.bitfield^=0x80;break;
      case 9:m.wantResponse=!m.wantResponse;break;
      default:if(m.payloadLength)m.payload[0]^=8;else{m.payloadLength=1;m.payload[0]=8;}break;
      }
      uint8_t message[267];auto n=mtPinnedSigningBuffer(from,id,to,&m,message,sizeof(message));
      CHECK(!mtPinnedSignatureVerify(p.binding.publicKey,d.xeddsa_signature.bytes,message,n));++mutations;
    }
    auto m=metadata(d);uint8_t message[267];const auto n=mtPinnedSigningBuffer(out.from,out.id,out.to,&m,message,sizeof(message));
    for(unsigned j=0;j<64;++j){uint8_t sig[64];memcpy(sig,d.xeddsa_signature.bytes,64);sig[j]^=1;
      CHECK(!mtPinnedSignatureVerify(p.binding.publicKey,sig,message,n));++mutations;}
  }
  CHECK(sign0&&sign1);std::puts("PASS exact_stock_xeddsa_bytes_both_scalar_signs_and_signed_metadata_mutations");
}
static void metricsBroadcast(){
  Provider p(199);Entropy e;SignedData out;meshtastic_Data d{};
  d.portnum=meshtastic_PortNum_TELEMETRY_APP;d.has_bitfield=true;
  // Canonical DeviceMetrics with explicitly present zero uptime, battery101,
  // and finite channel/airtime floats. Synthetic public fixture only.
  constexpr uint8_t payload[]={18,14,8,101,29,0,0,192,63,37,0,0,0,62,40,0};
  meshtastic_Telemetry telemetry{};
  auto input=pb_istream_from_buffer(payload,sizeof(payload));
  CHECK(pb_decode(&input,meshtastic_Telemetry_fields,&telemetry));
  CHECK(telemetry.which_variant==meshtastic_Telemetry_device_metrics_tag);
  const auto &metrics=telemetry.variant.device_metrics;
  CHECK(metrics.has_battery_level&&metrics.battery_level==101&&
    metrics.has_channel_utilization&&metrics.channel_utilization==1.5f&&
    metrics.has_air_util_tx&&metrics.air_util_tx==0.125f&&
    metrics.has_uptime_seconds&&metrics.uptime_seconds==0);
  uint8_t canonical[sizeof(payload)];auto output=pb_ostream_from_buffer(canonical,sizeof(canonical));
  CHECK(pb_encode(&output,meshtastic_Telemetry_fields,&telemetry)&&output.bytes_written==sizeof(payload)&&
    !memcmp(canonical,payload,sizeof(payload)));
  d.payload.size=sizeof(payload);memcpy(d.payload.bytes,payload,sizeof(payload));
  CHECK(PkiSigner::sign(p,p.ref,p.binding,e,300,0xffffffffu,d,out)==SigningResult::Ok);
  compareStock(p,e,out);const auto decoded=decode(out);
  CHECK(decoded.portnum==meshtastic_PortNum_TELEMETRY_APP&&decoded.payload.size==sizeof(payload)&&
    !memcmp(decoded.payload.bytes,payload,sizeof(payload)));
  std::puts("PASS canonical_metrics_broadcast_has_exact_stock_signature");
}
static void privateAdmission(){
  constexpr uint8_t query[]={8,67,18,2,18,0,24,1,72,2};
  std::vector<uint8_t> request(query,query+sizeof(query));
  CHECK(PrivateApplication::allowed(request.data(),request.size()));
  uint8_t response[40]{};size_t size=0;
  CHECK(mt::TelemetryQuery::uptimeResponse(123,77,response,sizeof(response),size));
  std::vector<uint8_t> reply(response,response+size);
  CHECK(PrivateApplication::allowed(reply.data(),reply.size()));
  for(const auto &base:{request,reply}){
    for(const std::vector<uint8_t> &tail:std::vector<std::vector<uint8_t>>{
      {37,1,0,0,0},{45,1,0,0,0},{37,0,0,0,0},{248,31,1},
      {8,67},{72,uint8_t(base==request?2:0)},{82,0}}){
      auto invalid=base;invalid.insert(invalid.end(),tail.begin(),tail.end());
      CHECK(!PrivateApplication::allowed(invalid.data(),invalid.size()));
    }
    auto reordered=base;reordered.erase(reordered.begin(),reordered.begin()+2);
    reordered.push_back(8);reordered.push_back(67);
    CHECK(!PrivateApplication::allowed(reordered.data(),reordered.size()));
  }
  auto absent=request;absent.resize(absent.size()-2);CHECK(!PrivateApplication::allowed(absent.data(),absent.size()));
  auto wrong=request;wrong[1]=5;CHECK(!PrivateApplication::allowed(wrong.data(),wrong.size()));
  CHECK(!PrivateApplication::allowed(nullptr,0));
  std::puts("PASS private_metrics_canonical_outer_Data_rejects_dest_source_unknown_duplicate_reordered_fields");
}
static void bootstrap(){
  Provider p(201);Entropy e;NodeInfoProfile profile;strcpy(profile.longName,"Synthetic maximum name X");strcpy(profile.shortName,"TEST");
  profile.hardwareModel=127;SigningEnvelope envelope;envelope.requestId=0xaabbccdd;envelope.replyId=0x12345678;
  envelope.emoji=0xffff0000;envelope.wantResponse=true;envelope.hasBitfield=true;envelope.bitfield=255;
  SignedData out;CHECK(PkiSigner::bootstrap(p,p.ref,p.binding,e,0x12345678,0xffffffffu,profile,envelope,out)==SigningResult::Ok);
  CHECK(out.size<=227);compareStock(p,e,out);auto d=decode(out);meshtastic_User user{};
  auto input=pb_istream_from_buffer(d.payload.bytes,d.payload.size);CHECK(pb_decode(&input,meshtastic_User_fields,&user));
  char expected[10];snprintf(expected,sizeof(expected),"!%08x",unsigned(p.binding.node));
  CHECK(!strcmp(user.id,expected)&&user.public_key.size==32&&!memcmp(user.public_key.bytes,p.binding.publicKey,32));
  CHECK(!strcmp(user.long_name,profile.longName)&&!strcmp(user.short_name,profile.shortName));
  CHECK(user.hw_model==127&&user.role==meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE&&!user.is_licensed);
  for(auto c:user.macaddr)CHECK(c==0);
  CHECK(user.has_is_unmessagable&&!user.is_unmessagable);
  CHECK(d.request_id==envelope.requestId&&d.reply_id==envelope.replyId&&d.emoji==envelope.emoji&&d.want_response&&d.has_bitfield&&d.bitfield==255);
  bench::RouteMetadata m;CHECK(bench::MtCodec::decodeData(out.bytes,out.size,m));m.channelDecoded=true;
  uint8_t frame[17]{};for(unsigned i=0;i<4;++i){frame[i]=uint8_t(out.to>>(8*i));frame[4+i]=uint8_t(out.from>>(8*i));frame[8+i]=uint8_t(out.id>>(8*i));}
  frame[13]=0xa7;bench::MtSignature receiver;
  CHECK(receiver.apply(frame,sizeof(frame),m,100)==bench::MtSignature::Result::Verified&&receiver.keyCount(100)==1);
  // Invalid claimed User identity is never signed, even with a valid key handle.
  for(unsigned fault=0;fault<2;++fault){
    auto malformed=d;malformed.xeddsa_signature.size=0;auto u=user;
    if(!fault)u.public_key.bytes[0]^=1;else strcpy(u.id,"!00000001");
    auto output=pb_ostream_from_buffer(malformed.payload.bytes,sizeof(malformed.payload.bytes));
    CHECK(pb_encode(&output,meshtastic_User_fields,&u));malformed.payload.size=pb_size_t(output.bytes_written);
    CHECK(PkiSigner::sign(p,p.ref,p.binding,e,2,0xffffffffu,malformed,out)==SigningResult::InvalidArgument);
  }
  for(int model=0;model<=255;++model) {
    if(model==28 || model==104 || (model>151 && model<255)) continue;
    profile.hardwareModel=uint8_t(model);
    CHECK(PkiSigner::bootstrap(p,p.ref,p.binding,e,200+model,0xffffffffu,profile,envelope,out)==SigningResult::Ok);
    compareStock(p,e,out);
    auto announcement=decode(out);
    auto payload=pb_istream_from_buffer(announcement.payload.bytes,announcement.payload.size);
    meshtastic_User advertised{};
    CHECK(pb_decode(&payload,meshtastic_User_fields,&advertised)&&advertised.hw_model==model);
    CHECK(!strcmp(advertised.long_name,profile.longName)&&!strcmp(advertised.short_name,profile.shortName));
  }
  const auto prior=out;
  const auto calls=e.calls,borrows=p.borrows;
  for(int unsupported=0;unsupported<=255;++unsupported) {
    if(unsupported!=28 && unsupported!=104 && !(unsupported>151 && unsupported<255)) continue;
    profile.hardwareModel=uint8_t(unsupported);
    CHECK(PkiSigner::bootstrap(p,p.ref,p.binding,e,602,0xffffffffu,profile,envelope,out)==SigningResult::InvalidArgument);
    CHECK(e.calls==calls&&p.borrows==borrows&&!memcmp(&prior,&out,sizeof(out)));
  }
  std::puts("PASS_authoritative_signed_NodeInfo_stock_verification_and_first_contact_admission");
}
static void failures(){
  for(int mode=0;mode<=8;++mode){
    Provider p(11);Entropy e;auto d=text(3);SignedData out;
    for(size_t i=0;i<sizeof(out);++i)reinterpret_cast<uint8_t *>(&out)[i]=0x5a;
    auto sentinel=out;
    if(mode==0)e.fail=true;else if(mode<=6)p.fault=mode;else if(mode==7)p.alive=false;else e.revoke=&p;
    CHECK(PkiSigner::sign(p,p.ref,p.binding,e,1,0xffffffffu,d,out)!=SigningResult::Ok);
    CHECK(!memcmp(&out,&sentinel,sizeof(out)));
  }
  for(int fault=1;fault<=6;++fault){Provider p;p.fault=fault;CHECK(!PkiSigner::ready(p,p.ref,p.binding));}
  for(unsigned mode=0;mode<14;++mode){
    Provider p;Entropy e;auto d=text();auto b=p.binding;auto ref=p.ref;uint32_t id=1,to=0xffffffffu;
    switch(mode){case 0:b.node^=8;break;case 1:b.publicKey[0]^=8;break;case 2:b.generation++;break;
      case 3:b.version=2;break;case 4:b.algorithm=2;break;case 5:ref.generation=0;break;
      case 6:id=0;break;case 7:to=1;break;case 8:to=b.node;break;
      case 9:d.dest=1;break;case 10:d.source=1;break;case 11:d.xeddsa_signature.size=1;break;
      case 12:d.portnum=meshtastic_PortNum_TRACEROUTE_APP;break;default:d.payload.size=234;break;}
    SignedData out;CHECK(PkiSigner::sign(p,ref,b,e,id,to,d,out)==SigningResult::InvalidArgument);
    CHECK(e.calls==0&&p.borrows==0);
  }
  Provider p;Entropy e;auto d=text();SignedData out;
  unsigned accepted=0,tooLarge=0;
  for(unsigned n=0;n<=233;++n){
    d.payload.size=pb_size_t(n);memset(d.payload.bytes,0x5a,n);const auto calls=e.calls;
    const auto result=PkiSigner::sign(p,p.ref,p.binding,e,n+1,0x12345678,d,out);
    if(result==SigningResult::Ok){++accepted;CHECK(out.size<=239);if(n%30==0)compareStock(p,e,out);}
    else{++tooLarge;CHECK(result==SigningResult::TooLarge&&e.calls==calls);}
  }
  CHECK(accepted&&tooLarge);
  auto initial=out;e.bytes[0]^=1;
  d.payload.size=10;
  CHECK(PkiSigner::sign(p,p.ref,p.binding,e,55,0xffffffffu,d,initial)==SigningResult::Ok);compareStock(p,e,initial);
  e.bytes[0]^=2;
  CHECK(PkiSigner::sign(p,p.ref,p.binding,e,55,0xffffffffu,d,out)==SigningResult::Ok);compareStock(p,e,out);
  CHECK(initial.size==out.size&&memcmp(initial.bytes,out.bytes,out.size));
  std::puts("PASS_provider_entropy_revocation_failures_atomic_output_and_encoded_size_boundaries");
}
struct Memory:mt::SettingsBackend {
  std::map<std::string,std::vector<uint8_t>> records;
  int read(const char *key,uint8_t *out,size_t cap)override{auto i=records.find(key);if(i==records.end())return -ENOENT;
    if(i->second.size()>cap)return -E2BIG;
    if(!i->second.empty())memcpy(out,i->second.data(),i->second.size());
    return int(i->second.size());}
  int write(const char *key,const uint8_t *p,size_t n)override{records[key]={p,p+n};return 0;}
};
static void realIdentity(){
  Provider fixture(43);Memory memory;IdentityJournal identity;MonocypherIdentityDeriver deriver;
  CHECK(identity.load(memory,deriver)==0);IdentityCandidate candidate(fixture.binding,fixture.scalar);
  CHECK(identity.store(memory,deriver,candidate,true)==0);IdentityBinding binding;KeyRef ref;
  CHECK(identity.binding(binding,ref)&&ref.slot==0&&PkiSigner::ready(identity,ref,binding));
  Entropy entropy;SignedData out;CHECK(PkiSigner::sign(identity,ref,binding,entropy,5,0xffffffffu,text(),out)==SigningResult::Ok);
  compareStock(fixture,entropy,out);CHECK(identity.close());auto sentinel=out;
  CHECK(!PkiSigner::ready(identity,ref,binding));
  CHECK(PkiSigner::sign(identity,ref,binding,entropy,6,0xffffffffu,text(),out)==SigningResult::NotReady&&!memcmp(&out,&sentinel,sizeof(out)));
  std::puts("PASS_real_memory_identity_provider_slot_zero_and_revocation");
}
int main(){differential();metricsBroadcast();privateAdmission();bootstrap();failures();realIdentity();
  CHECK(wipes>1000);std::printf("PASS %u candidate scratch wipes observed zero\n",wipes);
  std::printf("PASS %u checks, %u exact stock signing vectors, %u stock rejected mutations\n",checks,vectors,mutations);}
