// SPDX-License-Identifier: GPL-3.0-only
// Synthetic authenticated-ingress pipeline tests. Deterministic keys and
// memory-only journals; no device, RF, or production identity is involved.
#include "pki_runtime_fixture.h"
#include "../src/admin/RemoteAdminProtocol.h"

namespace {
using namespace mt::pki;

struct Provider {
  unsigned calls=0;
  bool deny=false,malformed=false;
  uint32_t expectedLocal=0,expectedFrom=0,expectedId=0;
  uint8_t expectedKey[32]{};
  bool validProvenance=false;
};

bool adminReply(void *opaque,uint32_t local,uint32_t from,const uint8_t key[32],
    uint32_t requestId,const uint8_t *request,size_t requestSize,uint8_t *out,
    size_t capacity,size_t &written) {
  auto &provider=*static_cast<Provider *>(opaque);++provider.calls;written=0;
  provider.validProvenance=local==provider.expectedLocal&&from==provider.expectedFrom&&
    requestId==provider.expectedId&&key&&!memcmp(key,provider.expectedKey,32)&&request&&requestSize;
  if(provider.deny||!provider.validProvenance)return false;
  mt::admin::remote_protocol::Query query{};
  if(!mt::admin::remote_protocol::canonicalGetRequest(request,requestSize,query)||
     query.operation!=mt::admin::remote_protocol::Operation::Owner)return false;

  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_get_owner_response_tag;
  admin.get_owner_response=meshtastic_User_init_zero;
  admin.session_passkey.size=8;
  memset(admin.session_passkey.bytes,0x5a,8);
  uint8_t payload[233]{};
  auto payloadStream=pb_ostream_from_buffer(payload,sizeof(payload));
  if(!pb_encode(&payloadStream,meshtastic_AdminMessage_fields,&admin))return false;

  meshtastic_Data data{};
  data.portnum=meshtastic_PortNum_ADMIN_APP;
  data.has_bitfield=true;
  data.bitfield=0;
  data.request_id=provider.malformed?requestId+1:requestId;
  data.payload.size=payloadStream.bytes_written;
  memcpy(data.payload.bytes,payload,data.payload.size);
  auto dataStream=pb_ostream_from_buffer(out,capacity);
  if(!pb_encode(&dataStream,meshtastic_Data_fields,&data))return false;
  written=dataStream.bytes_written;
  return true;
}

Bytes adminRequest(bool cachedPasskey=false) {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_get_owner_request_tag;
  admin.get_owner_request=true;
  if(cachedPasskey) {
    admin.session_passkey.size=8;
    memset(admin.session_passkey.bytes,0x4c,8);
  }
  uint8_t payload[233]{};
  auto payloadStream=pb_ostream_from_buffer(payload,sizeof(payload));
  assert(pb_encode(&payloadStream,meshtastic_AdminMessage_fields,&admin));
  meshtastic_Data data{};
  data.portnum=meshtastic_PortNum_ADMIN_APP;
  data.want_response=true;
  data.has_bitfield=true;
  data.bitfield=2;
  data.payload.size=payloadStream.bytes_written;
  memcpy(data.payload.bytes,payload,data.payload.size);
  uint8_t wire[227]{};
  auto stream=pb_ostream_from_buffer(wire,sizeof(wire));
  assert(pb_encode(&stream,meshtastic_Data_fields,&data));
  return {wire,wire+stream.bytes_written};
}

Bytes adminSetter() {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;
  admin.set_owner=meshtastic_User_init_zero;
  uint8_t payload[233]{};
  auto payloadStream=pb_ostream_from_buffer(payload,sizeof(payload));
  assert(pb_encode(&payloadStream,meshtastic_AdminMessage_fields,&admin));
  meshtastic_Data data{};
  data.portnum=meshtastic_PortNum_ADMIN_APP;
  data.want_response=true;
  data.has_bitfield=true;
  data.bitfield=2;
  data.payload.size=payloadStream.bytes_written;
  memcpy(data.payload.bytes,payload,data.payload.size);
  uint8_t wire[227]{};
  auto stream=pb_ostream_from_buffer(wire,sizeof(wire));
  assert(pb_encode(&stream,meshtastic_Data_fields,&data));
  return {wire,wire+stream.bytes_written};
}

template <typename F>
Bytes authenticatedFrame(F &f,uint32_t id,const Bytes &plain,bool wantAck=true) {
  uint8_t encrypted[239]{};size_t encryptedSize=0;
  assert(f.crypto.encrypt(f.peer.scalar,f.local.binding.publicKey,f.peer.binding.node,id,
      0x12345678,plain.data(),plain.size(),encrypted,sizeof(encrypted),encryptedSize));
  Bytes frame(encryptedSize+16);
  put(frame.data(),f.local.binding.node);put(frame.data()+4,f.peer.binding.node);put(frame.data()+8,id);
  frame[12]=uint8_t(wantAck?8:0);frame[15]=uint8_t(f.peer.binding.node);
  memcpy(frame.data()+16,encrypted,encryptedSize);
  return frame;
}

// Unlike the shared pki fixture, install the provider before radio start so
// the test exercises the production boot-bound, single-assignment guard.
struct M143Fixture {
  Key local{11},peer{97};Memory im,lm;MonocypherIdentityDeriver deriver;IdentityJournal identity;
  PkiLedger ledger;Gate gate;Entropy entropy;Backend backend;mt::experimental::PkiCrypto crypto{backend};
  LedgerNonceAuthority nonce{identity,ledger,lm,entropy,gate};
  LedgerReplayAuthority replay{identity,ledger,lm,gate};
  PkiRuntime runtime;Radio radio;mt::Service service{radio,true,local.binding.node,81,true,false,&runtime};
  mt::test::KnownChannelFixtureClock knownChannelClock;
  Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion c{service,usb,81,workspace,0},bc{service,ble,81,workspace,1};Provider provider;

  explicit M143Fixture(bool installProvider=true):runtime(crypto,backend,identity,nonce,replay) {
    assert(!identity.load(im,deriver));
    IdentityCandidate candidate(local.binding,local.scalar);
    assert(!identity.store(im,deriver,candidate,true));
    assert(!ledger.load(lm)&&!ledger.initialize(lm,local.binding.publicKey,local.binding.node,true));
    assert(!ledger.addTrustedPeer(lm,peer.binding.node,peer.binding.publicKey,true));
    assert(runtime.open(local.binding.node,peer.binding.node));
    gate.service=&service;
    provider.expectedLocal=local.binding.node;provider.expectedFrom=peer.binding.node;
    memcpy(provider.expectedKey,peer.binding.publicKey,32);
    if(installProvider) {
      assert(service.bindPrivateAdminSnapshot(adminReply,&provider));
      assert(!service.bindPrivateAdminSnapshot(adminReply,&provider));
    }
    knownChannelClock.bind(service);
    mt::Config cfg;cfg.bytes[4]=1;auto node=cfg.node();node.setPublic();cfg.node(node);
    assert(service.apply(cfg)&&service.start(false,0)&&service.armEndpoint(true));
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=1;
    input(c,r,1);input(bc,r,1);usb.out.clear();ble.out.clear();usb.records.clear();ble.records.clear();
  }
  void receive(const Bytes &frame,uint32_t now=1500) {
    service.rx(frame.data(),frame.size(),3,now,81);
    c.receive(frame.data(),frame.size(),-80,3,now,mt::Companion::WorkspaceProof{true});
    bc.receive(frame.data(),frame.size(),-80,3,now,mt::Companion::WorkspaceProof{true});
  }
  void poll(uint32_t now=2000){service.poll(now,71);c.poll(mt::Companion::WorkspaceProof{true});bc.poll(mt::Companion::WorkspaceProof{true});}
};

void group(const char *name){std::cout<<"PASS "<<name<<"\n";}
}

int main() {
  {
    M143Fixture f(false);const auto frame=authenticatedFrame(f,89,adminRequest());
    const auto writes=f.lm.writes;
    f.receive(frame);f.receive(frame,1600);f.poll(2200);
    assert(f.provider.calls==0&&f.lm.writes>writes&&f.radio.sends==0&&
      f.usb.out.empty()&&f.ble.out.empty());
    assert(f.replay.seen(f.peer.binding.node,f.peer.binding.publicKey,89)==ReplayCheck::Seen);
    group("absent provider commits authenticated Admin ingress but emits no event, ACK, or public fallback");
  }
  {
    // Provider-denied authenticated GET commits replay but publishes no event,
    // no response and no generic ACK. The callback's authority is scoped to
    // exact authenticated identity/request arguments.
    M143Fixture f;f.provider.deny=true;f.provider.expectedId=90;
    const auto frame=authenticatedFrame(f,90,adminRequest());
    const auto writes=f.lm.writes;
    f.receive(frame);
    assert(f.provider.calls==1&&f.provider.validProvenance);
    assert(f.lm.writes>writes&&f.radio.sends==0&&f.usb.out.empty()&&f.ble.out.empty());
    assert(f.replay.seen(f.peer.binding.node,f.peer.binding.publicKey,90)==ReplayCheck::Seen);
    f.receive(frame,1600);f.poll(2200);
    assert(f.provider.calls==1&&f.radio.sends==0&&f.usb.out.empty()&&f.ble.out.empty());
    group("denied and duplicate Admin GETs commit once without callback replay, generic ACK, fanout, or public fallback");
  }
  {
    M143Fixture f;f.provider.expectedId=91;
    const auto frame=authenticatedFrame(f,91,adminRequest());
    f.receive(frame);
    assert(f.provider.calls==1&&f.provider.validProvenance&&f.usb.out.empty()&&f.ble.out.empty());
    f.receive(frame,1600);f.poll(2200);
    assert(f.provider.calls==1&&f.radio.sends==1&&f.radio.frames.size()==1);
    const auto &wire=f.radio.frames.front();
    assert(wire.size()>16&&bench::le32(wire.data())==f.peer.binding.node&&
      bench::le32(wire.data()+4)==f.local.binding.node&&wire[13]==0);
    uint8_t response[227]{};size_t responseSize=0;
    assert(f.crypto.decrypt(f.local.scalar,f.peer.binding.publicKey,f.local.binding.node,
      bench::le32(wire.data()+8),wire.data()+16,wire.size()-16,response,sizeof(response),responseSize));
    mt::admin::remote_protocol::Query query{};
    const auto request=adminRequest();
    assert(mt::admin::remote_protocol::canonicalGetRequest(request.data(),request.size(),query));
    assert(mt::admin::remote_protocol::canonicalGetResponse(response,responseSize,query,91));
    assert(f.replay.seen(f.peer.binding.node,f.peer.binding.publicKey,91)==ReplayCheck::Seen);
    group("fresh canonical GET invokes provider once and queues one encrypted correlated same-peer reply");
  }
  {
    M143Fixture f;f.provider.expectedId=96;
    const auto frame=authenticatedFrame(f,96,adminRequest(true));
    f.receive(frame);f.poll(2000);
    assert(f.provider.calls==1&&f.provider.validProvenance&&f.radio.sends==1);
    group("stock cached eight-byte passkey GET remains a fresh authorized request shape");
  }
  {
    M143Fixture f;f.provider.expectedId=92;f.provider.malformed=true;
    const auto frame=authenticatedFrame(f,92,adminRequest());
    f.receive(frame);f.receive(frame,1600);f.poll(2200);
    assert(f.provider.calls==1&&f.radio.sends==0&&f.usb.out.empty()&&f.ble.out.empty());
    group("malformed provider output fails closed without retry, ACK, or client admission");
  }
  {
    M143Fixture f;const auto frame=authenticatedFrame(f,95,adminSetter());
    const auto writes=f.lm.writes;
    f.receive(frame);f.poll(2200);
    assert(f.provider.calls==0&&f.lm.writes==writes&&
      f.replay.seen(f.peer.binding.node,f.peer.binding.publicKey,95)==ReplayCheck::Fresh&&
      f.radio.sends==0&&f.usb.out.empty()&&f.ble.out.empty());
    group("authenticated Admin setter is rejected before durable replay commit and provider callback");
  }
  {
    M143Fixture f;const auto plain=data("non-admin");
    const auto frame=authenticatedFrame(f,93,plain,false);
    f.receive(frame);
    assert(f.provider.calls==0&&f.usb.previews==1&&f.ble.previews==1);
    assert(f.usb.previewPort==meshtastic_PortNum_TEXT_MESSAGE_APP&&
      f.usb.previewFrom==f.peer.binding.node&&f.usb.previewTo==f.local.binding.node);
    group("existing authenticated non-Admin private text fanout remains unchanged");
  }
  {
    M143Fixture f;const auto request=adminRequest();
    const auto entropyCalls=f.entropy.calls;const auto writes=f.lm.writes;
    assert(f.service.submitPrivate(mt::Endpoint::LocalUiOwner,f.peer.binding.node,94,
      request.data(),request.size(),3,false,nullptr,0,false,1500)==-EINVAL);
    assert(f.entropy.calls==entropyCalls&&f.lm.writes==writes&&f.provider.calls==0);
    group("caller/local-UI Admin sends remain rejected before nonce, replay, provider, or storage admission");
  }
  return 0;
}
