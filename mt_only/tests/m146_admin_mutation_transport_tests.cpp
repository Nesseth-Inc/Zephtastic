// Synthetic two-peer authenticated Admin mutation transport tests. These use
// deterministic test identities, in-memory ledgers, PSA host crypto and an
// inert radio; they do not exercise hardware or production credentials.
#include "pki_runtime_fixture.h"
#include "../src/admin/RemoteAdminMutationProtocol.h"
#include "../experimental/pki/PkiAck.h"

namespace {
using namespace mt::pki;

Bytes mutationRequest() {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;
  admin.set_owner=meshtastic_User_init_zero;
  admin.session_passkey.size=8;
  memset(admin.session_passkey.bytes,0x41,8);
  uint8_t payload[233]{};
  auto ps=pb_ostream_from_buffer(payload,sizeof(payload));
  assert(pb_encode(&ps,meshtastic_AdminMessage_fields,&admin));
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_ADMIN_APP;
  data.want_response=true;data.has_bitfield=true;data.bitfield=2;
  data.payload.size=ps.bytes_written;memcpy(data.payload.bytes,payload,data.payload.size);
  uint8_t wire[227]{};auto ds=pb_ostream_from_buffer(wire,sizeof(wire));
  assert(pb_encode(&ds,meshtastic_Data_fields,&data));
  return {wire,wire+ds.bytes_written};
}

struct M146Fixture;
struct MutationOwner {
  M146Fixture *fixture=nullptr;
  Bytes expected;
  uint32_t expectedRequest=0,routingError=0;
  unsigned calls=0;
  bool accept=true,restartService=false,borrowedDataIntact=false;
};
struct M146Fixture {
  Key local{11},peer{97};Memory im,lm;MonocypherIdentityDeriver deriver;
  IdentityJournal identity;PkiLedger ledger;Gate gate;Entropy entropy;Backend backend;
  mt::experimental::PkiCrypto crypto{backend};
  LedgerNonceAuthority nonce{identity,ledger,lm,entropy,gate};
  LedgerReplayAuthority replay{identity,ledger,lm,gate};
  PkiRuntime runtime;Radio radio;
  mt::Service service{radio,true,local.binding.node,81,true,false,&runtime};
  mt::test::KnownChannelFixtureClock knownChannelClock;
  MutationOwner owner;

  M146Fixture():runtime(crypto,backend,identity,nonce,replay) {
    assert(!identity.load(im,deriver));
    IdentityCandidate candidate(local.binding,local.scalar);
    assert(!identity.store(im,deriver,candidate,true));
    assert(!ledger.load(lm)&&!ledger.initialize(lm,local.binding.publicKey,local.binding.node,true));
    assert(!ledger.addTrustedPeer(lm,peer.binding.node,peer.binding.publicKey,true));
    assert(runtime.open(local.binding.node,peer.binding.node));gate.service=&service;
    knownChannelClock.bind(service);
    owner.fixture=this;
    assert(service.bindPrivateAdminMutation(applyMutation,&owner));
    assert(!service.bindPrivateAdminMutation(applyMutation,&owner));
    mt::Config cfg;cfg.bytes[4]=1;auto node=cfg.node();node.setPublic();cfg.node(node);
    assert(service.apply(cfg)&&service.start(false,0)&&service.armEndpoint(true));
    lm.onWrite=[this]{assert(service.privateStoragePaused()&&!radio.healthValue&&!service.active());};
  }
  static bool applyMutation(void *opaque,uint32_t local,uint32_t from,const uint8_t key[32],
      uint32_t request,const uint8_t *data,size_t size,uint32_t now,uint32_t &routingError) {
    auto &o=*static_cast<MutationOwner *>(opaque);auto &f=*o.fixture;++o.calls;
    assert(local==f.local.binding.node&&from==f.peer.binding.node&&request==o.expectedRequest);
    assert(key&&!memcmp(key,f.peer.binding.publicKey,32));
    assert(data&&size==o.expected.size()&&!memcmp(data,o.expected.data(),size));
    assert(f.service.receiving()&&!f.service.privateStoragePaused()&&f.radio.health()==1);
    assert(f.service.privateAdminPeerCurrent(from,key));
    mt::admin::mutation_protocol::Mutation parsed{};
    o.borrowedDataIntact=mt::admin::mutation_protocol::canonicalSetRequest(data,size,parsed)&&
        parsed.operation==mt::admin::mutation_protocol::Operation::Owner;
    if(o.restartService) {
      f.service.stop();
      assert(!f.service.receiving());
      assert(f.service.start(false,now+1));
      assert(f.service.armEndpoint(true));
      assert(data&&size==o.expected.size()&&!memcmp(data,o.expected.data(),size));
    }
    routingError=o.routingError;return o.accept;
  }
  void poll(uint32_t now,uint32_t random=71){service.poll(now,random);}
};

Bytes authenticatedMutation(M146Fixture &f,uint32_t id,const Bytes &plain,
                            bool reliable=true) {
  uint8_t encrypted[239]{};size_t encryptedSize=0;
  assert(f.crypto.encrypt(f.peer.scalar,f.local.binding.publicKey,
      f.peer.binding.node,id,0x12345678,plain.data(),plain.size(),
      encrypted,sizeof(encrypted),encryptedSize));
  Bytes frame(encryptedSize+16);
  put(frame.data(),f.local.binding.node);put(frame.data()+4,f.peer.binding.node);
  put(frame.data()+8,id);frame[12]=uint8_t(2|(5u<<5)|(reliable?8:0));
  frame[15]=uint8_t(f.peer.binding.node);
  memcpy(frame.data()+16,encrypted,encryptedSize);return frame;
}

void verifyPrivateRoutingResponse(M146Fixture &f,uint32_t request,uint32_t expectedError,
                                 uint8_t expectedHop,bool reliable=true) {
  assert(f.radio.frames.size()==1);
  const auto &frame=f.radio.frames.front();
  const uint32_t txId=bench::le32(frame.data()+8);
  assert(txId&&bench::le32(frame.data())==f.peer.binding.node&&
      bench::le32(frame.data()+4)==f.local.binding.node);
  assert((frame[12]&7)==expectedHop&&((frame[12]>>5)&7)==expectedHop&&
      bool(frame[12]&8)==reliable);
  assert(f.ledger.sent(txId));
  uint8_t plain[227]{};size_t plainSize=0;
  assert(f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,f.local.binding.node,
      txId,frame.data()+16,frame.size()-16,plain,sizeof(plain),plainSize));
  meshtastic_Data data{};auto ds=pb_istream_from_buffer(plain,plainSize);
  assert(pb_decode(&ds,meshtastic_Data_fields,&data)&&!ds.bytes_left);
  assert(data.portnum==meshtastic_PortNum_ROUTING_APP&&data.request_id==request&&
      data.has_bitfield&&!data.bitfield&&!data.want_response);
  uint8_t routing[233]{};size_t routingSize=0;
  mt::experimental::PkiAck proof(f.crypto);
  const mt::experimental::AckEnvelope received{true,f.local.binding.node,
      f.peer.binding.node,request};
  const mt::experimental::PendingAck pending{true,f.local.binding.node,
      f.peer.binding.node,request};
  assert(proof.verify(f.peer.scalar,f.local.binding.publicKey,pending,received,
      data.payload.bytes,data.payload.size,routing,sizeof(routing),routingSize)==
      mt::experimental::AckProofVerdict::ValidProof);
  meshtastic_Routing result{};auto rs=pb_istream_from_buffer(routing,routingSize);
  assert(pb_decode(&rs,meshtastic_Routing_fields,&result)&&!rs.bytes_left&&
      result.error_reason==static_cast<meshtastic_Routing_Error>(expectedError));
}
void group(const char *name){std::cout<<"PASS "<<name<<"\n";}
}

int main() {
  const Bytes request=mutationRequest();
  mt::admin::mutation_protocol::Mutation parsed{};
  assert(mt::admin::mutation_protocol::canonicalSetRequest(request.data(),request.size(),parsed));
  assert(parsed.operation==mt::admin::mutation_protocol::Operation::Owner);
  const uint8_t responseHop=uint8_t(mt::HopPolicy::response(5,2,3));
  {
    M146Fixture f;f.owner.restartService=true;f.owner.expected=request;f.owner.expectedRequest=701;
    const unsigned entropyBefore=f.entropy.calls;
    const auto frame=authenticatedMutation(f,701,request);
    f.service.rx(frame.data(),frame.size(),3,1500,81);
    mt::pki::PrivateEvent event{};
    assert(!f.service.takePrivateEvent(0,frame.data(),frame.size(),event));
    assert(f.owner.calls==0&&f.radio.sends==0&&
       f.replay.seen(f.peer.binding.node,f.peer.binding.publicKey,701)==ReplayCheck::Seen);
    f.service.rx(frame.data(),frame.size(),3,1600,82); // Seen never stages twice.
    assert(f.owner.calls==0);
    f.poll(2000);
    assert(f.owner.calls==1&&f.owner.borrowedDataIntact&&f.radio.sends==0&&
       f.entropy.calls==entropyBefore+1);
    f.poll(2600);
    assert(f.radio.sends==1&&f.owner.calls==1);
    verifyPrivateRoutingResponse(f,701,meshtastic_Routing_Error_NONE,responseHop);
    group("Fresh canonical SET defers callback until restored RX, survives setter stop/restart, and returns one stock-shaped encrypted proved Routing ACK with one nonce reservation");
  }
  {
    M146Fixture f;f.owner.accept=false;f.owner.expectedRequest=702;f.owner.expected=request;
    const auto frame=authenticatedMutation(f,702,request);f.service.rx(frame.data(),frame.size(),3,1500,81);
    assert(f.owner.calls==0&&f.radio.sends==0);
    f.poll(2000);f.poll(2600);
    assert(f.owner.calls==1&&f.radio.sends==0&&f.service.endpointControls()==0);
    group("callback refusal consumes once, wipes the request and emits no ACK");
  }
  {
    M146Fixture f;f.owner.expectedRequest=703;f.owner.expected=request;
    f.owner.routingError=meshtastic_Routing_Error_BAD_REQUEST;
    const auto frame=authenticatedMutation(f,703,request);f.service.rx(frame.data(),frame.size(),3,1500,81);
    f.poll(2000);f.poll(2600);
    assert(f.owner.calls==1&&f.radio.sends==1);
    verifyPrivateRoutingResponse(f,703,meshtastic_Routing_Error_BAD_REQUEST,responseHop);
    group("callback Routing error is correlated and returned in an encrypted proved Routing result");
  }
  {
    M146Fixture f;f.owner.expected=request;
    Bytes malformed;
    // The body is valid, but remove the required session passkey by using the
    // canonical legacy setter fixture; invalid Admin never reaches staging.
    meshtastic_AdminMessage admin{};admin.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;
    uint8_t payload[233]{};auto ps=pb_ostream_from_buffer(payload,sizeof(payload));
    assert(pb_encode(&ps,meshtastic_AdminMessage_fields,&admin));
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_ADMIN_APP;data.want_response=true;
    data.has_bitfield=true;data.bitfield=2;data.payload.size=ps.bytes_written;
    memcpy(data.payload.bytes,payload,data.payload.size);uint8_t raw[227]{};
    auto ds=pb_ostream_from_buffer(raw,sizeof(raw));assert(pb_encode(&ds,meshtastic_Data_fields,&data));
    malformed.assign(raw,raw+ds.bytes_written);
    const auto frame=authenticatedMutation(f,704,malformed);f.service.rx(frame.data(),frame.size(),3,1500,81);
    f.poll(2000);f.poll(2600);
    assert(f.owner.calls==0&&f.radio.sends==0);
    group("canonical classifier rejects malformed passkey shape before deferred setter and reply");
  }
  {
    M146Fixture f;f.owner.expected=request;f.owner.expectedRequest=705;
    f.service.armEndpoint(false);
    const auto frame=authenticatedMutation(f,705,request);f.service.rx(frame.data(),frame.size(),3,1500,81);
    f.poll(2000);
    assert(f.owner.calls==0&&f.radio.sends==0);
    group("unarmed endpoint commits no deferred mutation callback or response");
  }
}
