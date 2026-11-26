// SPDX-License-Identifier: GPL-3.0-only
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE)
#include "../stock/optional/ReplyWire.h"
#endif
#include "PkiRuntime.h"
#include "../CorePorts.h"
#include "MtModulePorts.h"
#include "MtSignature.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "PrivateApplication.h"
#include "../PositionQuery.h"
#include "../PublicApplication.h"
#include "../TraceRoute.h"
#if defined(CONFIG_MT_RF_ADMIN_GET)
#include "../admin/RemoteAdminProtocol.h"
#include "../admin/RemoteAdminMutationProtocol.h"
#include "../admin/LegacyAdminProtocol.h"
#endif
#include <pb_decode.h>
#include <pb_encode.h>
#if defined(CONFIG_MT_GPIO_MODULES)
#include "../modules/GpioModuleWire.h"
#endif
#include "../modules/audio/AudioWire.h"
namespace mt::pki {
static_assert(bench::ModuleNodeStatusPort==meshtastic_PortNum_NODE_STATUS_APP);
static_assert(bench::ModuleRangeTestPort==meshtastic_PortNum_RANGE_TEST_APP);
static_assert(bench::ModuleTelemetryPort==meshtastic_PortNum_TELEMETRY_APP);
static_assert(bench::ModuleNeighborInfoPort==meshtastic_PortNum_NEIGHBORINFO_APP);
namespace {
struct Wipe {void *p;size_t n;~Wipe(){crypto_wipe(p,n);}};
// Match the new Status/Range/Neighbor ModuleAnnouncement envelope without
// a second decoded Data/telemetry scratch on the 8KiB target call chain.
[[gnu::noinline]] bool canonicalModuleBroadcast(const meshtastic_Data &input,const uint8_t *data,size_t n) {
  if(input.bitfield||input.reply_id||input.emoji||!input.payload.size||input.payload.size>233)return false;
  if(input.portnum==meshtastic_PortNum_AUDIO_APP&&!audio::AudioWire::validPayload(input.payload.bytes,input.payload.size))return false;
  if(input.portnum==meshtastic_PortNum_PAXCOUNTER_APP&&!audio::PaxcountWire::validPayload(input.payload.bytes,input.payload.size))return false;
  uint8_t expected[239]{};Wipe wipe{expected,sizeof(expected)};
  auto out=pb_ostream_from_buffer(expected,sizeof(expected));
  return pb_encode(&out,meshtastic_Data_fields,&input)&&out.bytes_written==n&&!memcmp(expected,data,n);
}
[[gnu::noinline]] bool canonicalPositionEnvelope(const meshtastic_Data &input,const uint8_t *data,size_t n) {
  if(input.portnum!=meshtastic_PortNum_POSITION_APP||!input.has_bitfield||input.bitfield>1||
     input.want_response||input.request_id||input.reply_id||input.emoji||input.dest||input.source||
     input.xeddsa_signature.size||!input.payload.size||input.payload.size>sizeof(input.payload.bytes))return false;
  meshtastic_Data canonical=input;Wipe canonicalWipe{&canonical,sizeof(canonical)};canonical.bitfield=0;
  uint8_t expected[239]{};Wipe wipe{expected,sizeof(expected)};
  auto out=pb_ostream_from_buffer(expected,sizeof(expected));
  if(!pb_encode(&out,meshtastic_Data_fields,&canonical)||
     !PositionQuery::canonicalBroadcast(expected,out.bytes_written))return false;
  // Validate the payload with the zero-bitfield predicate, but compare the
  // actual canonical wire with its original 0/1 bitfield preserved.
  out=pb_ostream_from_buffer(expected,sizeof(expected));
  return pb_encode(&out,meshtastic_Data_fields,&input)&&out.bytes_written==n&&!memcmp(expected,data,n);
}
enum class ControlForm:uint8_t {Invalid,Response,GpioRequest,Position,PositionBroadcast,Telemetry,Trace,LocalStats,RemoteTelemetry,Neighbor,KeyVerification,AdminResponse};
// Parser scratch leaves the call chain before durable storage reservation.
// The Qwen proposal's partition was corrected: only want_response Data uses
// the dedicated query encoders; ordinary replies retain their actual payload.
[[gnu::noinline]] ControlForm controlForm(const uint8_t *data,size_t n) {
  meshtastic_Data input{};Wipe iw{&input,sizeof(input)};
  auto stream=pb_istream_from_buffer(data,n);
  if(!pb_decode(&stream,meshtastic_Data_fields,&input)||stream.bytes_left||!input.has_bitfield||
     input.xeddsa_signature.size||input.dest||input.source)return ControlForm::Invalid;
  const auto port=input.portnum;
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE)
  if(stock::optional::moduleReply(input)&&stock::optional::exactWire(data,n,false))return ControlForm::Response;
#endif
#if defined(CONFIG_MT_RF_ADMIN_GET)
  if(port==meshtastic_PortNum_ADMIN_APP)
    return admin::legacy_protocol::canonicalResponse(data,n,input.request_id)?ControlForm::AdminResponse:ControlForm::Invalid;
#endif
  if(port==meshtastic_PortNum_KEY_VERIFICATION_APP)
    return PrivateApplication::keyVerification(data,n)?ControlForm::KeyVerification:ControlForm::Invalid;
#if defined(CONFIG_MT_GPIO_MODULES)
  if(port==meshtastic_PortNum_REMOTE_HARDWARE_APP) {
    if(PublicApplication::canonicalGpioRequest(data,n))return ControlForm::GpioRequest;
    return modules::GpioModuleWire::replyData(data,n)?ControlForm::Response:ControlForm::Invalid;
  }
#endif
#if defined(CONFIG_MT_TAK_TRACKER)
  if(port==meshtastic_PortNum_ATAK_PLUGIN_V2)
    return tak_pli_reply::canonicalData(data,n)?ControlForm::Response:ControlForm::Invalid;
#endif
#if defined(CONFIG_MT_PAXCOUNTER)
  if(port==meshtastic_PortNum_PAXCOUNTER_APP && input.request_id && !input.want_response &&
     input.has_bitfield && !input.bitfield && !input.reply_id && !input.emoji &&
     audio::PaxcountWire::validPayload(input.payload.bytes,input.payload.size)) {
    uint8_t canonical[40];auto out=pb_ostream_from_buffer(canonical,sizeof(canonical));
    return pb_encode(&out,meshtastic_Data_fields,&input) && out.bytes_written==n && !memcmp(canonical,data,n)
        ? ControlForm::Response : ControlForm::Invalid;
  }
#endif
  if(port!=meshtastic_PortNum_POSITION_APP&&port!=meshtastic_PortNum_ROUTING_APP&&
     port!=meshtastic_PortNum_TELEMETRY_APP&&port!=meshtastic_PortNum_TRACEROUTE_APP&&port!=meshtastic_PortNum_NEIGHBORINFO_APP)return ControlForm::Invalid;
  if(port==meshtastic_PortNum_NEIGHBORINFO_APP) {
    bench::RouteMetadata m;Wipe mw{&m,sizeof(m)};
    uint8_t canonical[239]{};Wipe cw{canonical,sizeof(canonical)};size_t size=0;
    if(!bench::MtCodec::decodeData(data,n,m))return ControlForm::Invalid;
    const bool valid=input.want_response?
      (NeighborQuery::requestShape(m)&&NeighborQuery::request(canonical,sizeof(canonical),size)):
      NeighborQuery::response(m,canonical,sizeof(canonical),size);
    return valid&&size==n&&!memcmp(data,canonical,n)?
      (input.want_response?ControlForm::Neighbor:ControlForm::Response):ControlForm::Invalid;
  }
  if(!input.want_response) {
    bench::RouteMetadata m;Wipe mw{&m,sizeof(m)};
    return bench::MtCodec::decodeData(data,n,m)&&m.hasBitfield&&!m.wantResponse&&!m.signatureLength?
      ControlForm::Response:ControlForm::Invalid;
  }
  if(port==meshtastic_PortNum_POSITION_APP) {
    constexpr uint8_t expected[]={8,3,24,1,72,2};
    if(n==sizeof(expected)&&!memcmp(data,expected,n))return ControlForm::Position;
    return ControlForm::Invalid;
  }
  if(port==meshtastic_PortNum_TELEMETRY_APP) {
    constexpr uint8_t expected[]={8,67,18,2,18,0,24,1,72,2};
    constexpr uint8_t stats[]={8,67,18,2,0x32,0,24,1,72,2};
    if(n==sizeof(expected)&&!memcmp(data,expected,n))return ControlForm::Telemetry;
    if(n==sizeof(stats)&&!memcmp(data,stats,n))return ControlForm::LocalStats;
    const auto variant=RemoteTelemetryQuery::requestVariant(input.payload.bytes,input.payload.size);
    uint8_t canonical[]={8,67,18,2,uint8_t((variant<<3)|2),0,24,1,72,2};
    return variant&&n==sizeof(canonical)&&!memcmp(data,canonical,n)?ControlForm::RemoteTelemetry:ControlForm::Invalid;
  }
  if(port==meshtastic_PortNum_TRACEROUTE_APP) {
    bench::RouteMetadata m;Wipe mw{&m,sizeof(m)};
    uint8_t rebuilt[239]{};Wipe rw{rebuilt,sizeof(rebuilt)};size_t size=0;
    if(bench::MtCodec::decodeData(data,n,m)&&TraceRoute::requestShape(m)&&
       TraceRoute::requestData(m.payload,m.payloadLength,rebuilt,sizeof(rebuilt),size)&&
       size==n&&!memcmp(rebuilt,data,n))return ControlForm::Trace;
  }
  return ControlForm::Invalid;
}
void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(i*8));}
// Deliberately keep verification scratch off the signer call chain. Both
// targets have an 8KiB main stack; inlining joins otherwise disjoint lifetimes.
[[gnu::noinline]] bool encodeBootstrap(bench::MtCodec &codec,const SignedData &signedData,
  const uint8_t publicKey[32],uint32_t local,uint32_t to,uint32_t id,bench::Frame &frame) {
  bench::RouteMetadata m;Wipe mw{&m,sizeof(m)};uint8_t ed[32]{},message[267]{};
  Wipe edw{ed,sizeof(ed)},msgw{message,sizeof(message)};
  if(!bench::MtCodec::decodeData(signedData.bytes,signedData.size,m)||m.portnum!=4||m.signatureLength!=64||
    m.publicKeyLength!=32||memcmp(m.publicKey,publicKey,32)||
    !bench::MtSignature::admissiblePublicKey(publicKey,ed))return false;
  const size_t signLength=bench::MtSignature::signingBuffer(local,id,to,m,message,sizeof(message));
  if(!signLength||crypto_ed25519_check(m.signature,ed,message,signLength))return false;
  size_t n=0;if(!codec.encodeSignedNodeInfo(local,to,id,signedData.bytes,signedData.size,frame.bytes,255,n))return false;
  frame.len=n;return true;
}
}
bool PkiPublicProducer::canonicalPositionBroadcast(const uint8_t *data,size_t n) {
  if(!data||!n||n>239)return false;
  meshtastic_Data input{};Wipe wipe{&input,sizeof(input)};
  auto stream=pb_istream_from_buffer(data,n);
  return pb_decode(&stream,meshtastic_Data_fields,&input)&&!stream.bytes_left&&
    canonicalPositionEnvelope(input,data,n);
}
bool PkiPublicProducer::bootstrap(KeyProvider &identity,KeyRef key,const IdentityBinding &binding,
  NonceAuthority &nonces,SigningEntropy &signing,bench::MtCodec &codec,
  const bench::MtNodeRecord &record,uint8_t model,uint8_t role,bool ownerUnmessagable,
  uint32_t to,uint32_t id,bool wantResponse,uint32_t request,bench::Frame &frame) {
  if(!identity.active(key)||
     (!record.valid()||(!record.licensedPlaintext()&&
       ((record.bytes[3]!=16&&record.bytes[3]!=32)||!codec.hasEncryptedKey()))||
      codec.licensedPlaintext()!=record.licensedPlaintext()||
      !codec.configured()||codec.channelHash()!=record.bytes[4])||!id||
     to<=1||to==0xfffffffeu||to==binding.node||(wantResponse&&request))return false;
  uint32_t extra=0;Wipe ew{&extra,sizeof(extra)};
  if(!codec.originAllowed()||!nonces.reserve(key,binding.publicKey,binding.node,id,extra))return false;
  NodeInfoProfile profile;profile.hardwareModel=model;profile.role=role;
  profile.ownerUnmessagable=ownerUnmessagable;
  profile.licensed=record.licensedPlaintext();
  memcpy(profile.longName,record.bytes+bench::MtNodeRecord::Name,24);
  memcpy(profile.shortName,record.bytes+bench::MtNodeRecord::Short,4);
  SigningEnvelope envelope;envelope.requestId=request;envelope.hasBitfield=true;
  envelope.bitfield=wantResponse?2:0;
  SignedData signedData;Wipe sw{&signedData,sizeof(signedData)};
  if(PkiSigner::bootstrap(identity,key,binding,signing,id,to,profile,envelope,signedData,codec.signsPublic())!=SigningResult::Ok)return false;
  if(codec.signsPublic())return encodeBootstrap(codec,signedData,binding.publicKey,binding.node,to,id,frame);
  size_t size=0;
  if(!codec.encodeZeroHopData(binding.node,to,id,signedData.bytes,signedData.size,frame.bytes,sizeof(frame.bytes),size))return false;
  frame.len=size;return true;
}
[[gnu::noinline]] bool PkiPublicProducer::broadcast(KeyProvider &identity,KeyRef key,
  const IdentityBinding &binding,NonceAuthority &nonces,SigningEntropy &signing,
  bench::MtCodec &codec,uint32_t id,const uint8_t *data,size_t n,bench::Frame &frame) {
  if(!identity.active(key)||!id||!data||!n||n>239)return false;
  meshtastic_Data input{};Wipe iw{&input,sizeof(input)};
  auto stream=pb_istream_from_buffer(data,n);
  if(!pb_decode(&stream,meshtastic_Data_fields,&input)||stream.bytes_left)return false;
  const bool publicApplicationBroadcast=input.portnum!=meshtastic_PortNum_TEXT_MESSAGE_APP&&
    input.portnum!=meshtastic_PortNum_POSITION_APP&&
    !bench::moduleBroadcastPort(uint32_t(input.portnum));
  const bool canonicalPublicApplication=publicApplicationBroadcast&&PublicApplication::canonical(data,n);
  if((input.portnum!=meshtastic_PortNum_TEXT_MESSAGE_APP&&!bench::moduleBroadcastPort(uint32_t(input.portnum))&&
      !(input.portnum==meshtastic_PortNum_POSITION_APP&&canonicalPositionEnvelope(input,data,n))&&
      !canonicalPublicApplication)||
     !input.has_bitfield||input.want_response||input.request_id||input.xeddsa_signature.size||
     (input.bitfield&~1u)||input.dest||input.source)return false;
  if(bench::publicSignedModulePort(uint32_t(input.portnum))&&
     !canonicalModuleBroadcast(input,data,n))return false;
  uint32_t extra=0;Wipe ew{&extra,sizeof(extra)};
  if(!codec.originAllowed()||!nonces.reserve(key,binding.publicKey,binding.node,id,extra))return false;
  SignedData signedData;Wipe sw{&signedData,sizeof(signedData)};
  const auto signedResult=canonicalPublicApplication?
    PkiSigner::signPublicApplicationBroadcast(identity,key,binding,signing,id,data,n,signedData,codec.signsPublic()):
    PkiSigner::sign(identity,key,binding,signing,id,0xffffffffu,input,signedData,codec.signsPublic());
  if((signedResult==SigningResult::TooLarge||(signedResult==SigningResult::Ok&&!codec.signsPublic()))&&
     (input.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP||input.portnum==meshtastic_PortNum_POSITION_APP||
      canonicalPublicApplication||
      (bench::moduleBroadcastPort(uint32_t(input.portnum))&&canonicalModuleBroadcast(input,data,n)))) {
    size_t size=0;
    if(!codec.encodeZeroHopData(binding.node,0xffffffffu,id,data,n,frame.bytes,sizeof(frame.bytes),size))return false;
    frame.len=size;return true;
  }
  if(signedResult!=SigningResult::Ok)return false;
  size_t size=0;
  if(!codec.encodeSignedApplication(binding.node,id,signedData.bytes,signedData.size,frame.bytes,sizeof(frame.bytes),size))return false;
  frame.len=size;return true;
}
[[gnu::noinline]] bool PkiPublicProducer::licensedUnicast(KeyProvider &identity,KeyRef key,
  const IdentityBinding &binding,NonceAuthority &nonces,SigningEntropy &signing,
  bench::MtCodec &codec,uint32_t to,uint32_t id,const uint8_t *data,size_t n,bench::Frame &out) {
  if(!identity.active(key)||!codec.configured()||!id||
     to<=1||to>=0xfffffffeu||to==binding.node||!data||!n||n>239)return false;
  meshtastic_Data input{};Wipe iw{&input,sizeof(input)};
  auto stream=pb_istream_from_buffer(data,n);
  if(!pb_decode(&stream,meshtastic_Data_fields,&input)||stream.bytes_left||input.xeddsa_signature.size||
     input.dest||input.source||(!codec.licensedPlaintext()&&
       (input.portnum!=meshtastic_PortNum_POSITION_APP||!codec.signsPublic())))return false;
  // Nanopb must not silently drop duplicate or unknown request fields before
  // a signed envelope is produced. Admission of supported ports/controls is
  // repeated in the signer and the Service owner.
  uint8_t canonical[239]{};Wipe cw{canonical,sizeof(canonical)};
  auto encoded=pb_ostream_from_buffer(canonical,sizeof(canonical));
  if(!pb_encode(&encoded,meshtastic_Data_fields,&input)||encoded.bytes_written!=n||memcmp(canonical,data,n))return false;
  uint32_t extra=0;Wipe ew{&extra,sizeof(extra)};
  if(!codec.originAllowed()||!nonces.reserve(key,binding.publicKey,binding.node,id,extra))return false;
  SignedData signedData{};Wipe sw{&signedData,sizeof(signedData)};
  const bool sign=codec.signsPublic()&&input.portnum!=meshtastic_PortNum_TRACEROUTE_APP&&input.portnum!=meshtastic_PortNum_ROUTING_APP;
  const auto result=PkiSigner::signLicensedUnicast(identity,key,binding,signing,id,to,input,signedData,sign);
  bench::Frame candidate{};Wipe fw{&candidate,sizeof(candidate)};size_t size=0;
  if(result==SigningResult::TooLarge){
    if(!codec.encodeZeroHopData(binding.node,to,id,data,n,candidate.bytes,sizeof(candidate.bytes),size))return false;
  }else if(result!=SigningResult::Ok||!codec.encodeProducedUnicast(binding.node,to,id,
      signedData.bytes,signedData.size,candidate.bytes,sizeof(candidate.bytes),size,sign))return false;
  if(!identity.active(key))return false;
  candidate.len=uint16_t(size);out=candidate;return true;
}
[[gnu::noinline]] bool PkiPublicProducer::encodePublicApplication(KeyProvider &identity,KeyRef key,
  const IdentityBinding &binding,NonceAuthority &nonces,bench::MtCodec &codec,
  uint32_t to,uint32_t id,const uint8_t *data,size_t n,bench::Frame &out) {
  if(!identity.active(key)||!codec.configured()||(!codec.hasEncryptedKey()&&!codec.licensedPlaintext())||!id||
     !PublicApplication::destination(to)||to==binding.node||!PublicApplication::canonical(data,n))return false;
  uint32_t extra=0;Wipe ew{&extra,sizeof(extra)};
  if(!codec.originAllowed()||!nonces.reserve(key,binding.publicKey,binding.node,id,extra))return false;
  bench::Frame candidate;Wipe fw{&candidate,sizeof(candidate)};size_t size=0;
  if(!codec.encodeZeroHopData(binding.node,to,id,data,n,candidate.bytes,sizeof(candidate.bytes),size)||
     !identity.active(key))return false;
  candidate.len=size;out=candidate;return true;
}
[[gnu::noinline]] bool PkiPublicProducer::encodeChannelControl(KeyProvider &identity,KeyRef key,
  const IdentityBinding &binding,NonceAuthority &nonces,bench::MtCodec &codec,
  uint32_t to,uint32_t id,const uint8_t *data,size_t n,bench::Frame &out) {
  if(!identity.active(key)||!codec.configured()||(!codec.hasEncryptedKey()&&!codec.licensedPlaintext())||
     to<=1||to==binding.node||!id||!data||!n||n>239)return false;
  const auto form=to==0xffffffffu?
    (PositionQuery::canonicalBroadcast(data,n)?ControlForm::PositionBroadcast:ControlForm::Invalid):controlForm(data,n);
  if(form==ControlForm::Invalid||(to>=0xfffffffeu&&to!=0xffffffffu))return false;
  if(form==ControlForm::PositionBroadcast)return false;
  uint32_t extra=0;Wipe ew{&extra,sizeof(extra)};
  if(!codec.originAllowed()||!nonces.reserve(key,binding.publicKey,binding.node,id,extra))return false;
  bench::Frame candidate;Wipe fw{&candidate,sizeof(candidate)};size_t size=0;bool encoded=false;
  switch(form) {
#if defined(CONFIG_MT_RF_ADMIN_GET)
  case ControlForm::AdminResponse:
    encoded=codec.encodeAdminChannelResponse(binding.node,to,id,data,n,candidate.bytes,sizeof(candidate.bytes),size);break;
#endif
  case ControlForm::Neighbor:encoded=codec.encodeNeighborQuery(binding.node,to,id,candidate.bytes,sizeof(candidate.bytes),size);break;
  case ControlForm::Position:encoded=codec.encodePositionQuery(binding.node,to,id,candidate.bytes,sizeof(candidate.bytes),size);break;
  case ControlForm::PositionBroadcast:return false; // Runtime routes canonical position broadcasts through signed broadcast().
  case ControlForm::Telemetry:encoded=codec.encodeTelemetryQuery(binding.node,to,id,candidate.bytes,sizeof(candidate.bytes),size);break;
  case ControlForm::LocalStats:encoded=codec.encodeTelemetryQuery(binding.node,to,id,candidate.bytes,sizeof(candidate.bytes),size,true);break;
  case ControlForm::Trace:encoded=codec.encodeTraceRouteRequest(binding.node,to,id,data,n,candidate.bytes,sizeof(candidate.bytes),size);break;
  case ControlForm::RemoteTelemetry:encoded=codec.encodeTelemetryQuery(binding.node,to,id,candidate.bytes,sizeof(candidate.bytes),size,false,uint8_t(data[4]>>3));break;
  case ControlForm::Response:encoded=codec.encodeZeroHopData(binding.node,to,id,data,n,candidate.bytes,sizeof(candidate.bytes),size);break;
  case ControlForm::KeyVerification:encoded=codec.encodeZeroHopData(binding.node,to,id,data,n,candidate.bytes,sizeof(candidate.bytes),size);break;
#if defined(CONFIG_MT_GPIO_MODULES)
  case ControlForm::GpioRequest:encoded=codec.encodeGpioRequest(binding.node,to,id,data,n,candidate.bytes,sizeof(candidate.bytes),size);break;
#endif
  default:return false;
  }
  if(!encoded||!identity.active(key))return false;
  candidate.len=size;out=candidate;return true;
}
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
[[gnu::noinline]] bool PkiPublicProducer::encodeStoreForward(KeyProvider &identity,KeyRef key,
  const IdentityBinding &binding,NonceAuthority &nonces,bench::MtCodec &codec,
  const modules::StoreForwardPacket &packet,uint32_t id,bench::Frame &out) {
  if(!identity.active(key)||!codec.configured()||(!codec.hasEncryptedKey()&&!codec.licensedPlaintext())||
     !packet.valid()||!id)return false;
  uint32_t extra=0;Wipe ew{&extra,sizeof(extra)};
  if(!codec.originAllowed()||!nonces.reserve(key,binding.publicKey,binding.node,id,extra))return false;
  bench::Frame candidate;Wipe fw{&candidate,sizeof(candidate)};size_t size=0;
  if(!codec.encodeZeroHopData(packet.from(),packet.to(),id,packet.data(),packet.size(),
      candidate.bytes,sizeof(candidate.bytes),size)||!identity.active(key))return false;
  candidate.len=size;out=candidate;return true;
}
#endif
bool PkiRuntime::open(uint32_t local,uint32_t peer) {
  if(opened_||frozen_)return false;
  opened_=true;peer_=peer;
  uint8_t ed[32]{};Wipe ew{ed,32};
  if(!backend_.healthy()||!identity_.binding(binding_,key_)||binding_.node!=local||local==peer||
    bench::MtSignature::nodeId(binding_.publicKey)!=local||
    !bench::MtSignature::admissiblePublicKey(binding_.publicKey,ed)||
    !peers_.boundTo(binding_.publicKey,local)||!peers_.lookup(peer,peerPublic_)||
    !peers_.active(peer,peerPublic_)||peers_.revoked(peer)||
    !bench::MtSignature::admissiblePublicKey(peerPublic_,ed)||
    !session_.open(key_,local,binding_.publicKey,peer,peerPublic_)){freeze();return false;}
  return true;
}
bool PkiRuntime::openVerification(uint32_t local) {
  if(opened_||frozen_||!verification_||local<=1||local>=0xfffffffeu)return false;
  opened_=true;peer_=0;verificationOwner_=true;
  uint8_t ed[32]{};Wipe ew{ed,sizeof(ed)};
  if(!backend_.healthy()||!identity_.binding(binding_,key_)||binding_.node!=local||
     !identity_.active(key_)||
     bench::MtSignature::nodeId(binding_.publicKey)!=local||
     !bench::MtSignature::admissiblePublicKey(binding_.publicKey,ed)||
     !peers_.boundTo(binding_.publicKey,local)) {freeze();return false;}
  return true;
}
bool PkiRuntime::keyVerificationRandom(uint8_t *out,size_t length) {
  if(!out||!length||length>32||!signing_||!verification_||!opened_||frozen_||
     !backend_.healthy()||!identity_.active(key_)||!peers_.boundTo(binding_.publicKey,binding_.node))return false;
  uint8_t material[32]{};Wipe wipe{material,sizeof(material)};
  if(!signing_->hedge(material))return false;
  memcpy(out,material,length);return true;
}
bool PkiRuntime::keyVerificationSha256(const uint8_t *data,size_t length,uint8_t out[32]) {
  if(!data||!length||!out||!verification_||!opened_||frozen_||!backend_.healthy()||
     !identity_.active(key_)||!peers_.boundTo(binding_.publicKey,binding_.node))return false;
  return backend_.sha256(data,length,out);
}
bool PkiRuntime::keyVerificationLocalIdentity(uint32_t &node,uint8_t out[32])const {
  node=0;if(!out||!verification_||!opened_||frozen_||!backend_.healthy()||
     !identity_.active(key_)||!peers_.boundTo(binding_.publicKey,binding_.node))return false;
  node=binding_.node;memcpy(out,binding_.publicKey,32);return true;
}
bool PkiRuntime::keyVerificationPeerKey(uint32_t node,uint8_t out[32])const {
  if(!out||!verification_||!opened_||frozen_)return false;
  if(node<=1||node>=0xfffffffeu||node==binding_.node)return false;
  uint8_t key[32]{};
  const bool pinned=peers_.lookup(node,key)&&peers_.active(node,key)&&!peers_.revoked(node)&&
    bench::MtSignature::nodeId(key)==node;
  if(pinned)memcpy(out,key,32);
  crypto_wipe(key,sizeof(key));return pinned;
}
int PkiRuntime::keyVerificationStageCandidate(uint32_t node,const uint8_t publicKey[32]) {
  if(!verification_||!opened_||frozen_||!backend_.healthy()||!identity_.active(key_)||
     !peers_.boundTo(binding_.publicKey,binding_.node)||!publicKey||node<=1||node>=0xfffffffeu||node==binding_.node||
     bench::MtSignature::nodeId(publicKey)!=node||peers_.revoked(node))return -EINVAL;
  uint8_t current[32]{};Wipe wipe{current,sizeof(current)};
  const bool hasStoredKey=peers_.lookup(node,current);
  const bool alreadyPinned=hasStoredKey&&peers_.active(node,current)&&!peers_.revoked(node);
  if(hasStoredKey&&!alreadyPinned)return -EACCES;
  if(alreadyPinned&&crypto_verify32(current,publicKey))return -EKEYREJECTED;
  auto &v=*verification_;
  if(v.peer)return v.peer==node&&crypto_verify32(v.publicKey,publicKey)==0?0:-EBUSY;
  v.duplicateAck.clear();
  if(!v.session)v.session=new (v.sessionStorage) PkiSession(crypto_,identity_,nonces_);
  if(!v.session->open(key_,binding_.node,binding_.publicKey,node,publicKey)) {
    v.session->~PkiSession();v.session=nullptr;
    crypto_wipe(v.sessionStorage,sizeof(v.sessionStorage));return -EPERM;
  }
  v.peer=node;v.peerPinned=alreadyPinned;memcpy(v.publicKey,publicKey,32);return 0;
}
void PkiRuntime::keyVerificationClearCandidate(uint32_t node,const uint8_t publicKey[32]) {
  if(!verification_)return;auto &v=*verification_;
  if(!v.peer||node!=v.peer||!publicKey||crypto_verify32(v.publicKey,publicKey))return;
  if(v.peerPinned)return;
  if(v.session){v.session->~PkiSession();v.session=nullptr;crypto_wipe(v.sessionStorage,sizeof(v.sessionStorage));}
  v.duplicateAck.clear();
  v.peer=0;v.peerPinned=false;crypto_wipe(v.publicKey,sizeof(v.publicKey));
}
void PkiRuntime::keyVerificationReleasePeer(uint32_t node,const uint8_t publicKey[32]) {
  if(!verification_)return;auto &v=*verification_;
  if(!v.peerPinned||!v.peer||node!=v.peer||!publicKey||crypto_verify32(v.publicKey,publicKey))return;
  if(v.session){v.session->~PkiSession();v.session=nullptr;crypto_wipe(v.sessionStorage,sizeof(v.sessionStorage));}
  v.duplicateAck.clear();
  v.peer=0;v.peerPinned=false;crypto_wipe(v.publicKey,sizeof(v.publicKey));
}
int PkiRuntime::keyVerificationSend(Endpoint &ep,bench::MtCodec &codec,uint32_t peer,
    uint32_t replyTo,const uint8_t *data,size_t length,bool pki,const uint8_t *explicitKey,
    size_t keyLength,unsigned hop,bool reliable,uint32_t now,uint32_t random) {
  if(!verification_||!opened_||frozen_||!backend_.healthy()||!identity_.active(key_)||
     !peers_.boundTo(binding_.publicKey,binding_.node)||peer<=1||peer>=0xfffffffeu||peer==binding_.node||
     !data||!length||length>227||hop>HopPolicy::Maximum||!PrivateApplication::keyVerification(data,length))return -EINVAL;
  const uint32_t id=ep.generatedId(random);if(!id)return -EAGAIN;
  bench::Frame frameOut{};Wipe wipeFrame{&frameOut,sizeof(frameOut)};
  if(pki) {
    PkiSession *selected=nullptr;
    if(peer==peer_&&peer_&&ready(binding_.node)) {
      if(!explicitKey||keyLength!=32||crypto_verify32(explicitKey,peerPublic_))return -EKEYREJECTED;
      selected=&session_;
    } else {
      if(peer!=verification_->peer&&explicitKey&&keyLength==32)
        (void)keyVerificationStageCandidate(peer,explicitKey);
    }
    if(!selected&&peer==verification_->peer&&verification_->session&&verification_->session->ready()) {
      if(!explicitKey||keyLength!=32||crypto_verify32(explicitKey,verification_->publicKey))return -EKEYREJECTED;
      selected=verification_->session;
    }
    if(!selected)return -EACCES;
    PkiFrame encrypted{};Wipe wipeEncrypted{&encrypted,sizeof(encrypted)};
    const auto result=selected->send(id,data,length,encrypted);
    if(result!=SessionResult::Ok)return result==SessionResult::NoSlot?-ENOBUFS:-EPERM;
    put(frameOut.bytes,peer);put(frameOut.bytes+4,binding_.node);put(frameOut.bytes+8,id);
    frameOut.bytes[12]=uint8_t(hop|(hop<<5)|(reliable?8u:0u));
    frameOut.bytes[15]=uint8_t(binding_.node);memcpy(frameOut.bytes+16,encrypted.bytes,encrypted.size);
    frameOut.len=16+encrypted.size;
    const int queued=ep.submitPrivateControl(frameOut,replyTo,now);
    (void)selected->retire(id);return queued;
  }
  uint8_t pinned[32]{};Wipe wipePinned{pinned,sizeof(pinned)};
  const bool alreadyPinned=peers_.lookup(peer,pinned);
  if(keyLength||explicitKey||peer==peer_||alreadyPinned)return -EKEYREJECTED;
  if(!PkiPublicProducer::encodeChannelControl(identity_,key_,binding_,nonces_,codec,
      peer,id,data,length,frameOut))return -EPERM;
  frameOut.bytes[12]=uint8_t(hop|(hop<<5)|(reliable?8u:0u));
  return ep.submitModuleControl(frameOut,now,false,false,false,reliable);
}
bool PkiRuntime::ready(uint32_t local)const {
  return opened_&&!frozen_&&backend_.healthy()&&local==binding_.node&&identity_.active(key_)&&session_.ready()&&
    peers_.boundTo(binding_.publicKey,local)&&peers_.active(peer_,peerPublic_);
}
bool PkiRuntime::publicIdentity(uint8_t out[32])const {
  if(!out||!signing_||!ready(binding_.node)||!PkiSigner::ready(identity_,key_,binding_))return false;
  memcpy(out,binding_.publicKey,32);return true;
}
bool PkiRuntime::bootstrap(bench::MtCodec &codec,const bench::MtNodeRecord &record,uint8_t model,
  uint32_t to,uint32_t id,bool wantResponse,uint32_t request,bench::Frame &frame) {
  return signing_&&ready(binding_.node)&&PkiPublicProducer::bootstrap(identity_,key_,binding_,nonces_,
    *signing_,codec,record,model,deviceRole_,ownerUnmessagable_,to,id,wantResponse,request,frame);
}
bool PkiRuntime::licensedUnicast(bench::MtCodec &codec,uint32_t to,uint32_t id,
  const uint8_t *data,size_t n,bench::Frame &out) {
  return signing_&&ready(binding_.node)&&PkiPublicProducer::licensedUnicast(identity_,key_,binding_,nonces_,
    *signing_,codec,to,id,data,n,out);
}
void PkiRuntime::clearEvent(){authenticatedRxFrom_=0;crypto_wipe(&event_,sizeof(event_));crypto_wipe(&eventFrame_,sizeof(eventFrame_));eventOwners_=0;}
void PkiRuntime::freeze(){frozen_=true;clearEvent();duplicateAck_.clear();crypto_wipe(pending_,sizeof(pending_));
  if(verificationOwner_&&verification_){
    crypto_wipe(&verification_->receive,sizeof(verification_->receive));verification_->receiveReady=false;
    verification_->duplicateAck.clear();
    if(verification_->session){verification_->session->~PkiSession();verification_->session=nullptr;
      crypto_wipe(verification_->sessionStorage,sizeof(verification_->sessionStorage));}
    verification_->peer=0;verification_->peerPinned=false;crypto_wipe(verification_->publicKey,sizeof(verification_->publicKey));
  }
  session_.close();}
bool PkiRuntime::takeKeyVerificationReceive(KeyVerificationReceive &out) {
  out={};if(!verification_||!verification_->receiveReady)return false;
  out=verification_->receive;crypto_wipe(&verification_->receive,sizeof(verification_->receive));
  verification_->receiveReady=false;return true;
}
bool PkiRuntime::revoke(uint32_t peer) {
  if(!matchesPeer(peer))return true;
  const bool ok=identity_.active(key_)&&peers_.boundTo(binding_.publicKey,binding_.node)&&
    (peers_.revoked(peer_)||peers_.revokeTrustedPeer(peer_,peerPublic_));
  freeze();return ok;
}
bool PkiRuntime::application(const uint8_t *p,size_t n)const {
  return PrivateApplication::allowed(p,n);
}
[[gnu::noinline]] bool PkiRuntime::broadcast(bench::MtCodec &codec,uint32_t id,
                                            const uint8_t *data,size_t n,bench::Frame &frame) {
  return signing_&&ready(binding_.node)&&PkiPublicProducer::broadcast(identity_,key_,binding_,nonces_,
    *signing_,codec,id,data,n,frame);
}
[[gnu::noinline]] bool PkiRuntime::encodeChannelControl(bench::MtCodec &codec,uint32_t to,
  uint32_t id,const uint8_t *data,size_t n,bench::Frame &out) {
  if(codec.licensedPlaintext()&&to!=0xffffffffu)return licensedUnicast(codec,to,id,data,n,out);
    if(to!=UINT32_MAX&&codec.signsPublic()&&data&&n>=2&&data[0]==8&&data[1]==3)
      return signing_&&ready(binding_.node)&&PkiPublicProducer::licensedUnicast(identity_,key_,binding_,nonces_,*signing_,
        codec,to,id,data,n,out)&&ready(binding_.node);
  if(to==0xffffffffu&&signing_&&PkiPublicProducer::canonicalPositionBroadcast(data,n))
    return broadcast(codec,id,data,n,out);
  return ready(binding_.node)&&PkiPublicProducer::encodeChannelControl(identity_,key_,binding_,nonces_,
    codec,to,id,data,n,out)&&ready(binding_.node);
}
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
[[gnu::noinline]] bool PkiRuntime::encodeStoreForward(bench::MtCodec &codec,
  const modules::StoreForwardPacket &packet,uint32_t id,bench::Frame &out) {
  return ready(binding_.node)&&PkiPublicProducer::encodeStoreForward(identity_,key_,binding_,nonces_,
    codec,packet,id,out)&&ready(binding_.node);
}
#endif
[[gnu::noinline]] bool PkiRuntime::encodePublicApplication(bench::MtCodec &codec,uint32_t to,
  uint32_t id,const uint8_t *data,size_t n,bench::Frame &out) {
  if(codec.licensedPlaintext()&&to!=0xffffffffu)return licensedUnicast(codec,to,id,data,n,out);
  if(to==0xffffffffu)return broadcast(codec,id,data,n,out);
  return ready(binding_.node)&&PkiPublicProducer::encodePublicApplication(identity_,key_,binding_,
    nonces_,codec,to,id,data,n,out)&&ready(binding_.node);
}
void PkiRuntime::sweep(Endpoint &ep) {
  if(!ready(binding_.node)){ep.cancelPrivate();clearEvent();duplicateAck_.clear();return;}
  for(auto &p:pending_)if(p.id&&!ep.pendingPrivate(p.id)){
    session_.retire(p.id);crypto_wipe(&p,sizeof(p));
  }
}
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE)
int PkiRuntime::stockReply(Endpoint &ep,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                           unsigned hop,bool reliable,uint32_t request,uint32_t now) {
  if(to!=peer_||!ready(binding_.node)||!id||hop>HopPolicy::Maximum||!ep.moduleControlAvailable()||
     !stock::optional::exactWire(data,n,false))return -EINVAL;
  bench::Frame encrypted{};Wipe wipe{&encrypted,sizeof(encrypted)};
  if(!frame(id,data,n,hop,reliable,encrypted))return -EPERM;
  const int queued=ep.submitModuleControl(encrypted,now,false,false,true,reliable,request);
  (void)session_.retire(id);return queued;
}
#endif
bool PkiRuntime::frame(uint32_t id,const uint8_t *data,size_t n,unsigned hop,bool reliable,bench::Frame &out) {
  PkiFrame encrypted;Wipe w{&encrypted,sizeof(encrypted)};
  const auto result=session_.send(id,data,n,encrypted);
  if(result!=SessionResult::Ok) {
    if(result==SessionResult::CryptoFailure||result==SessionResult::KeyUnavailable)freeze();
    return false;
  }
  out={};put(out.bytes,peer_);put(out.bytes+4,binding_.node);put(out.bytes+8,id);
  out.bytes[12]=uint8_t(hop|(hop<<5)|(reliable?8:0));out.bytes[15]=uint8_t(binding_.node);
  memcpy(out.bytes+16,encrypted.bytes,encrypted.size);out.len=16+encrypted.size;return true;
}
#if !defined(CONFIG_MT_RF_ADMIN_GET)
int PkiRuntime::submit(Endpoint &ep,uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
  unsigned hop,bool reliable,const uint8_t *explicitKey,size_t keySize,bool /*explicitPki*/,uint32_t now) {
  if(!ready(binding_.node))return -EPERM;
  if((owner>=LocalClientCount&&owner!=Endpoint::LocalUiOwner)||to!=peer_||!id||hop>HopPolicy::Maximum||!application(data,n)||
    (keySize&&(keySize!=32||!explicitKey||memcmp(explicitKey,peerPublic_,32))))return -EINVAL;
  if(owner==Endpoint::LocalUiOwner&&!ep.localUiSubmissionAllowed(id))return -EBUSY;
  bench::RouteMetadata metadata;Wipe mw{&metadata,sizeof(metadata)};
  if(!bench::MtCodec::decodeData(data,n,metadata))return -EINVAL;
  const bool telemetryQuery=metadata.portnum==meshtastic_PortNum_TELEMETRY_APP&&metadata.wantResponse;
  const bool positionQuery=metadata.portnum==meshtastic_PortNum_POSITION_APP&&metadata.wantResponse;
  const bool neighborQuery=metadata.portnum==meshtastic_PortNum_NEIGHBORINFO_APP&&metadata.wantResponse;
  const bool query=telemetryQuery||positionQuery||neighborQuery;
  const uint8_t variant=telemetryQuery?RemoteTelemetryQuery::requestVariant(metadata.payload,metadata.payloadLength):0;
  const bool localStats=variant==6;
  sweep(ep);
  uint8_t digest[32]{};Wipe wipe{digest,32};
  if(!backend_.sha256(data,n,digest)){freeze();return -EPERM;}
  Pending *slot=nullptr;
  for(auto &p:pending_) {
    if(p.id==id) {
      if(p.hop!=hop||p.reliable!=reliable||crypto_verify32(p.digest,digest))return -EINVAL;
      PkiFrame cached;Wipe w{&cached,sizeof(cached)};
      if(session_.retry(id,cached)!=SessionResult::Ok)return -EPERM;
      bench::Frame f;put(f.bytes,to);put(f.bytes+4,binding_.node);put(f.bytes+8,id);
      f.bytes[12]=uint8_t(hop|(hop<<5)|(reliable?8:0));f.bytes[13]=f.bytes[14]=0;f.bytes[15]=uint8_t(binding_.node);
      memcpy(f.bytes+16,cached.bytes,cached.size);f.len=cached.size+16;
      return ep.submit(owner,f.bytes,f.len,reliable,now,false,true,query,nullptr,true,false,positionQuery,telemetryQuery,false,localStats,variant,neighborQuery);
    }
    if(!p.id&&!slot)slot=&p;
  }
  if(!slot||!ep.free())return -ENOBUFS;
  bench::Frame f;
  if(!frame(id,data,n,hop,reliable,f))return -EPERM;
  const int ret=ep.submit(owner,f.bytes,f.len,reliable,now,false,true,query,nullptr,true,false,positionQuery,telemetryQuery,false,localStats,variant,neighborQuery);
  if(ret){session_.retire(id);return ret;}
  slot->id=id;slot->hop=hop;slot->reliable=reliable;memcpy(slot->digest,digest,32);return 0;
}
#else
int PkiRuntime::submit(Endpoint &ep,uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
  unsigned hop,bool reliable,const uint8_t *explicitKey,size_t keySize,bool explicitPki,uint32_t now) {
  return submitImpl(ep,owner,to,id,data,n,hop,reliable,explicitKey,keySize,explicitPki,now,false);
}
#if defined(CONFIG_MT_RF_ADMIN_GET)
int PkiRuntime::submitAdminGet(Endpoint &ep,uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
  unsigned hop,bool reliable,const uint8_t *explicitKey,size_t keySize,bool explicitPki,uint32_t now) {
  return submitImpl(ep,owner,to,id,data,n,hop,reliable,explicitKey,keySize,explicitPki,now,true);
}
bool PkiRuntime::respondPrivateAdminMutation(Endpoint &ep,uint32_t local,uint32_t peer,
    const uint8_t key[32],uint32_t request,uint8_t responseHop,bool reliable,
    uint32_t routingError,uint32_t now,uint32_t random) {
  if(!ready(binding_.node)||local!=binding_.node||peer!=peer_||!key||!request||
     responseHop>HopPolicy::Maximum||!(routingError<=9||(routingError>=32&&routingError<=39))||
     memcmp(key,peerPublic_,32)||!peers_.active(peer_,peerPublic_))return false;
  // Match stock AdminModule's PKI-encrypted Routing ACK/NAK response. Its
  // proof covers the original packet ID and exact Routing error payload.
  uint8_t routing[12]{};Wipe rw{routing,sizeof(routing)};size_t routingSize=0;
  const uint8_t result[]={24,uint8_t(routingError)};
  if(!session_.attachProof(request,result,sizeof(result),routing,sizeof(routing),routingSize)||routingSize!=sizeof(routing))return false;
  uint8_t data[23]={8,5,18,12};Wipe dw{data,sizeof(data)};
  memcpy(data+4,routing,sizeof(routing));data[16]=53;put(data+17,request);data[21]=72;data[22]=0;
  const uint32_t id=ep.generatedId(random);if(!id)return false;
  bench::Frame encrypted{};Wipe fw{&encrypted,sizeof(encrypted)};
  // PkiSession::send inside frame() reserves the durable fresh identity. Do
  // not reserve the same ID separately here.
  if(!frame(id,data,sizeof(data),responseHop,reliable,encrypted))return false;
  const int queued=ep.submitPrivateControl(encrypted,request,now);
  (void)session_.retire(id);
  return queued==0;
}
#endif
int PkiRuntime::submitImpl(Endpoint &ep,uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
  unsigned hop,bool reliable,const uint8_t *explicitKey,size_t keySize,bool explicitPki,uint32_t now,bool adminGet) {
  if(!ready(binding_.node))return -EPERM;
  bool payloadAllowed=application(data,n);
#if defined(CONFIG_MT_RF_ADMIN_GET)
  if(adminGet){admin::remote_protocol::Query query{};payloadAllowed=explicitPki&&
    admin::remote_protocol::canonicalGetRequest(data,n,query);}
  const bool adminKeyValid=!adminGet||(explicitKey&&keySize==32&&!memcmp(explicitKey,peerPublic_,32));
#else
  (void)explicitPki;
  if(adminGet)return -ENOTSUP;
  const bool adminKeyValid=true;
#endif
  if((owner>=LocalClientCount&&owner!=Endpoint::LocalUiOwner)||to!=peer_||!id||hop>HopPolicy::Maximum||!payloadAllowed||
    !adminKeyValid||(keySize&&(keySize!=32||!explicitKey||memcmp(explicitKey,peerPublic_,32))))return -EINVAL;
  if(owner==Endpoint::LocalUiOwner&&!ep.localUiSubmissionAllowed(id))return -EBUSY;
  bench::RouteMetadata metadata;Wipe mw{&metadata,sizeof(metadata)};
  if(!bench::MtCodec::decodeData(data,n,metadata))return -EINVAL;
  const bool telemetryQuery=metadata.portnum==meshtastic_PortNum_TELEMETRY_APP&&metadata.wantResponse;
  const bool positionQuery=metadata.portnum==meshtastic_PortNum_POSITION_APP&&metadata.wantResponse;
  const bool neighborQuery=metadata.portnum==meshtastic_PortNum_NEIGHBORINFO_APP&&metadata.wantResponse;
  const bool query=telemetryQuery||positionQuery||neighborQuery;
  const uint8_t variant=telemetryQuery?RemoteTelemetryQuery::requestVariant(metadata.payload,metadata.payloadLength):0;
  const bool localStats=variant==6;
  sweep(ep);
  uint8_t digest[32]{};Wipe wipe{digest,32};
  if(!backend_.sha256(data,n,digest)){freeze();return -EPERM;}
  Pending *slot=nullptr;
  for(auto &p:pending_) {
    if(p.id==id) {
      if(p.hop!=hop||p.reliable!=reliable||p.adminGet!=adminGet||crypto_verify32(p.digest,digest))return -EINVAL;
      PkiFrame cached;Wipe w{&cached,sizeof(cached)};
      if(session_.retry(id,cached)!=SessionResult::Ok)return -EPERM;
      bench::Frame f;put(f.bytes,to);put(f.bytes+4,binding_.node);put(f.bytes+8,id);
      f.bytes[12]=uint8_t(hop|(hop<<5)|(reliable?8:0));f.bytes[13]=f.bytes[14]=0;f.bytes[15]=uint8_t(binding_.node);
      memcpy(f.bytes+16,cached.bytes,cached.size);f.len=cached.size+16;
      return ep.submit(owner,f.bytes,f.len,reliable,now,false,true,query,nullptr,true,false,positionQuery,telemetryQuery,false,localStats,variant,neighborQuery);
    }
    if(!p.id&&!slot)slot=&p;
  }
  if(!slot||!ep.free())return -ENOBUFS;
  bench::Frame f;
  if(!frame(id,data,n,hop,reliable,f))return -EPERM;
  const int ret=ep.submit(owner,f.bytes,f.len,reliable,now,false,true,query,nullptr,true,false,positionQuery,telemetryQuery,false,localStats,variant,neighborQuery);
  if(ret){session_.retire(id);return ret;}
  slot->id=id;slot->hop=hop;slot->reliable=reliable;slot->adminGet=adminGet;memcpy(slot->digest,digest,32);return 0;
}
#endif
bool PkiRuntime::reply(Endpoint &ep,bench::MtCodec &codec,uint32_t request,unsigned hop,uint32_t now,uint32_t random) {
  return reply(ep,codec,session_,peer_,request,hop,now,random);
}
bool PkiRuntime::reply(Endpoint &ep,bench::MtCodec &codec,PkiSession &replySession,uint32_t peer,
    uint32_t request,unsigned hop,uint32_t now,uint32_t random) {
  if(!ep.privateControlAvailable()||!codec.originAllowed())return false;
  uint8_t routing[12]{};size_t rn=0;const uint8_t ok[2]={24,0};
  if(!replySession.attachProof(request,ok,2,routing,sizeof(routing),rn))return false;
  uint8_t data[23]={8,5,18,12};memcpy(data+4,routing,12);data[16]=53;put(data+17,request);data[21]=72;data[22]=0;
  bench::Frame f;const auto id=ep.generatedId(random);
  size_t len=0;
  uint32_t extra=0;Wipe ew{&extra,sizeof(extra)};
  if(!id||!nonces_.reserve(key_,binding_.publicKey,binding_.node,id,extra))return false;
  if(rn!=12||!codec.encodeZeroHopData(binding_.node,peer,id,data,sizeof(data),f.bytes,255,len))return false;
  f.len=len;
  f.bytes[12]=uint8_t(hop|(hop<<5));return ep.submitPrivateControl(f,request,now)==0;
}
void PkiRuntime::metricsReply(Endpoint &ep,uint32_t request,unsigned hop,uint32_t now,uint32_t random,bool wantAck,bool localStats,bool environment,bool power,uint8_t extra) {
  auto &diag=replyDiagnostics_;PrivateReplyDiagnostics::bump(diag.attempts);
  if(!ep.free()){PrivateReplyDiagnostics::bump(diag.no_slot);return;}
  uint8_t data[128]{};Wipe dw{data,sizeof(data)};size_t size=0;
  if(!ep.privateTelemetryReply(peer_,request,now,data,sizeof(data),size,localStats,environment,power,extra)){
    PrivateReplyDiagnostics::bump(diag.producer);return;
  }
  const auto id=ep.generatedId(random);if(!id){PrivateReplyDiagnostics::bump(diag.id);return;}
  bench::Frame encrypted;
  if(!frame(id,data,size,hop,wantAck,encrypted)){PrivateReplyDiagnostics::bump(diag.frame);return;}
  if(ep.submitPrivateControl(encrypted,request,now))PrivateReplyDiagnostics::bump(diag.queue);
  else PrivateReplyDiagnostics::bump(diag.queued);
  // The control queue owns the exact ciphertext and does not re-encrypt. Its
  // retries reuse those bytes without a session slot; the nonce stays burned.
  (void)session_.retire(id);
}
void PkiRuntime::positionReply(Endpoint &ep,uint32_t request,unsigned hop,uint32_t now,uint32_t random,bool wantAck) {
  if(!ep.free())return;
#if defined(CONFIG_MT_TAK_TRACKER)
  // PKI plaintext reserves twelve bytes for its authentication envelope.
  uint8_t data[227]{};
#else
  uint8_t data[40]{};
#endif
  Wipe dw{data,sizeof(data)};size_t size=0;
  if(!ep.privatePositionReply(peer_,request,now,data,sizeof(data),size))return;
  const auto id=ep.generatedId(random);if(!id)return;
  bench::Frame encrypted;
  if(!frame(id,data,size,hop,wantAck,encrypted))return;
  if(!ep.submitPrivateControl(encrypted,request,now,true))ep.positionReplyAccepted(now);
  (void)session_.retire(id);
}
void PkiRuntime::neighborReply(Endpoint &ep,uint32_t request,unsigned hop,uint32_t now,uint32_t random,bool wantAck) {
  uint8_t data[227]{};Wipe dw{data,sizeof(data)};size_t size=0;
  if(!ep.neighborReplyData(request,now,data,sizeof(data),size))return;
  const auto id=ep.generatedId(random);if(!id)return;
  bench::Frame encrypted;
  if(!frame(id,data,size,hop,wantAck,encrypted))return;
  // All public/private peers share this window. Only installed ciphertext
  // consumes it; failed providers, encoding, durable IDs or queues do not.
  if(!ep.submitPrivateControl(encrypted,request,now))ep.neighborReplyAccepted(now);
  (void)session_.retire(id);
}
#if defined(CONFIG_MT_PAXCOUNTER)
[[gnu::noinline]] void PkiRuntime::paxReply(Endpoint &ep,uint32_t request,unsigned hop,uint32_t now,uint32_t random,bool wantAck) {
  if(!ep.privateControlAvailable())return;
  uint8_t data[40];Wipe dw{data,sizeof(data)};size_t size=0;
  if(!ep.paxReplyData(peer_,request,now,data,sizeof(data),size))return;
  const auto id=ep.generatedId(random);
  bench::Frame encrypted;
  if(!id || !frame(id,data,size,hop,wantAck,encrypted))return;
  if(!ep.submitPrivateControl(encrypted,request,now))ep.paxReplyAccepted(peer_,request,now);
  (void)session_.retire(id);
}
#endif
void PkiRuntime::receive(Endpoint &ep,bench::MtCodec &codec,const uint8_t *p,size_t n,uint32_t now,uint32_t random,bool armed) {
  receiveClassification_=ReceiveClassification::Hold;
  authenticatedRxFrom_=0;
  clearEvent();
  if(!p||n<29||n>255) {
    receiveClassification_=ReceiveClassification::NotPki;return;
  }
  if(!binding_.node)return;
  if(bench::le32(p)!=binding_.node||p[13]) {
    receiveClassification_=ReceiveClassification::NotPki;return;
  }
  if(!bench::le32(p+8)||(p[12]&7)>((p[12]>>5)&7)) {
    receiveClassification_=ReceiveClassification::NotPki;return;
  }
  auto *verification=verification_;
  if(verification&&verification->receiveReady)return;
  struct SpeculativeVerification {
    PkiRuntime &runtime;uint32_t peer=0;uint8_t key[32]{};bool retained=false;
    ~SpeculativeVerification(){
      if(peer&&!retained)runtime.keyVerificationReleasePeer(peer,key);
      crypto_wipe(key,sizeof(key));
    }
  } speculative{*this};
  if(p&&n>=29&&n<=255&&bench::le32(p)==binding_.node&&bench::le32(p+4)!=peer_&&
     p[13]==0&&(p[12]&7)<=((p[12]>>5)&7)&&verification&&!verification->peer) {
    uint8_t pinnedKey[32]{};
    if(peers_.lookup(bench::le32(p+4),pinnedKey)&&peers_.active(bench::le32(p+4),pinnedKey)&&
       !peers_.revoked(bench::le32(p+4))&&
       keyVerificationStageCandidate(bench::le32(p+4),pinnedKey)==0) {
      // Ordinary private traffic and failed authentication must not occupy
      // the one shared manual-verification slot after this packet attempt.
      speculative.peer=bench::le32(p+4);memcpy(speculative.key,pinnedKey,32);
    }
    crypto_wipe(pinnedKey,sizeof(pinnedKey));
  }
  if(verification&&verification->session&&verification->session->ready()&&verification->peer&&p&&n>=29&&n<=255&&
     (verificationOwner_||bench::le32(p+4)!=peer_)&&
     bench::le32(p)==binding_.node&&bench::le32(p+4)==verification->peer&&bench::le32(p+8)&&p[13]==0&&
     (p[12]&7)<=((p[12]>>5)&7)) {
    PkiFrame candidateFrame{};Wipe fw{&candidateFrame,sizeof(candidateFrame)};
    candidateFrame.from=verification->peer;candidateFrame.to=binding_.node;
    candidateFrame.id=bench::le32(p+8);candidateFrame.size=n-16;
    memcpy(candidateFrame.bytes,p+16,candidateFrame.size);
    ReplayCheck candidateReplay=ReplayCheck::Fresh;
    if(verification->peerPinned) {
      candidateReplay=peers_.seen(verification->peer,verification->publicKey,candidateFrame.id);
      if(candidateReplay!=ReplayCheck::Fresh&&candidateReplay!=ReplayCheck::Seen)return;
      if(candidateReplay==ReplayCheck::Seen)
        receiveClassification_=ReceiveClassification::AuthenticatedConsumed;
    }
    uint8_t candidatePlain[227]{};Wipe pw{candidatePlain,sizeof(candidatePlain)};size_t candidateSize=0;
    const auto candidateAuth=verification->session->authenticate(candidateFrame,candidatePlain,sizeof(candidatePlain),candidateSize);
    if(candidateAuth==SessionResult::AuthenticationFailed) {
      receiveClassification_=ReceiveClassification::Unauthenticated;return;
    }
    if(candidateAuth!=SessionResult::Ok)return;
    receiveClassification_=ReceiveClassification::AuthenticatedConsumed;
    bench::RouteMetadata candidateMetadata{};Wipe mw{&candidateMetadata,sizeof(candidateMetadata)};
    if(!bench::MtCodec::decodeData(candidatePlain,candidateSize,candidateMetadata)||
       candidateMetadata.portnum!=meshtastic_PortNum_KEY_VERIFICATION_APP||
       !PrivateApplication::keyVerification(candidatePlain,candidateSize)||
       candidateMetadata.payloadLength>sizeof(verification->receive.payload))return;
    // The temporary session records replay only after the exact dedicated
    // application gate succeeds. Re-authenticate into bounded scratch so the
    // generic PkiSession receive window cannot be consumed by other payloads.
    uint8_t replayPlain[227]{};Wipe replayWipe{replayPlain,sizeof(replayPlain)};size_t replaySize=0;
    const auto receiveResult=verification->session->receive(candidateFrame,replayPlain,sizeof(replayPlain),replaySize);
    const unsigned candidateStart=(p[12]>>5)&7,candidateHop=p[12]&7;
    const unsigned candidateReplyHop=HopPolicy::response(candidateStart,candidateHop,hopLimit_);
    if(candidateReplay==ReplayCheck::Seen||receiveResult==SessionResult::Replay) {
      // A duplicate is eligible only for an exact cached ciphertext whose
      // original canonical KVA request asked for a Routing ACK. It is never
      // re-delivered to the KVA owner or a normal private application path.
      uint8_t storedHop=0;
      if(armed&&(p[12]&8u)&&ep.privateControlAvailable()&&
         verification->duplicateAck.claim(candidateFrame.id,candidateFrame.bytes,candidateFrame.size,storedHop))
        (void)reply(ep,codec,*verification->session,verification->peer,candidateFrame.id,storedHop,now,random);
      return;
    }
    if(receiveResult!=SessionResult::Ok||replaySize!=candidateSize||
       memcmp(replayPlain,candidatePlain,candidateSize))return;
    if(verification->peerPinned&&
       (!peers_.commitAuthenticated(verification->peer,verification->publicKey,candidateFrame.id)||
        !backend_.healthy()||!identity_.active(key_)||!peers_.boundTo(binding_.publicKey,binding_.node))) {
      freeze();return;
    }
    verification->receive={};verification->receive.from=verification->peer;verification->receive.id=candidateFrame.id;
    verification->receive.pki=true;memcpy(verification->receive.publicKey,verification->publicKey,32);
    verification->receive.size=candidateMetadata.payloadLength;
    memcpy(verification->receive.payload,candidateMetadata.payload,verification->receive.size);
    verification->receiveReady=true;
    speculative.retained=true; // The KVA owner now owns this admitted request.
    if(armed&&(p[12]&8u)) {
      (void)verification->duplicateAck.remember(candidateFrame.id,candidateFrame.bytes,
        candidateFrame.size,uint8_t(candidateReplyHop));
      (void)reply(ep,codec,*verification->session,verification->peer,candidateFrame.id,
        candidateReplyHop,now,random);
    }
    return;
  }
  const bool ownerProviderReady=opened_&&!frozen_&&backend_.healthy()&&identity_.active(key_)&&
    peers_.boundTo(binding_.publicKey,binding_.node);
  const bool providerReady=ownerProviderReady&&
    (ready(binding_.node)||verificationOwner_);
  if(!ready(binding_.node)||bench::le32(p+4)!=peer_) {
    const uint32_t from=bench::le32(p+4);uint8_t known[32]{};
    const bool knownKey=peers_.lookup(from,known);crypto_wipe(known,sizeof(known));
    if(providerReady&&from>1&&from<0xfffffffeu&&from!=binding_.node&&from!=peer_&&
       !knownKey&&!peers_.revoked(from))
      receiveClassification_=ReceiveClassification::Unauthenticated;
    return;
  }
  PkiFrame f;f.from=peer_;f.to=binding_.node;f.id=bench::le32(p+8);f.size=n-16;memcpy(f.bytes,p+16,f.size);
  Wipe fw{&f,sizeof(f)};uint8_t plain[227]{};Wipe pw{plain,sizeof(plain)};size_t size=0;
  const auto auth=session_.authenticate(f,plain,sizeof(plain),size);
  if(auth==SessionResult::AuthenticationFailed) {
    receiveClassification_=ReceiveClassification::Unauthenticated;return;
  }
  if(auth==SessionResult::Replay) {
    receiveClassification_=ReceiveClassification::AuthenticatedConsumed;return;
  }
  if(auth!=SessionResult::Ok)return;
  receiveClassification_=ReceiveClassification::AuthenticatedConsumed;
  const bool keyVerificationApplication=PrivateApplication::keyVerification(plain,size);
#if defined(CONFIG_MT_PAXCOUNTER)
  const bool paxQuery=modules::pax::isRequest(plain,size);
#else
  constexpr bool paxQuery=false;
#endif
#if defined(CONFIG_MT_REPLY_EXAMPLE)
  const bool exampleRequest=stock::optional::exactWire(plain,size,true);
#else
  constexpr bool exampleRequest=false;
#endif
#if defined(CONFIG_MT_RF_ADMIN_GET)
  bool adminCandidate=false,adminMutationCandidate=false,adminResponseCandidate=false;uint32_t adminResponseOwners=0;
  admin::remote_protocol::Query adminQuery{};
  admin::mutation_protocol::Mutation adminMutation{};
  if(!application(plain,size)&&!keyVerificationApplication&&!paxQuery&&!exampleRequest) {
    bench::RouteMetadata candidate;Wipe cw{&candidate,sizeof(candidate)};
    if(!bench::MtCodec::decodeData(plain,size,candidate)||candidate.portnum!=meshtastic_PortNum_ADMIN_APP)return;
    // Preserve the narrow GET responder. A response is eligible only when the
    // Endpoint finds an exact live query owned by at least one local client.
    if(admin::remote_protocol::canonicalGetRequest(plain,size,adminQuery))adminCandidate=true;
    else if(admin::mutation_protocol::canonicalSetRequest(plain,size,adminMutation))adminMutationCandidate=true;
    else {
      adminResponseOwners=ep.privateAdminResponseOwners(binding_.node,peer_,peerPublic_,f.id,plain,size,now,false);
      if(!adminResponseOwners)return;
      adminResponseCandidate=true;
    }
  }
#else
  if(!application(plain,size)&&!keyVerificationApplication&&!paxQuery&&!exampleRequest)return;
#endif
  if(rebroadcastMode_==5) {
    bench::RouteMetadata gate;Wipe gw{&gate,sizeof(gate)};
#if defined(CONFIG_MT_RF_ADMIN_GET)
    if(!bench::MtCodec::decodeData(plain,size,gate)||
       (!adminCandidate&&!adminResponseCandidate&&!keyVerificationApplication&&!mt::corePort(gate.portnum)))return;
#else
    if(!bench::MtCodec::decodeData(plain,size,gate)||
       (!keyVerificationApplication&&!mt::corePort(gate.portnum)))return;
#endif
  }
  // Even known (Seen) IDs authenticate and pass the application gate first. The replay authority
  // then decides Fresh vs Seen; every other verdict fails closed.
  const auto check=peers_.seen(peer_,peerPublic_,f.id);
  if(check!=ReplayCheck::Fresh&&check!=ReplayCheck::Seen)return;
  if(check==ReplayCheck::Seen){
    // Seen: never commit, never re-deliver, never re-synthesize telemetry. Only a
    // one-shot exact-content claim re-ACKs via a fresh TX ID/nonce and proof path.
    bench::RouteMetadata metadata;Wipe mw{&metadata,sizeof(metadata)};
    if(!bench::MtCodec::decodeData(plain,size,metadata))return;
#if defined(CONFIG_MT_RF_ADMIN_GET)
    if(metadata.portnum==meshtastic_PortNum_ADMIN_APP)return;
#endif
    if(armed&&(p[12]&8)&&!paxQuery&&!exampleRequest&&!((metadata.portnum==meshtastic_PortNum_TELEMETRY_APP||
       metadata.portnum==meshtastic_PortNum_POSITION_APP||metadata.portnum==meshtastic_PortNum_NEIGHBORINFO_APP)&&metadata.wantResponse)){
      uint8_t storedHop=0;
      if(duplicateAck_.claim(f.id,f.bytes,f.size,storedHop))reply(ep,codec,f.id,storedHop,now,random);
    }
    return;
  }
  if(!peers_.commitAuthenticated(peer_,peerPublic_,f.id)||!ready(binding_.node)){freeze();return;}
  bench::RouteMetadata metadata;Wipe mw{&metadata,sizeof(metadata)};
  if(!bench::MtCodec::decodeData(plain,size,metadata))return;
  // Fresh authenticated PKI application admission is observable once, before
  // the reply-slot/provider paths below can return early. No plaintext/key
  // leaves this runtime; Service consumes only the sender ID.
  authenticatedRxFrom_=peer_;
  if(metadata.portnum==meshtastic_PortNum_KEY_VERIFICATION_APP) {
    if(!PrivateApplication::keyVerification(plain,size)||
       !verification||metadata.payloadLength>sizeof(verification->receive.payload))return;
    verification->receive={};verification->receive.from=peer_;verification->receive.id=f.id;
    verification->receive.pki=true;memcpy(verification->receive.publicKey,peerPublic_,32);
    verification->receive.size=metadata.payloadLength;
    memcpy(verification->receive.payload,metadata.payload,verification->receive.size);
    verification->receiveReady=true;
    if(armed&&(p[12]&8u)) {
      const unsigned start=(p[12]>>5)&7,hop=p[12]&7;
      const unsigned replyHop=HopPolicy::response(start,hop,hopLimit_);
      (void)duplicateAck_.remember(f.id,f.bytes,f.size,uint8_t(replyHop));
      (void)reply(ep,codec,f.id,replyHop,now,random);
    }
    return;
  }
#if defined(CONFIG_MT_RF_ADMIN_GET)
  if(metadata.portnum==meshtastic_PortNum_ADMIN_APP) {
    if(adminResponseCandidate){
      const uint32_t owners=ep.privateAdminResponseOwners(binding_.node,peer_,peerPublic_,f.id,plain,size,now,true);
      if(!owners)return;
      event_.from=peer_;event_.to=binding_.node;event_.id=f.id;event_.size=size;
      event_.adminOwners=uint8_t(owners&LocalClientMask);
      if(!event_.adminOwners)return;
      ep.privateAdminResponse(peer_,metadata.request_id);
      memcpy(event_.publicKey,peerPublic_,32);memcpy(event_.data,plain,size);
      memcpy(eventFrame_.bytes,p,n);eventFrame_.len=n;
      return;
    }
    // A remote SET is never applied from RX. Fresh replay is already committed
    // to legacy durable history or the explicitly selected bounded RAM window;
    // stage its exact canonical bytes for the Service owner loop after RX
    // restoration. It never becomes a generic client event or an eager ACK.
    if(adminMutationCandidate) {
      if(armed) {
        const unsigned start=(p[12]>>5)&7,hop=p[12]&7;
        const uint8_t responseHop=uint8_t(HopPolicy::response(start,hop,hopLimit_));
        (void)ep.privateAdminMutationRequest(binding_.node,peer_,peerPublic_,f.id,
            responseHop,(p[12]&8)!=0,plain,size,now);
      }
      return;
    }
    // Admin never enters the generic client event or ACK path. Only a
    // canonical read-only GET on authenticated Fresh ingress can invoke the
    // immutable owner provider, once, after the selected replay policy commits.
    if(!adminCandidate||!armed)return;
    uint8_t response[admin::remote_protocol::MaxDataBytes]{};
    Wipe rw{response,sizeof(response)};size_t responseSize=0;
    if(!ep.privateAdminReplyData(binding_.node,peer_,peerPublic_,f.id,plain,size,
        response,sizeof(response),responseSize)||!responseSize||responseSize>sizeof(response)||
       !admin::remote_protocol::canonicalGetResponse(response,responseSize,adminQuery,f.id))return;
    const auto id=ep.generatedId(random);if(!id)return;
    const unsigned start=(p[12]>>5)&7;
    const unsigned responseHop=HopPolicy::response(start,p[12]&7,hopLimit_);
    bench::Frame encrypted;
    if(!frame(id,response,responseSize,responseHop,(p[12]&8)!=0,encrypted))return;
    (void)ep.submitPrivateControl(encrypted,f.id,now);
    (void)session_.retire(id);
    return;
  }
#endif
  if(metadata.portnum==meshtastic_PortNum_TELEMETRY_APP&&metadata.request_id&&
     !ep.privateTelemetry(binding_.node,peer_,binding_.node,f.id,metadata,p,n,now))return;
  if(Endpoint::positionResponsePort(metadata.portnum)&&metadata.request_id&&
     !ep.privatePosition(binding_.node,peer_,binding_.node,f.id,metadata,p,n,now))return;
  if(metadata.portnum==meshtastic_PortNum_NEIGHBORINFO_APP&&metadata.request_id&&
     !ep.privateNeighbor(binding_.node,peer_,binding_.node,f.id,metadata,p,n,now))return;
  event_.from=peer_;event_.to=binding_.node;event_.id=f.id;event_.size=size;
  memcpy(event_.publicKey,peerPublic_,32);memcpy(event_.data,plain,size);
  memcpy(eventFrame_.bytes,p,n);eventFrame_.len=n;
  const unsigned start=(p[12]>>5)&7,replyHop=HopPolicy::response(start,p[12]&7,hopLimit_);
#if defined(CONFIG_MT_PAXCOUNTER)
  if(paxQuery) {
    if(armed && !(p[12]&0x10))paxReply(ep,f.id,replyHop,now,random,(p[12]&8)!=0);
    return;
  }
#endif
  if(exampleRequest)return;
  if(armed&&metadata.portnum==meshtastic_PortNum_TELEMETRY_APP&&metadata.wantResponse) {
    // Select the measured LocalStats producer explicitly; unsupported counters
    // remain omitted and DeviceMetrics retains its existing response shape.
    const bool localStats=LocalStatsQuery::request(metadata.payload,metadata.payloadLength);
    const bool environment=EnvironmentQuery::request(metadata.payload,metadata.payloadLength);
    const bool power=PowerQuery::request(metadata.payload,metadata.payloadLength);
    const auto extra=bench::MtCodec::supplementalTelemetryVariant(metadata.payload,metadata.payloadLength);
    if(localStats||environment||power||extra||TelemetryQuery::request(metadata.payload,metadata.payloadLength))
      metricsReply(ep,f.id,replyHop,now,random,(p[12]&8)!=0,localStats,environment,power,extra);
  }
  else if(armed&&metadata.portnum==meshtastic_PortNum_POSITION_APP&&metadata.wantResponse)
    positionReply(ep,f.id,replyHop,now,random,(p[12]&8)!=0);
  else if(armed&&metadata.portnum==meshtastic_PortNum_NEIGHBORINFO_APP&&metadata.wantResponse)
    neighborReply(ep,f.id,replyHop,now,random,(p[12]&8)!=0);
  else if(armed&&(p[12]&8))reply(ep,codec,f.id,replyHop,now,random);
  if(armed&&(p[12]&8)&&!((metadata.portnum==meshtastic_PortNum_TELEMETRY_APP||
     metadata.portnum==meshtastic_PortNum_POSITION_APP||metadata.portnum==meshtastic_PortNum_NEIGHBORINFO_APP)&&metadata.wantResponse))
    duplicateAck_.remember(f.id,f.bytes,f.size,replyHop);
}
ReceiveClassification PkiRuntime::receiveClassified(Endpoint &ep,bench::MtCodec &codec,
    const uint8_t *p,size_t n,uint32_t now,uint32_t random,bool armed) {
  receive(ep,codec,p,n,now,random,armed);
  return receiveClassification_;
}
bool PkiRuntime::keyVerificationProof(Endpoint &ep,const uint8_t *p,size_t n,
    const bench::RouteMetadata &m,uint32_t now) {
  if(!verificationOwner_||!verification_||!verification_->session||
     !verification_->session->ready()||!verification_->peer||!p||n<17||n>255||
     !backend_.healthy()||!identity_.active(key_)||
     !peers_.boundTo(binding_.publicKey,binding_.node)||
     m.portnum!=5||!m.routingPresent||!m.routingErrorPresent||!m.request_id||
     bench::le32(p)!=binding_.node||bench::le32(p+4)!=verification_->peer||
     !bench::le32(p+8)||((p[12]&7)>((p[12]>>5)&7)))return false;
  ep.tick(now,0);
  if(!ep.pendingPrivate(m.request_id,verification_->peer,true))return false;
  uint8_t routing[233]{};size_t size=0;Wipe w{routing,sizeof(routing)};
  if(verification_->session->verifyProof(verification_->peer,binding_.node,m.request_id,
     m.payload,m.payloadLength,routing,sizeof(routing),size)!=experimental::AckProofVerdict::ValidProof)return false;
  const uint8_t code=(m.routingError<=9||(m.routingError>=32&&m.routingError<=39))?
    uint8_t(m.routingError):2;
  ep.authenticated(m.request_id,verification_->peer,code,p,n,m,now,false);return true;
}
void PkiRuntime::proof(Endpoint &ep,const uint8_t *p,size_t n,const bench::RouteMetadata &m,uint32_t now) {
  if(verificationOwner_) {(void)keyVerificationProof(ep,p,n,m,now);return;}
  if(!ready(binding_.node)||!p||n<17||m.portnum!=5||!m.routingPresent||!m.routingErrorPresent||
    !m.request_id||bench::le32(p)!=binding_.node||bench::le32(p+4)!=peer_)return;
  ep.tick(now,0);if(!ep.pendingPrivate(m.request_id,peer_,true))return;
  uint8_t routing[233]{};size_t size=0;Wipe w{routing,sizeof(routing)};
  if(session_.verifyProof(peer_,binding_.node,m.request_id,m.payload,m.payloadLength,routing,sizeof(routing),size)!=
    experimental::AckProofVerdict::ValidProof)return;
  const uint8_t code=(m.routingError<=9||(m.routingError>=32&&m.routingError<=39))?uint8_t(m.routingError):2;
#if defined(CONFIG_MT_RF_ADMIN_GET)
  if(code)ep.privateAdminFailure(peer_,peerPublic_,m.request_id);
#endif
  ep.authenticated(m.request_id,peer_,code,p,n,m,now,true);
}
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE) || defined(CONFIG_MT_DROPZONE)
const PrivateEvent *PkiRuntime::moduleEvent(const uint8_t *frame,size_t size){
  if(!ready(binding_.node) || !event_.size || !frame || size!=eventFrame_.len ||
     memcmp(frame,eventFrame_.bytes,size))return nullptr;
#if defined(CONFIG_MT_RF_ADMIN_GET)
  if(event_.adminOwners)return nullptr;
#endif
  return &event_;
}
#endif
bool PkiRuntime::event(uint8_t owner,const uint8_t *p,size_t n,PrivateEvent &out) {
  if(owner>=LocalClientCount||!ready(binding_.node)||!event_.size||(eventOwners_&(1u<<owner))||
#if defined(CONFIG_MT_RF_ADMIN_GET)
    (event_.adminOwners&&!(event_.adminOwners&(1u<<owner)))||
#endif
    !p||n!=eventFrame_.len||memcmp(p,eventFrame_.bytes,n))return false;
  eventOwners_|=1u<<owner;out=event_;
#if defined(CONFIG_MT_RF_ADMIN_GET)
  const unsigned targetOwners=event_.adminOwners?event_.adminOwners:LocalClientMask;
#else
  const unsigned targetOwners=LocalClientMask;
#endif
  if((eventOwners_&targetOwners)==targetOwners)clearEvent();
  return true;
}
}
