// SPDX-License-Identifier: GPL-3.0-only
// Service-level private UI admission tests. Public deterministic keys and
// memory-only storage; no device, RF, or production identity is involved.
#include "pki_runtime_fixture.h"
#include "pki/MultiPeerRuntime.h"
#include <climits>

namespace {
using namespace mt::pki;

struct CapturingRuntime:mt::pki::PrivateRuntime {
  mt::pki::MultiPeerRuntime &inner;
  uint8_t queuedChannel=UINT8_MAX;
  unsigned submitCalls=0;
  explicit CapturingRuntime(mt::pki::MultiPeerRuntime &runtime):inner(runtime){}
  uint32_t peer()const override{return inner.peer();}
  void hopLimit(uint8_t value)override{inner.hopLimit(value);}
  void deviceRole(uint8_t value)override{inner.deviceRole(value);}
  void ownerUnmessagable(bool value)override{inner.ownerUnmessagable(value);}
  void rebroadcastMode(uint8_t value)override{inner.rebroadcastMode(value);}
  mt::pki::PrivateReplyDiagnostics replyDiagnostics()const override{return inner.replyDiagnostics();}
  bool ready(uint32_t local)const override{return inner.ready(local);}
  bool dormant(uint32_t local)const override{return inner.dormant(local);}
  bool publicIdentity(uint8_t out[32])const override{return inner.publicIdentity(out);}
  bool peerPublicIdentity(uint32_t node,uint8_t out[32])const override{return inner.peerPublicIdentity(node,out);}
  bool bootstrap(bench::MtCodec &codec,const bench::MtNodeRecord &record,uint8_t model,
      uint32_t to,uint32_t id,bool wantResponse,uint32_t request,bench::Frame &out)override{
    return inner.bootstrap(codec,record,model,to,id,wantResponse,request,out);
  }
  bool broadcast(bench::MtCodec &codec,uint32_t id,const uint8_t *data,size_t size,bench::Frame &out)override{
    return inner.broadcast(codec,id,data,size,out);
  }
  bool encodePublicApplication(bench::MtCodec &codec,uint32_t to,uint32_t id,const uint8_t *data,
      size_t size,bench::Frame &out)override{return inner.encodePublicApplication(codec,to,id,data,size,out);}
  bool encodeChannelControl(bench::MtCodec &codec,uint32_t to,uint32_t id,const uint8_t *data,
      size_t size,bench::Frame &out)override{return inner.encodeChannelControl(codec,to,id,data,size,out);}
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  bool encodeStoreForward(bench::MtCodec &codec,const mt::modules::StoreForwardPacket &packet,
      uint32_t id,bench::Frame &out)override{return inner.encodeStoreForward(codec,packet,id,out);}
#endif
  void freeze()override{inner.freeze();}
  bool acceptsPeerKey(uint32_t node,const uint8_t *key,size_t size)const override{return inner.acceptsPeerKey(node,key,size);}
  bool matchesPeer(uint32_t node)const override{return inner.matchesPeer(node);}
  bool revoke(uint32_t node)override{return inner.revoke(node);}
  void clearEvent()override{inner.clearEvent();}
  void sweep(mt::Endpoint &endpoint)override{inner.sweep(endpoint);}
  int submit(mt::Endpoint &endpoint,uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,
      size_t size,unsigned hop,bool reliable,const uint8_t *key,size_t keySize,bool explicitPki,
      uint32_t now)override{
    ++submitCalls;
    const int result=inner.submit(endpoint,owner,to,id,data,size,hop,reliable,key,keySize,explicitPki,now);
    if(!result){auto *job=endpoint.next(now);if(job&&bench::le32(job->frame.bytes+8)==id)queuedChannel=job->channelSlot;}
    return result;
  }
  void receive(mt::Endpoint &endpoint,bench::MtCodec &codec,const uint8_t *data,size_t size,
      uint32_t now,uint32_t random,bool armed)override{inner.receive(endpoint,codec,data,size,now,random,armed);}
  void proof(mt::Endpoint &endpoint,const uint8_t *data,size_t size,
      const bench::RouteMetadata &metadata,uint32_t now)override{inner.proof(endpoint,data,size,metadata,now);}
  bool event(uint8_t owner,const uint8_t *data,size_t size,mt::pki::PrivateEvent &out)override{
    return inner.event(owner,data,size,out);
  }
};

struct ServiceFixture {
  Key local{11},peers[2]{Key{97},Key{61}};
  Memory im,lm;MonocypherIdentityDeriver deriver;IdentityJournal identity;PkiLedger ledger;
  Gate gate;Entropy entropy;SignEntropy signing;Backend backend;
  mt::experimental::PkiCrypto crypto{backend};
  LedgerNonceAuthority nonce{identity,ledger,lm,entropy,gate};
  LedgerReplayAuthority replay{identity,ledger,lm,gate};
  MultiPeerRuntime runtime{crypto,backend,identity,nonce,replay,&signing};
  PkiPeerSet pins;CapturingRuntime capture{runtime};Radio radio;
  mt::test::KnownChannelFixtureClock knownChannelClock;
  mt::Service service{radio,true,local.binding.node,127,true,false,&capture};
  Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion companion{service,usb,127,workspace,0},bleCompanion{service,ble,127,workspace,1};

  explicit ServiceFixture(bool secondary=false) {
    assert(!identity.load(im,deriver));
    IdentityCandidate candidate(local.binding,local.scalar);
    assert(!identity.store(im,deriver,candidate,true));
    assert(!ledger.load(lm));
    assert(!ledger.initialize(lm,local.binding.publicKey,local.binding.node,true));
    PkiPeerPin values[2]{};
    for(unsigned i=0;i<2;++i) {
      values[i].node=peers[i].binding.node;
      memcpy(values[i].publicKey,peers[i].binding.publicKey,32);
      assert(!ledger.addTrustedPeer(lm,values[i].node,values[i].publicKey,true));
    }
    assert(pins.assign(local.binding,values,2));
    assert(runtime.open(local.binding.node,pins));
    gate.service=&service;
    knownChannelClock.bind(service);

    mt::Config cfg;cfg.bytes[4]=1;auto node=cfg.node();node.setPublic();cfg.node(node);
    const uint8_t primaryKey[16]={0x21,0x35,0x49,0x5d,0x61,0x75,0x89,0x9d,
      0xa1,0xb5,0xc9,0xdd,0xe1,0xf5,0x09,0x1d};
    mt::PrimaryChannel::Selection primary;
    assert(mt::PrimaryChannel::normalize(primaryKey,sizeof(primaryKey),primary));
    node.setKey(primary.hash,primary.bytes,primary.length);cfg.node(node);
    mt::PrimaryChannel::clear(primary);
    if(secondary) {
      const uint8_t key[16]={0x4a,0x57,0x14,0x92,0x13,0x51,0x16,0xa0,
        0xb9,0x82,0x26,0x18,0xfa,0x27,0x6b,0x3c};
      mt::ChannelRecord record;
      assert(record.set(2,"M141-test",key,sizeof(key)));
      assert(cfg.setSecondaryChannel(1,record));
    }
    assert(service.apply(cfg)&&service.start(false,0)&&service.armEndpoint(true));
    lm.onWrite=[this]{assert(service.privateStoragePaused()&&!radio.healthValue&&!service.active());};
  }

  int sendTo(uint32_t to,unsigned channel,uint32_t random,uint32_t &id,bool reliable=false) {
    static constexpr uint8_t text[]="private UI message";
    assert(knownChannelClock.set(100));
    return service.submitLocalPrivateInput(meshtastic_PortNum_TEXT_MESSAGE_APP,to,
      text,sizeof(text)-1,channel,reliable,100,random,id);
  }
  int send(unsigned peer,unsigned channel,uint32_t random,uint32_t &id,bool reliable=false) {
    return sendTo(peers[peer].binding.node,channel,random,id,reliable);
  }

  Bytes ack(unsigned signer,unsigned source,uint32_t wireRequest,uint32_t signedRequest,
            unsigned channel,uint32_t packetId) {
    mt::PrimaryChannel::Selection selection;
    assert(service.config.channelSelection(channel,selection));
    bench::MtCodec channelCodec;
    assert(channelCodec.setKey(selection.hash,selection.bytes,selection.length));
    mt::PrimaryChannel::clear(selection);
    uint8_t routing[12]{},raw[2]={24,0};size_t routingSize=0;
    mt::experimental::PkiAck proof(crypto);
    assert(proof.attach(peers[signer].scalar,local.binding.publicKey,
      {true,peers[source].binding.node,local.binding.node,signedRequest},raw,sizeof(raw),
      routing,sizeof(routing),routingSize));
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_ROUTING_APP;
    data.request_id=wireRequest;data.has_bitfield=true;data.payload.size=routingSize;
    memcpy(data.payload.bytes,routing,routingSize);
    uint8_t payload[227]{},frame[255]{};auto output=pb_ostream_from_buffer(payload,sizeof(payload));
    assert(pb_encode(&output,meshtastic_Data_fields,&data));size_t frameSize=0;
    assert(channelCodec.encodeZeroHopData(peers[source].binding.node,local.binding.node,
      packetId,payload,output.bytes_written,frame,sizeof(frame),frameSize));
    return {frame,frame+frameSize};
  }
  void receiveAck(const Bytes &frame,uint32_t now) {
    assert(knownChannelClock.set(now));
    service.rx(frame.data(),frame.size(),-70,now,0x3141,true);
  }

  Bytes transmit(uint32_t now=1000) {
    assert(knownChannelClock.set(now));
    service.poll(now,0x3179);
    assert(radio.frames.size()==1);
    return radio.frames.front();
  }

  void complete(uint32_t now=1300) {
    assert(knownChannelClock.set(now));
    radio.done=true;service.poll(now,0x317a);
  }
};

static void configureBare(mt::Service &service) {
  mt::Config config;config.bytes[4]=1;auto node=config.node();node.setPublic();config.node(node);
  assert(service.apply(config)&&service.start(false,0)&&service.armEndpoint(true));
}

static void actualServicePinsProduceBoundPrivateUiCompletions() {
  for(unsigned peer=0;peer<2;++peer) {
    ServiceFixture f;uint32_t id=0;
    assert(f.send(peer,0,0x51a10000u+peer,id,true)==0&&id);
    assert(f.service.selectedChannel()==0&&f.service.receiving()&&!f.service.faulted());
    const auto frame=f.transmit();
    assert(frame.size()>=29&&bench::le32(frame.data())==f.peers[peer].binding.node&&
      bench::le32(frame.data()+4)==f.local.binding.node&&bench::le32(frame.data()+8)==id);
    uint8_t plaintext[227]{};size_t plaintextSize=0;
    assert(f.crypto.decrypt(f.peers[peer].scalar,f.local.binding.publicKey,f.local.binding.node,id,
      frame.data()+16,frame.size()-16,plaintext,sizeof(plaintext),plaintextSize));
    meshtastic_Data data{};auto input=pb_istream_from_buffer(plaintext,plaintextSize);
    assert(pb_decode(&input,meshtastic_Data_fields,&data)&&!input.bytes_left&&
      data.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP&&!data.request_id&&!data.reply_id&&
      data.payload.size==sizeof("private UI message")-1&&
      !memcmp(data.payload.bytes,"private UI message",sizeof("private UI message")-1));
    uint8_t wrong[227]{};size_t wrongSize=99;Key other{0x52};
    assert(!f.crypto.decrypt(other.scalar,f.local.binding.publicKey,f.local.binding.node,id,
      frame.data()+16,frame.size()-16,wrong,sizeof(wrong),wrongSize)&&wrongSize==0);
    f.complete();mt::Endpoint::Result result{};
    assert(!f.service.takeLocalUiResult(result));
    f.receiveAck(f.ack(peer,peer,id,id,0,0x62001000u+peer),1500);
    assert(f.service.takeLocalUiResult(result)&&result.id==id&&result.peer==f.peers[peer].binding.node&&
      result.kind==mt::Endpoint::Kind::Authenticated);
    assert(!f.service.takeLocalUiResult(result));
    assert(f.usb.out.empty()&&f.ble.out.empty());
  }
  {
    ServiceFixture f;uint32_t id=0;assert(f.send(1,0,0x51a10010,id,true)==0&&id);
    (void)f.transmit();f.complete();
    f.receiveAck(f.ack(0,0,id,id,0,0x62001010),1500); // Valid other-peer proof, no matching request.
    f.receiveAck(f.ack(0,1,id,id,0,0x62001011),1600); // Target peer on wire, wrong signer.
    f.receiveAck(f.ack(1,1,id,id+1,0,0x62001012),1700); // Correct signer, wrong signed request.
    mt::Endpoint::Result result{};assert(!f.service.takeLocalUiResult(result));
    f.receiveAck(f.ack(1,1,id,id,0,0x62001013),1800);
    assert(f.service.takeLocalUiResult(result)&&result.id==id&&result.peer==f.peers[1].binding.node&&
      result.kind==mt::Endpoint::Kind::Authenticated);
    assert(f.usb.out.empty()&&f.ble.out.empty());
  }
  std::cout<<"PASS Service UI first/non-first pinned sends decrypt only for the destination; only the matching authenticated ACK completes the local UI lane\n";
}

static void rejectedDestinationsAndModesDoNotReserveOrQueue() {
  ServiceFixture f;uint32_t id=0;const unsigned writes=f.lm.writes,entropy=f.entropy.calls;
  const auto free=f.service.endpointFree();
  assert(f.send(0,8,0x51a11001,id)==-EINVAL&&id==0);
  assert(f.send(0,7,0x51a11002,id)==-ENODEV&&id==0);
  assert(f.sendTo(0x23456321,0,0x51a11003,id)==-EPERM&&id==0);
  assert(f.lm.writes==writes&&f.entropy.calls==entropy&&f.service.endpointFree()==free&&
    f.radio.sends==0&&f.radio.frames.empty()&&f.service.receiving()&&!f.service.faulted());
  assert(f.service.revokePrivatePeer(f.peers[1].binding.node));
  const unsigned revokedWrites=f.lm.writes,revokedEntropy=f.entropy.calls;
  assert(f.send(1,0,0x51a11004,id)==-EPERM&&id==0);
  assert(f.lm.writes==revokedWrites&&f.entropy.calls==revokedEntropy&&f.service.endpointFree()==free&&
    f.radio.sends==0&&f.radio.frames.empty()&&f.service.receiving()&&!f.service.faulted());

  assert(f.service.revokePrivatePeer(f.peers[0].binding.node)&&f.runtime.dormant(f.local.binding.node));
  const unsigned dormantWrites=f.lm.writes,dormantEntropy=f.entropy.calls;
  assert(f.send(0,0,0x51a11005,id)==-EACCES&&id==0);
  assert(f.lm.writes==dormantWrites&&f.entropy.calls==dormantEntropy&&
    f.service.endpointFree()==free&&f.radio.sends==0&&f.radio.frames.empty());

  Radio radio;mt::Service noRuntime{radio,true,0x12345678,127,true,false,nullptr};
  configureBare(noRuntime);static constexpr uint8_t text[]="private UI message";
  const auto bareFree=noRuntime.endpointFree();
  assert(noRuntime.submitLocalPrivateInput(meshtastic_PortNum_TEXT_MESSAGE_APP,
    0x23456321,text,sizeof(text)-1,0,false,100,0x51a11006,id)==-EACCES&&id==0);
  assert(noRuntime.endpointFree()==bareFree&&radio.sends==0&&radio.frames.empty()&&noRuntime.receiving());
  std::cout<<"PASS invalid-channel, unknown/revoked/dormant peer and no-runtime submissions reject before durable admission or RF\n";
}

static void secondaryChannelScopeRestoresAndUiFlightIsSingle() {
  ServiceFixture f(true);uint32_t id=0;const unsigned writes=f.lm.writes,entropy=f.entropy.calls;
  assert(f.send(1,1,0x51a12001,id)==0&&id);
  assert(f.service.selectedChannel()==0&&f.service.receiving()&&!f.service.faulted());
  assert(f.capture.submitCalls==1&&f.capture.queuedChannel==1);
  uint32_t rejectedId=0;
  assert(f.send(0,0,0x51a12002,rejectedId,true)==-EBUSY&&rejectedId==0);
  assert(f.lm.writes==writes+1&&f.entropy.calls==entropy+1&&f.service.endpointFree()==3&&
    f.radio.sends==0&&f.radio.frames.empty()&&f.service.selectedChannel()==0&&f.service.receiving());
  const auto frame=f.transmit();
  mt::PrimaryChannel::Selection selection;assert(f.service.config.channelSelection(1,selection));
  assert(frame[13]==0&&f.capture.queuedChannel==1&&bench::le32(frame.data())==f.peers[1].binding.node);
  uint8_t plain[227]{};size_t plainSize=0;
  assert(f.crypto.decrypt(f.peers[1].scalar,f.local.binding.publicKey,f.local.binding.node,
    bench::le32(frame.data()+8),frame.data()+16,frame.size()-16,plain,sizeof(plain),plainSize));
  mt::PrimaryChannel::clear(selection);
  f.complete();mt::Endpoint::Result result{};
  assert(f.service.takeLocalUiResult(result)&&result.id==id&&result.kind==mt::Endpoint::Kind::Local);
  std::cout<<"PASS secondary-channel UI job retains its channel after Service restores primary selection; concurrent UI flight consumes no second durable nonce\n";
}

static void storageFailureDoesNotPublishAndRestoresRx() {
  ServiceFixture f;uint32_t id=0;f.lm.fault=1;
  const auto before=f.lm.writes,entropy=f.entropy.calls,free=f.service.endpointFree();
  const int result=f.send(0,0,0x51a13001,id);
  assert(result<0&&id==0&&f.lm.writes>before&&f.entropy.calls==entropy);
  assert(!f.service.receiving()&&f.service.faulted()&&!f.service.endpointArmed()&&
    !f.service.privateStoragePaused()&&f.service.endpointFree()==free&&
    f.radio.sends==0&&f.radio.frames.empty());
  mt::Endpoint::Result ui{};assert(!f.service.takeLocalUiResult(ui));
  std::cout<<"PASS durable-reservation failure reports refusal, fails closed with RX disarmed, and creates neither private RF work nor public fallback\n";
}
}

int main() {
  actualServicePinsProduceBoundPrivateUiCompletions();
  rejectedDestinationsAndModesDoNotReserveOrQueue();
  secondaryChannelScopeRestoresAndUiFlightIsSingle();
  storageFailureDoesNotPublishAndRestoresRx();
}
