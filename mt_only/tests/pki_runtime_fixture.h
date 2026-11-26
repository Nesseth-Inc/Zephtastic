#pragma once
// SPDX-License-Identifier: GPL-3.0-only
// Public deterministic keys, memory journals and mock radios. Never target-linked.
#include "Companion.h"
#include "known_channel_fixture.h"
#include "pki/PkiRuntime.h"
#include "pki/PkiLedger.h"
#include "pki/PsaPkiBackend.h"
#include "monocypher.h"
extern "C" psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *,uint8_t *,size_t,size_t *length){*length=0;return PSA_ERROR_INSUFFICIENT_ENTROPY;}
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using namespace mt::pki;
using Bytes=std::vector<uint8_t>;
struct Key {
  uint8_t scalar[32]{};IdentityBinding binding;
  explicit Key(unsigned seed){for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed+31*i);scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;
    crypto_x25519_public_key(binding.publicKey,scalar);binding.node=bench::MtSignature::nodeId(binding.publicKey);binding.generation=1;}
  ~Key(){crypto_wipe(scalar,32);}
};
struct Memory:mt::SettingsBackend {
  std::map<std::string,Bytes> values;unsigned writes=0;int fault=0;std::function<void()> onWrite;
  int read(const char *key,uint8_t *out,size_t cap)override{auto f=values.find(key);if(f==values.end())return -ENOENT;assert(f->second.size()<=cap);memcpy(out,f->second.data(),f->second.size());return int(f->second.size());}
  int write(const char *key,const uint8_t *p,size_t n)override{++writes;if(onWrite)onWrite();if(fault==1)return -EIO;values[key]={p,p+n};return fault==2?-EIO:0;}
};
struct Radio:mt::Radio {
  unsigned sends=0,starts=0,stops=0,restores=0;int healthValue=0,startError=0,stopError=0,restoreError=0;
  bool busyValue=false,done=false,stuckOff=false,stuckOn=false;std::vector<Bytes> frames;
  bool ready()override{return true;}int start()override{++starts;if(!startError&&!stuckOff)healthValue=1;return startError;}
  int stop()override{++stops;if(!stopError&&!stuckOn)healthValue=0;return stopError;}
  int restore()override{++restores;if(!restoreError&&!stuckOff)healthValue=1;return restoreError;}int health()override{return healthValue;}bool busy()override{return busyValue;}
  uint32_t airtime(size_t)override{return 100;}int send(uint8_t *p,size_t n)override{++sends;frames.emplace_back(p,p+n);done=false;return 0;}
  bool completion(int &r)override{r=0;return done;}
};
struct Gate:LedgerStorageGate {mt::Service *service=nullptr;bool quiescent()const override{return service&&service->privateStoragePaused();}};
struct Entropy:EntropySource {unsigned calls=0;bool fail=false;bool fill(uint8_t *p,size_t n)override{++calls;memset(p,0x71,n);return !fail;}};
struct SignEntropy:SigningEntropy {bool fail=false;unsigned calls=0;bool hedge(uint8_t out[32])override{++calls;memset(out,0x43,32);return !fail;}};
struct Backend:mt::experimental::PkiBackend {
  PsaPkiBackend real;unsigned encrypts=0,decrypts=0;bool failure=false;
  Backend(){assert(real.begin());}
  bool healthy()const override{return real.healthy();}
  bool sha256(const uint8_t *p,size_t n,uint8_t out[32])override{return real.sha256(p,n,out);}
  bool hmac256(const uint8_t key[32],const uint8_t *p,size_t n,uint8_t out[32])override{return real.hmac256(key,p,n,out);}
  bool ccm(bool enc,const uint8_t key[32],const uint8_t nonce[13],const uint8_t *p,size_t n,uint8_t *out,uint8_t tag[8])override{
    enc?++encrypts:++decrypts;if(failure){memset(out,0xa5,n);return false;}return real.ccm(enc,key,nonce,p,n,out,tag);}
};
struct Host:mt::Companion::Host {
  std::vector<meshtastic_FromRadio> out;std::vector<Bytes> records;unsigned previews=0;
  uint32_t previewFrom=0,previewTo=0,previewId=0;unsigned previewPort=0;bool previewLocal=false;
  bool ready()const override{return true;}int save(const mt::Config &)override{return -ENOTSUP;}
  bool send(const uint8_t *p,size_t n)override {records.emplace_back(p+4,p+n);meshtastic_FromRadio v{};auto s=pb_istream_from_buffer(p+4,n-4);assert(pb_decode(&s,meshtastic_FromRadio_fields,&v));out.push_back(v);return true;}
  void preview(uint32_t from,uint32_t to,uint32_t id,unsigned port,const uint8_t *,size_t,int16_t,int8_t,uint32_t,bool local)override{
    ++previews;previewFrom=from;previewTo=to;previewId=id;previewPort=port;previewLocal=local;
  }
};
struct HotObservation {unsigned calls=0;uint32_t from=0,utc=0,uptime=0;bool viaMqtt=false;
  static void receive(void *ctx,uint32_t node,uint32_t epoch,uint32_t seconds,bool mqtt){
    auto &self=*static_cast<HotObservation*>(ctx);++self.calls;self.from=node;self.utc=epoch;self.uptime=seconds;self.viaMqtt=mqtt;
  }
};
static void input(mt::Companion &c,const meshtastic_ToRadio &r,uint32_t now=100) {uint8_t bytes[512];auto s=pb_ostream_from_buffer(bytes,512);assert(pb_encode(&s,meshtastic_ToRadio_fields,&r));c.packet(bytes,s.bytes_written,now,mt::Companion::WorkspaceProof{true});}
static Bytes data(const char *text="private",uint32_t reply=0) {meshtastic_Data d{};d.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;d.payload.size=strlen(text);memcpy(d.payload.bytes,text,d.payload.size);d.has_bitfield=true;d.reply_id=reply;uint8_t out[227];auto s=pb_ostream_from_buffer(out,sizeof(out));assert(pb_encode(&s,meshtastic_Data_fields,&d));return {out,out+s.bytes_written};}
static void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
struct Fixture {
  Key local{11},peer{97};Memory im,lm;MonocypherIdentityDeriver deriver;IdentityJournal identity;PkiLedger ledger;Gate gate;Entropy entropy;
  SignEntropy signing;Backend backend;mt::experimental::PkiCrypto crypto{backend};LedgerNonceAuthority nonce{identity,ledger,lm,entropy,gate};LedgerReplayAuthority replay{identity,ledger,lm,gate};
  mt::test::KnownChannelFixtureClock knownChannelClock;
  PkiRuntime runtime;Radio radio;mt::Service s{radio,true,local.binding.node,81,true,false,&runtime};
  Host usb,ble;mt::companion::CompanionWorkspace workspace;
  HotObservation hot;
  mt::Companion c{s,usb,81,workspace,0},bc{s,ble,81,workspace,1};
  explicit Fixture(bool sign=false,bool discovery=false):runtime(crypto,backend,identity,nonce,replay,sign?&signing:nullptr),s(radio,true,local.binding.node,81,true,discovery,&runtime){
    knownChannelClock.bind(s);
    assert(s.bindHotNodeObservationSink(HotObservation::receive,&hot));
    assert(!identity.load(im,deriver));IdentityCandidate candidate(local.binding,local.scalar);assert(!identity.store(im,deriver,candidate,true));
    assert(!ledger.load(lm)&&!ledger.initialize(lm,local.binding.publicKey,local.binding.node,true));
    assert(!ledger.addTrustedPeer(lm,peer.binding.node,peer.binding.publicKey,true));
    assert(runtime.open(local.binding.node,peer.binding.node));gate.service=&s;
    mt::Config cfg;cfg.bytes[4]=1;auto n=cfg.node();n.setPublic();cfg.node(n);assert(s.apply(cfg)&&s.start(false,0)&&s.armEndpoint(true));
    lm.onWrite=[this]{assert(s.privateStoragePaused()&&!radio.healthValue&&!s.active());};
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=1;input(c,r,1);input(bc,r,1);usb.out.clear();ble.out.clear();usb.records.clear();ble.records.clear();
  }
  meshtastic_ToRadio request(uint32_t id=10){meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;p.id=id;p.to=peer.binding.node;p.from=0xdeadbeef;p.want_ack=true;p.hop_limit=3;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;p.decoded.payload.size=7;memcpy(p.decoded.payload.bytes,"private",7);return r;}
  void send(uint32_t id=10){input(c,request(id));}
  void poll(uint32_t now=1000){s.poll(now,71);c.poll(mt::Companion::WorkspaceProof{true});bc.poll(mt::Companion::WorkspaceProof{true});}
  void complete(uint32_t now=1200){radio.done=true;poll(now);}
  Bytes incoming(uint32_t id=90,const char *text="incoming",bool want=true){auto plain=data(text);uint8_t encrypted[239];size_t n=0;assert(crypto.encrypt(peer.scalar,local.binding.publicKey,peer.binding.node,id,0x12345678,plain.data(),plain.size(),encrypted,sizeof(encrypted),n));Bytes frame(n+16);put(frame.data(),local.binding.node);put(frame.data()+4,peer.binding.node);put(frame.data()+8,id);frame[12]=want?8:0;frame[15]=uint8_t(peer.binding.node);memcpy(frame.data()+16,encrypted,n);return frame;}
  Bytes ack(uint32_t request=10,bool proof=true,uint32_t code=0,uint32_t from=0){
    if(!from)from=peer.binding.node;
    uint8_t routing[12],raw[2]={24,uint8_t(code)};size_t rn=2;memcpy(routing,raw,2);
    if(proof){mt::experimental::PkiAck a(crypto);assert(a.attach(peer.scalar,local.binding.publicKey,{true,from,local.binding.node,request},raw,2,routing,sizeof(routing),rn));}
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_ROUTING_APP;d.request_id=request;d.has_bitfield=true;d.payload.size=rn;memcpy(d.payload.bytes,routing,rn);
    uint8_t bytes[239],frame[255];auto b=pb_ostream_from_buffer(bytes,239);assert(pb_encode(&b,meshtastic_Data_fields,&d));size_t len;
    assert(s.codec.encodeZeroHopData(from,local.binding.node,request+1000,bytes,b.bytes_written,frame,255,len));return {frame,frame+len};
  }
  void rx(const Bytes &f,uint32_t now=1500){s.rx(f.data(),f.size(),3,now,81,true,nullptr,false,1792660000u+now/1000u,now/1000u);c.receive(f.data(),f.size(),-80,3,now,mt::Companion::WorkspaceProof{true});bc.receive(f.data(),f.size(),-80,3,now,mt::Companion::WorkspaceProof{true});}
  unsigned delivered(const Host &h)const{unsigned n=0;for(auto &r:h.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.pki_encrypted)++n;return n;}
  unsigned result(uint32_t id,bool authenticated,int code=-1)const{unsigned n=0;for(auto &r:usb.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP&&r.packet.decoded.request_id==id&&
    (r.packet.ack_proof_status==meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_VALID)==authenticated){meshtastic_Routing rt{};auto b=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);assert(pb_decode(&b,meshtastic_Routing_fields,&rt));if(code<0||int(rt.error_reason)==code)++n;}
    return n;}
};
