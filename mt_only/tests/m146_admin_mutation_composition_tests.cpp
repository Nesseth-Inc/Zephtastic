// Full host-only Service/PkiRuntime/Companion mutation composition.
// Storage is an in-memory lifecycle fake; no target, radio, BLE, or RF is used.
#define main m143_private_admin_responder_regressions_unused_main
#include "m143_private_admin_responder_tests.cpp"
#undef main

namespace {

struct CompositionHost final : Host {
  mt::Service *service=nullptr;
  meshtastic_Config_SecurityConfig security{};
  mt::Config durable{};
  uint32_t monotonic=1000;
  unsigned passkeyCalls=0,saveCalls=0,saveWhileReceiving=0;
  bool saveSawPaused=false;

  bool adminKeys(meshtastic_Config_SecurityConfig &out) const override {
    out=security;return true;
  }
  bool adminPasskey(uint8_t *out,size_t size) override {
    ++passkeyCalls;
    if(!out||size!=8)return false;
    memset(out,0x5A,size);return true;
  }
  uint32_t adminSessionNow() const override {return monotonic;}
  int save(const mt::Config &candidate) override {
    ++saveCalls;
    if(!service||!service->receiving()||service->privateStoragePaused()) {
      ++saveWhileReceiving;
      saveSawPaused=service&&service->privateStoragePaused();
      return -EIO;
    }
    const bool wasArmed=service->endpointArmed();
    service->stop();
    durable=candidate; // Deliberately simple memory-backed persistence model.
    if(!service->apply(candidate)||!service->start(false,monotonic)||
       (wasArmed&&!service->armEndpoint(true)))return -EIO;
    return 0;
  }
};

struct CompositionFixture {
  Key local{11},peer{97};Memory im,lm;MonocypherIdentityDeriver deriver;
  IdentityJournal identity;PkiLedger ledger;Gate gate;Entropy entropy;SignEntropy signing;
  Backend backend;mt::experimental::PkiCrypto crypto{backend};
  LedgerNonceAuthority nonce{identity,ledger,lm,entropy,gate};
  LedgerReplayAuthority replay{identity,ledger,lm,gate};
  PkiRuntime runtime;Radio radio;
  mt::Service service{radio,true,local.binding.node,81,true,false,&runtime};
  mt::test::KnownChannelFixtureClock knownChannelClock;
  Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion usbCompanion{service,usb,81,workspace,0},bleCompanion{service,ble,81,workspace,1};
  CompositionHost adminHost;mt::Companion adminCompanion{service,adminHost,81,workspace,2};

  CompositionFixture():runtime(crypto,backend,identity,nonce,replay,&signing) {
    assert(!identity.load(im,deriver));
    IdentityCandidate candidate(local.binding,local.scalar);
    assert(!identity.store(im,deriver,candidate,true));
    assert(!ledger.load(lm)&&!ledger.initialize(lm,local.binding.publicKey,local.binding.node,true));
    assert(!ledger.addTrustedPeer(lm,peer.binding.node,peer.binding.publicKey,true));
    assert(runtime.open(local.binding.node,peer.binding.node));
    gate.service=&service;knownChannelClock.bind(service);
    adminHost.service=&service;
    adminHost.security.admin_key_count=1;
    adminHost.security.admin_key[0].size=32;
    memcpy(adminHost.security.admin_key[0].bytes,peer.binding.publicKey,32);

    assert(service.bindPrivateAdminSnapshot(
      [](void *ctx,uint32_t localNode,uint32_t from,const uint8_t key[32],uint32_t id,
         const uint8_t *request,size_t requestSize,uint8_t *out,size_t capacity,
         size_t &written)->bool {
        return static_cast<mt::Companion*>(ctx)->privateAdminReplyData(localNode,from,key,id,
            request,requestSize,out,capacity,written,mt::Companion::WorkspaceProof{true});
      },&adminCompanion));
    assert(service.bindPrivateAdminMutation(
      [](void *ctx,uint32_t localNode,uint32_t from,const uint8_t key[32],uint32_t id,
         const uint8_t *request,size_t requestSize,uint32_t now,uint32_t &routingError)->bool {
        return static_cast<mt::Companion*>(ctx)->privateAdminMutation(localNode,from,key,id,
            request,requestSize,now,routingError,mt::Companion::WorkspaceProof{true});
      },&adminCompanion));

    mt::Config cfg;cfg.bytes[4]=1;auto node=cfg.node();node.setPublic();cfg.node(node);
    assert(service.apply(cfg)&&service.start(false,0)&&service.armEndpoint(true));
    meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
    hello.want_config_id=1;input(usbCompanion,hello,1);input(bleCompanion,hello,1);
    usb.out.clear();ble.out.clear();usb.records.clear();ble.records.clear();
    adminHost.durable=service.config;
    lm.onWrite=[this]{assert(service.privateStoragePaused()&&!radio.healthValue&&!service.active());};
  }
  void receive(const Bytes &frame,uint32_t now) {
    adminHost.monotonic=now;
    service.rx(frame.data(),frame.size(),3,now,81);
    usbCompanion.receive(frame.data(),frame.size(),-80,3,now,mt::Companion::WorkspaceProof{true});
    bleCompanion.receive(frame.data(),frame.size(),-80,3,now,mt::Companion::WorkspaceProof{true});
  }
  void poll(uint32_t now) {
    adminHost.monotonic=now;
    service.poll(now,71);usbCompanion.poll(mt::Companion::WorkspaceProof{true});bleCompanion.poll(mt::Companion::WorkspaceProof{true});adminCompanion.poll(mt::Companion::WorkspaceProof{true});
  }
  void finishTransmission(uint32_t now) {
    radio.done=true;poll(now);
  }
  bool status(mt::pki::PkiLedger::Status &out) const {return ledger.status(out);}
};

Bytes ownerSetter(const uint8_t token[8],const char *name) {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;
  strcpy(admin.set_owner.long_name,name);
  admin.session_passkey.size=8;memcpy(admin.session_passkey.bytes,token,8);
  uint8_t payload[233]{};auto ps=pb_ostream_from_buffer(payload,sizeof(payload));
  assert(pb_encode(&ps,meshtastic_AdminMessage_fields,&admin));
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_ADMIN_APP;
  data.want_response=true;data.has_bitfield=true;data.bitfield=2;
  data.payload.size=ps.bytes_written;memcpy(data.payload.bytes,payload,data.payload.size);
  uint8_t wire[227]{};auto ds=pb_ostream_from_buffer(wire,sizeof(wire));
  assert(pb_encode(&ds,meshtastic_Data_fields,&data));return {wire,wire+ds.bytes_written};
}

void decodePkiResponse(CompositionFixture &f,size_t index,uint32_t request,
                       meshtastic_PortNum expectedPort,uint32_t expectedError,
                       uint8_t tokenOut[8]=nullptr) {
  assert(index<f.radio.frames.size());
  const auto &frame=f.radio.frames[index];
  assert(frame.size()>16&&bench::le32(frame.data())==f.peer.binding.node&&
         bench::le32(frame.data()+4)==f.local.binding.node&&frame[13]==0);
  const uint32_t packetId=bench::le32(frame.data()+8);
  assert(packetId&&f.ledger.sent(packetId));
  uint8_t plaintext[227]{};size_t plaintextSize=0;
  // The remote recipient verifies using its own private key and the pinned
  // sender public key, matching actual peer-side decryption semantics.
  assert(f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,f.local.binding.node,
      packetId,frame.data()+16,frame.size()-16,plaintext,sizeof(plaintext),plaintextSize));
  meshtastic_Data data{};auto ds=pb_istream_from_buffer(plaintext,plaintextSize);
  assert(pb_decode(&ds,meshtastic_Data_fields,&data)&&!ds.bytes_left);
  assert(data.portnum==expectedPort&&data.request_id==request&&!data.want_response);
  if(expectedPort==meshtastic_PortNum_ADMIN_APP) {
    meshtastic_AdminMessage admin{};auto as=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
    assert(pb_decode(&as,meshtastic_AdminMessage_fields,&admin)&&!as.bytes_left);
    mt::admin::remote_protocol::Query query{mt::admin::remote_protocol::Operation::Owner,0};
    assert(mt::admin::remote_protocol::canonicalGetResponse(plaintext,plaintextSize,query,request));
    assert(admin.which_payload_variant==meshtastic_AdminMessage_get_owner_response_tag&&
           admin.session_passkey.size==8);
    if(tokenOut)memcpy(tokenOut,admin.session_passkey.bytes,8);
    bench::wipeKeyBuffer(&admin,sizeof(admin));
    return;
  }
  uint8_t routing[233]{};size_t routingSize=0;
  mt::experimental::PkiAck proof(f.crypto);
  const mt::experimental::PendingAck pending{true,f.local.binding.node,f.peer.binding.node,request};
  const mt::experimental::AckEnvelope received{true,f.local.binding.node,f.peer.binding.node,request};
  assert(proof.verify(f.peer.scalar,f.local.binding.publicKey,pending,received,
      data.payload.bytes,data.payload.size,routing,sizeof(routing),routingSize)==
      mt::experimental::AckProofVerdict::ValidProof);
  meshtastic_Routing decoded{};auto rs=pb_istream_from_buffer(routing,routingSize);
  assert(pb_decode(&rs,meshtastic_Routing_fields,&decoded)&&!rs.bytes_left&&
         decoded.which_variant==meshtastic_Routing_error_reason_tag&&
         uint32_t(decoded.error_reason)==expectedError);
  const unsigned expectedHop=mt::HopPolicy::response(0,0,f.service.config.hopLimit());
  assert((frame[12]&7u)==expectedHop&&((frame[12]>>5)&7u)==expectedHop);
  bench::wipeKeyBuffer(routing,sizeof(routing));
}

void testReceiveStagePollApplyProofAndRestore() {
  CompositionFixture f;
  const auto original=f.service.config.node();
  mt::pki::PkiLedger::Status before{};assert(f.status(before));

  // Obtain a real passkey through authenticated Fresh GET ingress and its
  // encrypted same-peer Admin response.
  const Bytes get=adminRequest();
  const Bytes getFrame=authenticatedFrame(f,701,get,false);
  f.receive(getFrame,1000);
  assert(f.adminHost.passkeyCalls==1&&f.adminHost.saveCalls==0);
  assert(f.usb.previews==0&&f.ble.previews==0&&f.usb.out.empty()&&f.ble.out.empty());
  mt::pki::PrivateEvent event{};
  assert(!f.service.takePrivateEvent(0,getFrame.data(),getFrame.size(),event));
  assert(!f.service.takePrivateEvent(1,getFrame.data(),getFrame.size(),event));
  f.poll(1400);
  assert(f.radio.frames.size()==1);
  mt::pki::PkiLedger::Status afterGet{};assert(f.status(afterGet));
  assert(afterGet.sent==uint16_t(before.sent+1)&&afterGet.received==uint16_t(before.received+1));
  uint8_t token[8]{};
  decodePkiResponse(f,0,701,meshtastic_PortNum_ADMIN_APP,0,token);
  f.finishTransmission(1800);

  // Fresh authenticated SET ingress commits replay during RX but only stages
  // bytes there. The typed setter and storage lifecycle run from Service.poll.
  const Bytes setter=ownerSetter(token,"M146-remote-owner");
  const Bytes setterFrame=authenticatedFrame(f,702,setter,false);
  mt::pki::PkiLedger::Status beforeSet{};assert(f.status(beforeSet));
  f.receive(setterFrame,2000);
  mt::pki::PkiLedger::Status afterRx{};assert(f.status(afterRx));
  assert(afterRx.received==uint16_t(beforeSet.received+1)&&afterRx.sent==beforeSet.sent);
  assert(f.adminHost.saveCalls==0&&f.adminHost.saveWhileReceiving==0&&
         f.service.receiving()&&f.radio.healthValue==1);
  assert(f.usb.previews==0&&f.ble.previews==0&&f.usb.out.empty()&&f.ble.out.empty());
  assert(!f.service.takePrivateEvent(0,setterFrame.data(),setterFrame.size(),event));
  assert(!f.service.takePrivateEvent(1,setterFrame.data(),setterFrame.size(),event));

  f.poll(2400);
  assert(f.adminHost.saveCalls==1&&f.adminHost.saveWhileReceiving==0&&!f.adminHost.saveSawPaused);
  const auto changed=f.service.config.node();
  assert(!strcmp(reinterpret_cast<const char*>(changed.bytes+bench::MtNodeRecord::Name),
                 "M146-remote-owner"));
  mt::pki::PkiLedger::Status afterApply{};assert(f.status(afterApply));
  assert(afterApply.sent==uint16_t(beforeSet.sent+1));
  // A byte-identical duplicate after staging is Seen: no second setter, replay
  // commit, or response nonce/result.
  f.receive(setterFrame,2500);
  mt::pki::PkiLedger::Status afterDuplicate{};assert(f.status(afterDuplicate));
  assert(f.adminHost.saveCalls==1&&afterDuplicate.received==afterRx.received&&
         afterDuplicate.sent==afterApply.sent);
  f.poll(2800);
  assert(f.radio.frames.size()==2);
  decodePkiResponse(f,1,702,meshtastic_PortNum_ROUTING_APP,
                    meshtastic_Routing_Error_NONE);
  f.finishTransmission(3200);

  // Restore the original owner name with a second authenticated mutation.
  const Bytes restore=ownerSetter(token,reinterpret_cast<const char*>(original.bytes+bench::MtNodeRecord::Name));
  const Bytes restoreFrame=authenticatedFrame(f,703,restore,false);
  mt::pki::PkiLedger::Status beforeRestore{};assert(f.status(beforeRestore));
  f.receive(restoreFrame,3500);
  mt::pki::PkiLedger::Status stagedRestore{};assert(f.status(stagedRestore));
  assert(stagedRestore.received==uint16_t(beforeRestore.received+1)&&
         stagedRestore.sent==beforeRestore.sent&&f.adminHost.saveCalls==1);
  f.poll(3900);
  assert(f.adminHost.saveCalls==2&&f.adminHost.saveWhileReceiving==0&&!f.adminHost.saveSawPaused);
  mt::pki::PkiLedger::Status afterRestore{};assert(f.status(afterRestore));
  assert(afterRestore.sent==uint16_t(beforeRestore.sent+1));
  const auto restored=f.service.config.node();
  assert(!memcmp(restored.bytes,original.bytes,sizeof(original.bytes)));
  f.poll(4300);
  assert(f.radio.frames.size()==3);
  decodePkiResponse(f,2,703,meshtastic_PortNum_ROUTING_APP,
                    meshtastic_Routing_Error_NONE);
  assert(f.usb.previews==0&&f.ble.previews==0&&f.usb.out.empty()&&f.ble.out.empty());
  bench::wipeKeyBuffer(token,sizeof(token));
}

} // namespace

int main() {
  testReceiveStagePollApplyProofAndRestore();
  puts("PASS_M146_ADMIN_MUTATION_COMPOSITION");
  return 0;
}
