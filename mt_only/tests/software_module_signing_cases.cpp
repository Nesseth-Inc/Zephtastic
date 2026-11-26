// SPDX-License-Identifier: GPL-3.0-only
// Actual signer/runtime/ledger/Service/channel cipher; synthetic public keys,
// memory journals and radios. Private CCM backend is intentionally unusable.
#include "Companion.h"
#include "known_channel_fixture.h"
#include "modules/BeaconController.h"
#include "TelemetryAnnouncement.h"
#include "pki/PkiRuntime.h"
#include "pki/PkiLedger.h"
#include "pki/verification/KeyVerificationService.h"
#include "monocypher.h"
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <vector>
#ifdef MT_TEST_STOCK_SIGNING_282
#include "stock-signing.inc"
#endif
namespace {
using namespace mt::pki;
using Bytes=std::vector<uint8_t>;
static_assert(bench::ModuleNodeStatusPort==meshtastic_PortNum_NODE_STATUS_APP);
static_assert(bench::ModuleRangeTestPort==meshtastic_PortNum_RANGE_TEST_APP);
static_assert(bench::ModuleTelemetryPort==meshtastic_PortNum_TELEMETRY_APP);
static_assert(bench::ModuleNeighborInfoPort==meshtastic_PortNum_NEIGHBORINFO_APP);
struct Key {
 uint8_t scalar[32]{};IdentityBinding binding{};
 explicit Key(unsigned seed){for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed+31*i);scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;crypto_x25519_public_key(binding.publicKey,scalar);binding.node=bench::MtSignature::nodeId(binding.publicKey);binding.generation=1;}
 ~Key(){crypto_wipe(scalar,32);}
};
struct Memory:mt::SettingsBackend {
 std::map<std::string,Bytes> values;unsigned writes=0;bool fail=false;std::function<void()> onWrite;
 int read(const char*k,uint8_t*out,size_t cap)override{auto i=values.find(k);if(i==values.end())return -ENOENT;assert(i->second.size()<=cap);memcpy(out,i->second.data(),i->second.size());return int(i->second.size());}
 int write(const char*k,const uint8_t*p,size_t n)override{++writes;if(onWrite)onWrite();if(fail)return -EIO;values[k]={p,p+n};return 0;}
};
struct Radio:mt::Radio {
 unsigned sends=0;int healthValue=0;bool occupied=false,done=false;std::vector<Bytes> frames;
 bool ready()override{return true;}int start()override{healthValue=1;return 0;}int restore()override{healthValue=1;return 0;}int stop()override{healthValue=0;return 0;}int health()override{return healthValue;}bool busy()override{return occupied;}
 uint32_t airtime(size_t)override{return 100;}int send(uint8_t*p,size_t n)override{++sends;frames.emplace_back(p,p+n);done=false;return 0;}bool completion(int&r)override{r=0;return done;}
};
struct Gate:LedgerStorageGate {mt::Service*service=nullptr;bool quiescent()const override{return service&&service->privateStoragePaused();}};
struct NonceEntropy:EntropySource {bool fail=false;unsigned calls=0;bool fill(uint8_t*p,size_t n)override{++calls;memset(p,0x71,n);return !fail;}};
struct SignEntropy:SigningEntropy {bool fail=false;unsigned calls=0;bool hedge(uint8_t out[32])override{++calls;memset(out,0x43,32);return !fail;}};
struct NoPrivateCrypto:mt::experimental::PkiBackend {
 unsigned calls=0;bool healthy()const override{return true;}
 bool sha256(const uint8_t*,size_t,uint8_t[32])override{++calls;return false;}
 bool hmac256(const uint8_t[32],const uint8_t*,size_t,uint8_t[32])override{++calls;return false;}
 bool ccm(bool,const uint8_t[32],const uint8_t[13],const uint8_t*,size_t,uint8_t*,uint8_t[8])override{++calls;return false;}
};
struct Host:mt::Companion::Host {
 std::vector<meshtastic_FromRadio> out;bool ready()const override{return true;}int save(const mt::Config&)override{return -ENOTSUP;}
 bool send(const uint8_t*p,size_t n)override{meshtastic_FromRadio f{};auto in=pb_istream_from_buffer(p+4,n-4);assert(pb_decode(&in,meshtastic_FromRadio_fields,&f));out.push_back(f);return true;}
};
void hello(mt::Companion&c){meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=1;uint8_t out[512];auto p=pb_ostream_from_buffer(out,sizeof(out));assert(pb_encode(&p,meshtastic_ToRadio_fields,&r));c.packet(out,p.bytes_written,1,mt::Companion::WorkspaceProof{true});}
struct RuntimeFixture {
 Key local{11},peer{97};Memory im,lm;MonocypherIdentityDeriver deriver;IdentityJournal identity;PkiLedger ledger;Gate gate;NonceEntropy extra;SignEntropy signing;NoPrivateCrypto backend;
 mt::experimental::PkiCrypto crypto{backend};LedgerNonceAuthority nonce{identity,ledger,lm,extra,gate};LedgerReplayAuthority replay{identity,ledger,lm,gate};
 PkiRuntime runtime;mt::test::KnownChannelFixtureClock clock;Radio radio;mt::Service service;Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion c,bc;
 explicit RuntimeFixture(bool sign=true,bool privateMode=true,unsigned seed=11,unsigned policy=1,bool aead=false,bool discovery=false):local(seed),runtime(crypto,backend,identity,nonce,replay,sign?&signing:nullptr),service(radio,true,local.binding.node,81,true,discovery,privateMode?&runtime:nullptr),c(service,usb,81,workspace,0),bc(service,ble,81,workspace,1){
  assert(!identity.load(im,deriver));IdentityCandidate candidate(local.binding,local.scalar);assert(!identity.store(im,deriver,candidate,true));
  assert(!ledger.load(lm)&&!ledger.initialize(lm,local.binding.publicKey,local.binding.node,true)&&!ledger.addTrustedPeer(lm,peer.binding.node,peer.binding.publicKey,true));
  assert(runtime.open(local.binding.node,peer.binding.node));gate.service=&service;clock.bind(service);
  mt::Config cfg;assert(cfg.setPacketSignaturePolicy(policy));cfg.bytes[4]=1;auto record=cfg.node();record.setPublic();cfg.node(record);assert(!aead||cfg.setPrimaryAead(true));assert(service.apply(cfg)&&service.start(false,0)&&service.armEndpoint(true));
  lm.onWrite=[this]{assert(service.privateStoragePaused()&&!radio.healthValue&&!service.active());};hello(c);hello(bc);usb.out.clear();ble.out.clear();
 }
};
Bytes encode(const meshtastic_Data&d){uint8_t out[239]{};auto wire=pb_ostream_from_buffer(out,sizeof(out));assert(pb_encode(&wire,meshtastic_Data_fields,&d));return {out,out+wire.bytes_written};}
meshtastic_Data announcement(unsigned port,size_t payload=5){
 meshtastic_Data d{};d.portnum=static_cast<meshtastic_PortNum>(port);d.has_bitfield=true;
 if(port==67){uint8_t bytes[239]{};size_t n=0;assert(mt::TelemetryAnnouncement::encode(1,{}, {},bytes,sizeof(bytes),n));auto p=pb_istream_from_buffer(bytes,n);assert(pb_decode(&p,meshtastic_Data_fields,&d));return d;}
 if(port==37){meshtastic_MeshBeacon b{};strcpy(b.message,"beacon");b.has_offer_preset=true;auto output=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));assert(pb_encode(&output,meshtastic_MeshBeacon_fields,&b));d.payload.size=pb_size_t(output.bytes_written);return d;}
 if(port==9){const uint8_t audio[]={0xc0,0xde,0xc2,0,1,2,3,4,5,6,7,8};memcpy(d.payload.bytes,audio,sizeof(audio));d.payload.size=sizeof(audio);return d;}
 if(port==34){meshtastic_Paxcount pax{};pax.wifi=1;auto output=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));assert(pb_encode(&output,meshtastic_Paxcount_fields,&pax));d.payload.size=pb_size_t(output.bytes_written);return d;}
 d.payload.size=pb_size_t(payload);for(size_t i=0;i<payload;++i)d.payload.bytes[i]=uint8_t('A'+i%26);return d;
}
const std::vector<unsigned>& modulePorts(){static const std::vector<unsigned> ports{9,34,36,66,67,71};return ports;}
bench::RouteMetadata decoded(RuntimeFixture&f,const Bytes&frame,bool verify=true){bench::RouteMetadata m;assert(f.service.codec.decode(frame.data(),frame.size(),m)&&m.channelDecoded&&!m.decodeRejected);if(verify){bench::MtSignature sig;assert(sig.trustPublicKey(f.local.binding.node,f.local.binding.publicKey,0));sig.apply(frame.data(),frame.size(),m,1000);assert(m.signatureVerified&&!m.signatureRejected&&!m.signatureQuarantined);}return m;}
void group(const char*n){std::cout<<"PASS "<<n<<'\n';}
void policy(){for(uint32_t p=0;p<512;++p){bool broadcast=p==9||p==34||p==36||p==37||p==66||p==67||p==71;bool publicSigned=p==9||p==34||p==36||p==37||p==66||p==71;
#if defined(CONFIG_MT_GPIO_MODULES)
 broadcast=broadcast||p==2||p==10;publicSigned=publicSigned||p==2||p==10;
#endif
 assert(bench::moduleBroadcastPort(p)==broadcast);assert(bench::publicSignedModulePort(p)==publicSigned);}
 assert(!bench::moduleBroadcastPort(UINT32_MAX));group("actual generated module ports Audio/Paxcounter/NodeStatus/Beacon/Range/Telemetry/Neighbor; public signed excludes telemetry and GPIO follows build flag");}
void outgoing(){for(unsigned port:modulePorts()){
 auto f=std::make_unique<RuntimeFixture>();const auto identity=f->im.values;const auto plain=encode(announcement(port));bool telemetry=false;assert(mt::ModuleAnnouncement::canonical(plain.data(),plain.size(),telemetry)&&telemetry==(port==67));uint32_t id=0;
 assert(!f->service.submitModuleAnnouncement(plain.data(),plain.size(),3,0,1000+port,id)&&id==1000+port&&f->ledger.sent(id));assert(f->signing.calls==1&&f->extra.calls==1&&f->lm.writes>0&&f->im.values==identity&&f->service.endpointFree()==4);
 uint32_t second=99;const auto writes=f->lm.writes;assert(f->service.submitModuleAnnouncement(plain.data(),plain.size(),3,1,2000+port,second)==-EAGAIN&&!second&&f->lm.writes==writes&&f->signing.calls==1);
 f->radio.occupied=true;f->service.poll(250,1);assert(!f->radio.sends&&f->signing.calls==1);f->radio.occupied=false;f->service.poll(500,2);assert(f->radio.sends==1&&f->radio.frames.size()==1&&f->signing.calls==1);
 const auto&frame=f->radio.frames[0];assert(bench::le32(frame.data())==UINT32_MAX&&bench::le32(frame.data()+4)==f->local.binding.node&&bench::le32(frame.data()+8)==id&&frame[12]==(3|(3<<5)));
 auto m=decoded(*f,frame);assert(m.portnum==port&&m.signatureLength==64&&!m.wantResponse&&!m.request_id&&m.hasBitfield&&!m.bitfield);assert(m.payloadLength==announcement(port).payload.size&&!memcmp(m.payload,announcement(port).payload.bytes,m.payloadLength));
 f->radio.done=true;f->service.poll(501,3);f->service.poll(2500,4);assert(f->radio.sends==1&&f->signing.calls==1&&f->backend.calls==0);mt::Endpoint::Result result;assert(!f->service.takeMessageResult(0,result)&&!f->service.takeMessageResult(1,result));
 uint8_t out[255];memset(out,0x5a,sizeof(out));size_t n=99;const auto signs=f->signing.calls;assert(!f->service.encodePublicMessage(UINT32_MAX,id,plain.data(),plain.size(),out,sizeof(out),n)&&!n&&out[0]==0x5a&&f->signing.calls==signs&&f->ledger.sent(id));
 }group("all six implemented Service module broadcasts sign under actual durable ledger/pause, hold immutable queued frame, consume no client slot/result and never private CCM");}
void noFallback(){for(unsigned mode=0;mode<4;++mode){auto f=std::make_unique<RuntimeFixture>(mode!=0);const auto plain=encode(announcement(66));if(mode==1)f->extra.fail=true;if(mode==2)f->signing.fail=true;if(mode==3)f->lm.fail=true;uint32_t id=9;
 assert(f->service.submitModuleAnnouncement(plain.data(),plain.size(),0,0,300,id)<0&&!id&&f->service.endpointControls()==0&&f->service.endpointFree()==4&&!f->radio.sends&&f->backend.calls==0);
 if(mode==1||mode==2){assert(f->ledger.sent(300));}
 if(mode==0||mode==1){assert(f->signing.calls==0);}
 f->service.poll(2000,1);assert(!f->radio.sends);
 }
 {
  auto f=std::make_unique<RuntimeFixture>();const auto plain=encode(announcement(66,200));uint32_t id=0;
  assert(!f->service.submitModuleAnnouncement(plain.data(),plain.size(),0,0,300,id)&&id==300&&
         f->ledger.sent(id)&&f->service.endpointControls()==1&&f->service.endpointFree()==4&&
         f->signing.calls==0&&f->backend.calls==0&&!f->radio.sends);
  f->service.poll(250,1);assert(f->radio.sends==1&&f->radio.frames.size()==1);
  const auto &frame=f->radio.frames[0];assert(bench::le32(frame.data())==UINT32_MAX&&
      bench::le32(frame.data()+4)==f->local.binding.node&&bench::le32(frame.data()+8)==id&&
      frame[12]==0&&frame[13]==f->service.codec.channelHash()&&
      frame[15]==uint8_t(f->local.binding.node));
  const auto metadata=decoded(*f,frame,false);assert(metadata.portnum==66&&!metadata.signatureLength&&
      metadata.hasBitfield&&!metadata.bitfield&&!metadata.wantResponse&&
      !metadata.request_id&&!metadata.reply_id&&!metadata.emoji&&metadata.payloadLength==200);
  // Compare the decoded payload with the typed canonical fixture value.
  const auto expected=announcement(66,200);
  assert(metadata.payloadLength==expected.payload.size&&
         !memcmp(metadata.payload,expected.payload.bytes,metadata.payloadLength));
  f->radio.done=true;f->service.poll(251,2);mt::Endpoint::Result result;
  assert(f->radio.sends==1&&!f->service.takeMessageResult(0,result)&&
         !f->service.takeMessageResult(1,result)&&f->signing.calls==0&&
         f->backend.calls==0&&f->ledger.sent(id));
 }
 for(unsigned port:modulePorts()){auto f=std::make_unique<RuntimeFixture>(true,false);const auto plain=encode(announcement(port,port==66?200:5));const auto writes=f->lm.writes;uint32_t id=0;assert(!f->service.submitModuleAnnouncement(plain.data(),plain.size(),0,0,400+port,id));f->service.poll(250,1);assert(f->radio.sends==1&&f->signing.calls==0&&f->extra.calls==0&&f->lm.writes==writes);const auto m=decoded(*f,f->radio.frames[0],false);assert(m.portnum==port&&!m.signatureLength);}
 group("configured missing signer/nonce entropy/sign entropy/storage refuse without unsigned fallback; canonical oversized module uses one bounded unsigned TooLarge fallback; no-private mode retains unsigned modules");}
void refusalAndCapacity(){auto f=std::make_unique<RuntimeFixture>();IdentityBinding b;KeyRef ref;assert(f->identity.binding(b,ref));
 for(unsigned port:{36u,66u,71u})for(unsigned mode=0;mode<7;++mode){auto d=announcement(port);uint32_t to=UINT32_MAX;switch(mode){case 0:to=f->peer.binding.node;break;case 1:d.want_response=true;break;case 2:d.request_id=1;break;case 3:d.has_bitfield=false;break;case 4:d.bitfield=2;break;case 5:d.dest=1;break;case 6:d.xeddsa_signature.size=64;break;}SignedData out{};memset(out.bytes,0x5a,sizeof(out.bytes));out.size=0x5a5a;out.from=out.to=out.id=0x5a5a5a5a;const Bytes before(reinterpret_cast<const uint8_t*>(&out),reinterpret_cast<const uint8_t*>(&out)+sizeof(out));const auto calls=f->signing.calls;assert(PkiSigner::sign(f->identity,ref,b,f->signing,700,to,d,out)==SigningResult::InvalidArgument&&!memcmp(&out,before.data(),before.size())&&f->signing.calls==calls);}
 for(unsigned port:{6u,3u,5u,8u,70u,256u}){auto d=announcement(port);SignedData out;assert(PkiSigner::sign(f->identity,ref,b,f->signing,701,UINT32_MAX,d,out)==SigningResult::InvalidArgument);}
 for(size_t length:{size_t(166),size_t(167)}){auto d=announcement(66,length);auto sized=d;sized.xeddsa_signature.size=64;size_t bytes=0;assert(pb_get_encoded_size(&bytes,meshtastic_Data_fields,&sized));assert((bytes<=239)==(length==166));SignedData out{};memset(out.bytes,0x5a,sizeof(out.bytes));out.size=0x5a5a;out.from=out.to=out.id=0x5a5a5a5a;const Bytes before(reinterpret_cast<const uint8_t*>(&out),reinterpret_cast<const uint8_t*>(&out)+sizeof(out));const auto calls=f->signing.calls;auto result=PkiSigner::sign(f->identity,ref,b,f->signing,702,UINT32_MAX,d,out);if(length==166)assert(result==SigningResult::Ok&&out.size==239&&f->signing.calls==calls+1);else assert(result==SigningResult::TooLarge&&!memcmp(&out,before.data(),before.size())&&f->signing.calls==calls);}
 for(unsigned mode=0;mode<6;++mode){auto d=announcement(66);if(mode==0)d.want_response=true;if(mode==1)d.request_id=1;if(mode==2)d.reply_id=1;if(mode==3)d.emoji=1;if(mode==4)d.bitfield=1;auto plain=encode(d);if(mode==5)plain.insert(plain.end(),{8,66});const auto writes=f->lm.writes;const auto entropy=f->extra.calls;bench::Frame out;out.len=99;memset(out.bytes,0x5a,sizeof(out.bytes));const Bytes old(out.bytes,out.bytes+sizeof(out.bytes));assert(!f->runtime.broadcast(f->service.codec,800+mode,plain.data(),plain.size(),out)&&out.len==99&&!memcmp(out.bytes,old.data(),old.size())&&f->lm.writes==writes&&f->extra.calls==entropy);}
 group("all new signer ports reject unicast/query/caller signature/legacy fields before entropy; exact239 capacity and canonical runtime refusals preserve outputs and nonce ordering");}
#ifdef MT_TEST_STOCK_SIGNING_282
void pinnedPolicies(){
 using namespace donor282;
 unsigned decisions=0;
 for(unsigned policy=0;policy<=2;++policy)for(bool aead:{false,true}){
  auto f=std::make_unique<RuntimeFixture>(true,true,11,policy,aead);
  const uint32_t peer=f->peer.binding.node;uint8_t header[16]{};
  for(unsigned i=0;i<4;i++)header[4+i]=uint8_t(peer>>(8*i));
  config.security.packet_signature_policy=policy;channels.aead=aead;
  for(bool known:{false,true}){
   st.known=known;
   if(known)assert(f->service.nodes.rememberVerifiedSigner(peer,f->local.binding.node));
   for(bool verified:{false,true})
    assert(db.refusesUnsignedIdentity(peer,0,verified)==f->service.refusesUnsignedIdentity(peer,0,verified));
   for(unsigned port:{1u,3u,4u,5u,70u,unsigned(meshtastic_PortNum_KEY_VERIFICATION_APP)})for(unsigned size:{0u,32u,64u}){
    for(bool key:{false,true})for(bool valid:{false,true}){
     st.key=key;st.valid=valid;st.learned=false;
     meshtastic_MeshPacket packet{};packet.from=peer;packet.to=10;
     packet.decoded.portnum=static_cast<meshtastic_PortNum>(port);
     packet.decoded.xeddsa_signature.size=size;packet.xeddsa_signed=true;
     const bool expected=checkXeddsaReceivePolicy(&packet);
     bench::RouteMetadata m{};m.portnum=port;m.signatureLength=uint8_t(size);
     if(size==64&&port!=70){m.signatureVerified=key&&valid;m.signatureRejected=key&&!valid;}
     assert(expected==f->service.packetSignatureAllowed(header,m,1));
     assert(packet.xeddsa_signed==m.signatureVerified);
     ++decisions;
    }
   }
  }
  for(bool ham:{false,true})for(bool broadcast:{false,true})for(unsigned port:{1u,3u,4u,5u,70u}){
   meshtastic_MeshPacket p{};p.to=broadcast?UINT32_MAX:peer;p.decoded.portnum=static_cast<meshtastic_PortNum>(port);
   owner.is_licensed=ham;st.fits=true;
   const bool zt=f->service.codec.signsPublic()&&port!=70&&(broadcast||port==3||port==4||(ham&&port!=5));
   assert(signs(&p)==zt);++decisions;
  }
 }
 assert(decisions==984);group("pinned Router.cpp RX/TX and NodeDB.cpp identity predicates, 984 cross-policy decisions");
}
#endif
void signingModes(){
 for(unsigned policy=0;policy<3;++policy)for(bool aead:{false,true}){
  auto f=std::make_unique<RuntimeFixture>(true,true,11,policy,aead,true);
  const auto self=f->local.binding.node,peer=f->peer.binding.node;
  const bool sign=policy!=0&&!aead;
  uint8_t header[16]{};for(unsigned i=0;i<4;++i)header[4+i]=uint8_t(peer>>(8*i));
  assert(f->service.nodes.rememberVerifiedSigner(peer,self));
  assert(f->service.refusesUnsignedIdentity(peer,0,false)==sign);
  assert(!f->service.refusesUnsignedIdentity(peer,0,true)&&!f->service.refusesUnsignedIdentity(self,0,false));
  for(unsigned port:{1u,3u,4u,5u,67u,70u,74u,static_cast<unsigned>(meshtastic_PortNum_KEY_VERIFICATION_APP)}){
   bench::RouteMetadata metadata{};metadata.portnum=port;
   const bool exempt=port==5||port==70||port==meshtastic_PortNum_KEY_VERIFICATION_APP;
   assert(f->service.packetSignatureAllowed(header,metadata,0)==(!sign||policy!=2||exempt));
   metadata.signatureLength=63;assert(f->service.packetSignatureAllowed(header,metadata,0)==(port==70));
   metadata.signatureLength=64;metadata.signatureQuarantined=true;
   assert(f->service.packetSignatureAllowed(header,metadata,0)==(!sign||policy!=2||port==70));
   metadata.signatureRejected=true;assert(!f->service.packetSignatureAllowed(header,metadata,0));
  }
  assert(!f->service.submitNodeInfo(0,300,peer,0,false,false,0));
  f->service.poll(500,1);assert(f->radio.frames.size()==1);
  auto n=decoded(*f,f->radio.frames[0],sign);
  f->radio.done=true;f->service.poll(501,1);
  assert(n.portnum==4&&n.publicKeyLength==32&&!memcmp(n.publicKey,f->local.binding.publicKey,32)&&n.signatureLength==(sign?64:0));
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;
  auto &q=request.packet;q.which_payload_variant=meshtastic_MeshPacket_decoded_tag;q.to=peer;q.id=301;
  q.decoded.portnum=meshtastic_PortNum_POSITION_APP;q.decoded.want_response=true;
  uint8_t bytes[128]{};auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&stream,meshtastic_ToRadio_fields,&request));
  f->c.packet(bytes,stream.bytes_written,502,mt::Companion::WorkspaceProof{true});
  f->c.packet(bytes,stream.bytes_written,503,mt::Companion::WorkspaceProof{true});
  f->service.poll(3000,1);assert(f->radio.frames.size()==2);
  const Bytes &position=f->radio.frames[1];auto pos=decoded(*f,position,sign);
  assert(pos.portnum==3&&pos.wantResponse&&pos.signatureLength==(sign?64:0));
  assert(f->service.codec.validatePositionRequest(position.data(),position.size(),sign));
  assert(f->signing.calls==(sign?2u:0u));
 }
 {
  auto f=std::make_unique<RuntimeFixture>();auto &service=f->service;const auto peer=f->peer.binding.node;
  mt::NodeCache::Entry trusted{};trusted.identity(peer,81,0);
  memcpy(trusted.bytes+mt::NodeCache::Entry::Name,"Verified name",14);
  assert(service.nodes.accept(trusted,f->local.binding.node,0,0)>=0);
  assert(service.nodes.rememberVerifiedSigner(peer,f->local.binding.node));
  auto event=[&](const char *name,unsigned role,unsigned time){
   meshtastic_User user{};snprintf(user.id,sizeof(user.id),"!%08x",peer);
   snprintf(user.long_name,sizeof(user.long_name),"%s",name);user.role=static_cast<meshtastic_Config_DeviceConfig_Role>(role);
   bench::RouteMetadata m{};m.portnum=4;auto o=pb_ostream_from_buffer(m.payload,sizeof(m.payload));
   assert(pb_encode(&o,meshtastic_User_fields,&user));m.payloadLength=uint8_t(o.bytes_written);
   service.noteRefusedIdentity(peer,m,time);
  };
  meshtastic_ClientNotification warning{};
  event("Verified name",0,1);assert(!service.takeRefusedIdentityNotice(warning));
  event("Refused name",1,10);assert(service.takeRefusedIdentityNotice(warning));
  assert(strstr(warning.message,"Verified name")&&!strstr(warning.message,"Refused name")&&strstr(warning.message,"Balanced"));
  event("Different",2,3600009);assert(!service.takeRefusedIdentityNotice(warning));
  event("Different",2,3600010);assert(service.takeRefusedIdentityNotice(warning));
  assert(service.config.enableLicensedOperation());
  mt::pki::verification::KeyVerificationService kva(service);
  meshtastic_KeyVerificationAdmin cmd{};cmd.message_type=meshtastic_KeyVerificationAdmin_MessageType_INITIATE_VERIFICATION;
  cmd.remote_nodenum=peer;assert(kva.admin(0,cmd,20)==-EPERM&&kva.takeNotification(warning));
  assert(warning.level==meshtastic_LogRecord_Level_WARNING&&!strcmp(warning.message,"Key verification is unavailable in licensed (ham) mode"));
  mt::pki::KeyVerificationReceive incoming{};incoming.from=peer;incoming.id=42;
  assert(kva.receive(incoming,21)==-EPERM&&!kva.takeNotification(warning));
  assert(kva.state()==mt::pki::verification::KeyVerification::State::Idle&&!f->radio.sends);
 }
 group("2.8.2 three signing policies and AES-CCM, real canonical NodeInfo and directed Position, malformed signatures, signer refusal and strict exceptions");
}
void outgoingBeacon(){
 auto f=std::make_unique<RuntimeFixture>();const auto data=encode(announcement(37));bool telemetry=false;
 assert(!mt::ModuleAnnouncement::canonical(data.data(),data.size(),telemetry));
 assert(mt::ModuleAnnouncement::canonical(data.data(),data.size(),telemetry,true)&&!telemetry);
 const auto writes=f->lm.writes;uint32_t id=1;
 assert(f->service.submitModuleAnnouncement(data.data(),data.size(),1,0,901,id,true)==-EINVAL&&!id&&f->lm.writes==writes);
 assert(!f->service.submitModuleAnnouncement(data.data(),data.size(),0,0,902,id,true)&&id==902&&f->ledger.sent(id));
 assert(f->signing.calls==1&&f->extra.calls==1&&f->radio.frames.empty());f->service.poll(500,2);assert(f->radio.frames.size()==1);
 const auto &frame=f->radio.frames[0];auto m=decoded(*f,frame);assert(m.portnum==37&&m.signatureLength==64&&!frame[12]&&!m.wantResponse&&!m.request_id&&mt::beacon::canonicalPayload(m.payload,m.payloadLength));
 assert(mt::PublicApplication::received(f->service.codec,frame.data(),frame.size(),m));
 group("same-channel zero-hop MeshBeacon reserves durable ID and signs through actual Service/runtime/ledger without ACK");
}
void targetedBeacons(){
 for(unsigned policy=0;policy<3;++policy)for(unsigned mode=0;mode<3;++mode){
  const bool targetAead=mode==1,licensed=mode==2;
  const unsigned targetIndex=licensed?0:1;
  auto f=std::make_unique<RuntimeFixture>(true,true,11,policy);
  auto cfg=f->service.config;
  if(licensed){
   assert(cfg.setPrimaryName("Licensed")&&cfg.enableLicensedOperation());
  }else{
   // The home and target ciphers deliberately differ in both directions.
   assert(cfg.setPrimaryAead(!targetAead));
   mt::ChannelRecord secondary;
   const uint8_t key[16]={9,8,7};
   assert(secondary.set(2,"Secondary",key,sizeof(key),0,false,false,false,targetAead));
   assert(cfg.setSecondaryChannel(targetIndex,secondary));
  }
  cfg.setTxEnabled(true);
  assert(!f->service.pauseForStorage()&&f->service.apply(cfg)&&f->service.start(false,0)&&f->service.armEndpoint(true));
  Memory settings;
  mt::modules::ExtendedModuleJournal journal;
  mt::modules::ExtendedModuleEdit edit(f->service,journal,settings);
  assert(!journal.load(settings)&&edit.load());
  meshtastic_ModuleConfig config{};
  config.which_payload_variant=meshtastic_ModuleConfig_mesh_beacon_tag;
  auto &beacon=config.payload_variant.mesh_beacon;
  beacon.flags=3;
  strcpy(beacon.broadcast_message,"test offer");
  beacon.has_broadcast_offer_preset=true;
  beacon.broadcast_targets_count=1;
  beacon.broadcast_targets[0].has_channel_index=true;
  beacon.broadcast_targets[0].channel_index=targetIndex;
  assert(!edit.save(config,10,false));
  mt::modules::BeaconBroadcaster broadcaster;
  mt::modules::BeaconTargetRuntime targets(f->service);
  const uint32_t epoch=71,now=60000;
  assert(f->clock.set(now));
  broadcaster.pollTargets(f->service,edit,true,now,0,2001,[&](const uint8_t *data,size_t size,uint32_t id,unsigned channel){
   assert(f->c.modulePacket(data,size,id,channel,mt::Companion::WorkspaceProof{true}));
  },targets,epoch);
  assert(targets.queued()==1&&f->usb.out.size()==1&&f->radio.frames.empty());
  const auto &message=f->usb.out.front();
  assert(message.which_payload_variant==meshtastic_FromRadio_packet_tag);
  const auto &packet=message.packet;
  assert(packet.channel==targetIndex&&packet.from==f->local.binding.node&&packet.to==UINT32_MAX&&
         packet.id&&packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
         packet.decoded.portnum==meshtastic_PortNum_MESH_BEACON_APP&&f->ledger.sent(packet.id));
  targets.poll(now+1,epoch,packet.from,true);
  assert(f->radio.frames.size()==1);
  const auto &frame=f->radio.frames.front();
  mt::Service::ChannelScope selected(f->service,targetIndex);
  assert(selected.valid()&&bench::le32(frame.data()+8)==packet.id&&frame[13]==f->service.codec.channelHash());
  const bool sign=policy!=0&&!targetAead;
  const auto metadata=decoded(*f,frame,sign);
  assert(metadata.signatureLength==(sign?64u:0u)&&metadata.signatureVerified==sign&&f->signing.calls==(sign?1u:0u));
  assert(metadata.portnum==packet.decoded.portnum&&metadata.payloadLength==packet.decoded.payload.size&&
         !memcmp(metadata.payload,packet.decoded.payload.bytes,metadata.payloadLength));
  f->radio.done=true;
  targets.poll(now+2,epoch,packet.from,true);
  assert(!targets.busy()&&f->radio.sends==1);
 }
 group("enrolled MeshBeacon target ciphertext, verified signature, durable ID and stock FromRadio channel across all three policies and CTR, CCM or licensed plaintext");
}
void incomingApplication(unsigned port){auto f=std::make_unique<RuntimeFixture>();IdentityBinding b;KeyRef ref;assert(f->identity.binding(b,ref));auto d=announcement(port);d.reply_id=123;d.emoji=0x1f44d;d.bitfield=1;SignedData signedData;assert(PkiSigner::sign(f->identity,ref,b,f->signing,901,UINT32_MAX,d,signedData)==SigningResult::Ok);
 uint8_t raw[255]{};size_t n=0;assert(f->service.codec.encodeSignedApplication(b.node,901,signedData.bytes,signedData.size,raw,sizeof(raw),n));Bytes frame(raw,raw+n);auto m=decoded(*f,frame);assert(m.portnum==port&&mt::PublicApplication::received(f->service.codec,frame.data(),frame.size(),m));
 for(unsigned mode=0;mode<11;++mode){auto changed=m;auto body=frame;switch(mode){case 0:changed.signatureVerified=false;break;case 1:changed.signatureRejected=true;break;case 2:changed.signatureQuarantined=true;break;case 3:changed.channelDecoded=false;break;case 4:changed.wantResponse=true;break;case 5:changed.request_id=1;break;case 6:changed.reply_id^=1;break;case 7:changed.emoji^=1;break;case 8:body[0]=0;break;case 9:body[8]^=1;break;case 10:changed.signature[0]^=1;break;}assert(!mt::PublicApplication::received(f->service.codec,body.data(),body.size(),changed));}
 auto unknown=m;unknown.signatureVerified=false;unknown.signatureQuarantined=true;assert(!mt::PublicApplication::received(f->service.codec,frame.data(),frame.size(),unknown));auto client=meshtastic_MeshPacket{};client.which_payload_variant=meshtastic_MeshPacket_decoded_tag;client.to=UINT32_MAX;client.id=901;auto p=pb_istream_from_buffer(signedData.bytes,signedData.size);assert(pb_decode(&p,meshtastic_Data_fields,&client.decoded));assert(!mt::PublicApplication::client(client)&&!mt::PublicApplication::canonical(signedData.bytes,signedData.size));
 auto receiver=std::make_unique<RuntimeFixture>(true,false,43);mt::NodeCache::Entry peer{};peer.identity(b.node,81,0);peer.bytes[mt::NodeCache::Entry::KeyLength]=32;memcpy(peer.bytes+mt::NodeCache::Entry::Key,b.publicKey,32);
 assert(receiver->service.nodes.accept(peer,receiver->local.binding.node,0,0)>=0&&receiver->service.signature.trustPublicKey(b.node,b.publicKey,0));
 auto receive=[&](uint32_t now){receiver->service.rx(frame.data(),frame.size(),3,now,42);receiver->c.receive(frame.data(),frame.size(),-90,3,now,mt::Companion::WorkspaceProof{true});receiver->bc.receive(frame.data(),frame.size(),-90,3,now,mt::Companion::WorkspaceProof{true});};
 receive(1000);receive(1001);
 for(const auto*host:{&receiver->usb,&receiver->ble}){unsigned count=0;for(const auto&message:host->out)if(message.which_payload_variant==meshtastic_FromRadio_packet_tag&&uint32_t(message.packet.decoded.portnum)==port){++count;const auto&packet=message.packet;assert(packet.from==b.node&&packet.to==UINT32_MAX&&packet.id==901&&packet.xeddsa_signed&&!packet.pki_encrypted&&packet.public_key.size==32&&!memcmp(packet.public_key.bytes,b.publicKey,32));const auto&data=packet.decoded;assert(data.reply_id==123&&data.emoji==0x1f44d&&data.has_bitfield&&data.bitfield==1&&data.xeddsa_signature.size==64&&!memcmp(data.xeddsa_signature.bytes,m.signature,64));}assert(count==1);}
 assert(!receiver->radio.sends);
 group(port==37?"actual verified signed MeshBeacon traverses both Service USB/BLE paths, covered metadata and duplicate/trust gates":"actual verified signed Range preserves both Service USB/BLE paths and covered metadata");}
}
int main(){
#ifdef MT_TEST_STOCK_SIGNING_282
pinnedPolicies();
#endif
signingModes();policy();outgoing();noFallback();refusalAndCapacity();outgoingBeacon();targetedBeacons();incomingApplication(66);incomingApplication(37);}
