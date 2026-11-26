// SPDX-License-Identifier: GPL-3.0-only
// Companion and Service boundary tests with a synthetic PrivateRuntime.
// The fixture proves local admission/correlation and output routing only;
// it does not claim PKI cryptography, replay persistence, or RF behavior.
#define CONFIG_MT_RF_ADMIN_GET 1
#define main m143_admin_snapshot_unused_main
#include "m143_admin_snapshot_tests.cpp"
#undef main

#include <algorithm>
#include <cerrno>
#include <vector>

namespace {
using mt::admin::remote_protocol::MaxDataBytes;
using mt::admin::remote_protocol::Query;

Wire response(const Query &query,uint32_t request) {
  assert(query.operation==mt::admin::remote_protocol::Operation::Owner&&query.selector==0);
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_get_owner_response_tag;
  admin.get_owner_response=meshtastic_User_init_zero;
  admin.session_passkey.size=8;memset(admin.session_passkey.bytes,0x5a,8);
  uint8_t payload[233]{};auto out=pb_ostream_from_buffer(payload,sizeof(payload));
  assert(pb_encode(&out,meshtastic_AdminMessage_fields,&admin));
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_ADMIN_APP;
  data.request_id=request;data.has_bitfield=true;data.payload.size=out.bytes_written;
  memcpy(data.payload.bytes,payload,data.payload.size);
  uint8_t encoded[MaxDataBytes]{};out=pb_ostream_from_buffer(encoded,sizeof(encoded));
  assert(pb_encode(&out,meshtastic_Data_fields,&data));
  return Wire(encoded,encoded+out.bytes_written);
}

struct ClientHost final : mt::Companion::Host {
  std::vector<meshtastic_FromRadio> output;
  unsigned previews=0,peripheralPackets=0,adminKeyReads=0,passkeyCalls=0;
  bool send(const uint8_t *bytes,size_t size) override {
    if(!bytes||size<4||bytes[0]!=0x94||bytes[1]!=0xc3||
       size_t(bytes[2]*256u+bytes[3])!=size-4)return false;
    meshtastic_FromRadio record{};
    auto in=pb_istream_from_buffer(bytes+4,size-4);
    if(!pb_decode(&in,meshtastic_FromRadio_fields,&record)||in.bytes_left)return false;
    output.push_back(record);return true;
  }
  int save(const mt::Config &) override { return -ENOTSUP; }
  bool ready() const override { return true; }
  bool adminKeys(meshtastic_Config_SecurityConfig &) const override {
    ++const_cast<ClientHost *>(this)->adminKeyReads;return false;
  }
  bool adminPasskey(uint8_t *,size_t) override { ++passkeyCalls;return false; }
  void preview(uint32_t,uint32_t,uint32_t,unsigned,const uint8_t *,size_t,
               int16_t,int8_t,uint32_t,bool) override { ++previews; }
  void peripheralPacket(uint32_t,uint32_t,uint32_t,unsigned,unsigned,
                        const uint8_t *,size_t,bool) override { ++peripheralPackets; }
  size_t adminReplies() const {
    size_t count=0;
    for(const auto &record:output) if(record.which_payload_variant==meshtastic_FromRadio_packet_tag&&
        record.packet.decoded.portnum==meshtastic_PortNum_ADMIN_APP&&record.packet.pki_encrypted)
      ++count;
    return count;
  }
};

struct ClientRadio final : mt::Radio {
  int state=0;
  bool ready() override { return true; }
  int start() override { state=1;return 0; }
  int stop() override { state=0;return 0; }
  int restore() override { state=1;return 0; }
  int stopForStorage() override { state=0;return 0; }
  int health() override { return state; }
  bool busy() override { return false; }
  uint32_t airtime(size_t) override { return 100; }
  int send(uint8_t *,size_t) override { return -ENOTSUP; }
  bool completion(int &) override { return false; }
};

struct FakePrivateRuntime final : mt::pki::PrivateRuntime {
  mt::Service *service=nullptr;
  uint32_t local=0,peerNode=0;
  uint8_t publicKey[32]{};
  bool readyFlag=true,pinned=true;
  int submitResult=0;
  unsigned submits=0,receiveCalls=0,eventCalls=0;
  uint8_t submittedOwners[8]{};
  uint32_t submittedIds[8]{};
  Wire responseWire;
  uint32_t responsePacketId=0;
  mt::pki::PrivateEvent pending{};
  uint8_t pendingFrame[255]{};size_t pendingFrameSize=0;
  bool pendingValid=false,delivered=false;

  FakePrivateRuntime(uint32_t localNode,uint32_t peerNode,const uint8_t key[32]):
    local(localNode),peerNode(peerNode) { memcpy(publicKey,key,32); }
  uint32_t peer() const override { return peerNode; }
  bool ready(uint32_t node) const override { return readyFlag&&node==local; }
  bool publicIdentity(uint8_t out[32]) const override { memcpy(out,publicKey,32);return true; }
  bool peerPublicIdentity(uint32_t node,uint8_t out[32]) const override {
    if(!pinned||node!=peerNode)return false;memcpy(out,publicKey,32);return true;
  }
  bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,uint32_t,uint32_t,
                 bool,uint32_t,bench::Frame &) override { return false; }
  void freeze() override { readyFlag=false;clearEvent(); }
  bool matchesPeer(uint32_t node) const override { return pinned&&node==peerNode; }
  bool revoke(uint32_t node) override { if(node!=peerNode)return false;pinned=false;return true; }
  void clearEvent() override {
    pendingValid=false;delivered=false;
    bench::wipeKeyBuffer(&pending,sizeof(pending));
    bench::wipeKeyBuffer(pendingFrame,sizeof(pendingFrame));pendingFrameSize=0;
  }
  void sweep(mt::Endpoint &) override {}
  int submit(mt::Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,
             bool,const uint8_t *,size_t,bool,uint32_t) override { return -ENOTSUP; }
  int submitAdminGet(mt::Endpoint &,uint8_t owner,uint32_t,uint32_t id,
      const uint8_t *data,size_t size,unsigned,bool,const uint8_t *key,size_t keySize,
      bool explicitPki,uint32_t) override {
    assert(service&&service->privateStoragePaused());
    assert(data&&size&&key&&keySize==32&&explicitPki);
    if(submits<8){submittedOwners[submits]=owner;submittedIds[submits]=id;}
    ++submits;return submitResult;
  }
  void receive(mt::Endpoint &,bench::MtCodec &,const uint8_t *frame,size_t size,
               uint32_t now,uint32_t,bool) override {
    ++receiveCalls;clearEvent();
    assert(service&&service->privateStoragePaused()&&frame&&size>=29&&size<=255);
    if(responseWire.empty()||!responsePacketId)return;
    const uint32_t owners=service->privateAdminResponseOwners(local,peerNode,publicKey,
        responsePacketId,responseWire.data(),responseWire.size(),now,false);
    if(!owners)return;
    // Test seam representing the real runtime's durable Fresh replay commit.
    const uint32_t committedOwners=service->privateAdminResponseOwners(local,peerNode,
        publicKey,responsePacketId,responseWire.data(),responseWire.size(),now,true);
    if(!committedOwners)return;
    pending.from=peerNode;pending.to=local;pending.id=responsePacketId;
    memcpy(pending.publicKey,publicKey,32);pending.size=responseWire.size();
    memcpy(pending.data,responseWire.data(),responseWire.size());
    pending.adminOwners=uint8_t(committedOwners);
    memcpy(pendingFrame,frame,size);pendingFrameSize=size;pendingValid=true;
  }
  void proof(mt::Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t) override {}
  bool event(uint8_t owner,const uint8_t *frame,size_t size,mt::pki::PrivateEvent &out) override {
    ++eventCalls;
    if(!pendingValid||owner>=mt::LocalClientCount||
       !(pending.adminOwners&(1u<<owner))||delivered||!frame||size!=pendingFrameSize||
       memcmp(frame,pendingFrame,size))return false;
    delivered=true;out=pending;return true;
  }
};

void connectClient(mt::Companion &client,uint32_t nonce) {
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
  request.want_config_id=nonce;input(client,request,1);
}

struct ClientFixture {
  Key local{11},peer{97};ClientRadio radio;FakePrivateRuntime runtime{local.binding.node,
      peer.binding.node,peer.binding.publicKey};
  mt::Service service{radio,true,local.binding.node,81,true,false,&runtime};
  ClientHost host0,host1;mt::companion::CompanionWorkspace workspace;mt::Companion client0{service,host0,81,workspace,0},client1{service,host1,81,workspace,1};
  ClientFixture() {
    runtime.service=&service;
    mt::Config config;config.bytes[4]=1;auto node=config.node();node.setPublic();config.node(node);
    assert(service.apply(config)&&service.start(false,0)&&service.armEndpoint(true));
    connectClient(client0,1);connectClient(client1,2);
    host0.output.clear();host1.output.clear();
    // Config streaming during connection reads local Admin-key policy. Begin
    // operation counters after that handshake, before any client GET.
    host0.adminKeyReads=host1.adminKeyReads=0;
    host0.passkeyCalls=host1.passkeyCalls=0;
    host0.previews=host1.previews=0;
    host0.peripheralPackets=host1.peripheralPackets=0;
  }
  void send(mt::Companion &client,meshtastic_MeshPacket packet) {
    meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;
    request.packet=packet;input(client,request,10);
  }
  meshtastic_MeshPacket request(uint32_t id,const Wire &wire) const {
    meshtastic_MeshPacket packet{};packet.id=id;packet.to=peer.binding.node;
    packet.want_ack=true;packet.pki_encrypted=true;packet.public_key.size=32;
    memcpy(packet.public_key.bytes,peer.binding.publicKey,32);
    packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    auto in=pb_istream_from_buffer(wire.data(),wire.size());
    assert(pb_decode(&in,meshtastic_Data_fields,&packet.decoded)&&!in.bytes_left);
    return packet;
  }
  uint32_t owners(const Wire &wire,uint32_t outerId=900,uint32_t now=30) {
    assert(service.beginPrivateAdministration());
    const uint32_t found=service.privateAdminResponseOwners(local.binding.node,
        peer.binding.node,peer.binding.publicKey,outerId,wire.data(),wire.size(),now,false);
    assert(service.endPrivateAdministration());return found;
  }
  void incoming(const Wire &plain,uint32_t outerId=900,uint32_t now=40) {
    runtime.responseWire=plain;runtime.responsePacketId=outerId;
    uint8_t frame[29]{};put(frame,local.binding.node);put(frame+4,peer.binding.node);
    put(frame+8,outerId);frame[12]=0;frame[13]=0;frame[15]=uint8_t(peer.binding.node);
    service.rx(frame,sizeof(frame),-80,now,71,true);
    client0.receive(frame,sizeof(frame),-80,3,now,now,false,0,true,mt::Companion::WorkspaceProof{true});
    client1.receive(frame,sizeof(frame),-80,3,now,now,false,0,true,mt::Companion::WorkspaceProof{true});
  }
};

void remoteGetUsesOnlyDedicatedPrivateLane() {
  ClientFixture f;const Wire wire=getRequest(ownerGet());
  f.send(f.client0,f.request(100,wire));
  assert(f.runtime.submits==1&&f.runtime.submittedOwners[0]==0&&f.runtime.submittedIds[0]==100);
  assert(f.owners(response(mt::admin::remote_protocol::Query{mt::admin::remote_protocol::Operation::Owner,0},100))==1u);
  assert(f.host0.previews==0&&f.host0.peripheralPackets==0&&f.host0.adminKeyReads==0&&f.host0.passkeyCalls==0);
}

void remoteMutationResponsePublicAndForgedInputsAreRejected() {
  ClientFixture f;
  meshtastic_AdminMessage setter{};setter.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;
  strcpy(setter.set_owner.long_name,"not allowed");
  const Wire setWire=getRequest(setter);f.send(f.client0,f.request(110,setWire));
  meshtastic_Data responseData{};responseData.portnum=meshtastic_PortNum_ADMIN_APP;
  responseData.request_id=109;responseData.has_bitfield=true;
  responseData.payload.size=1;responseData.payload.bytes[0]=0x01;
  uint8_t responseBytes[MaxDataBytes]{};auto out=pb_ostream_from_buffer(responseBytes,sizeof(responseBytes));
  assert(pb_encode(&out,meshtastic_Data_fields,&responseData));
  const Wire responseInput(responseBytes,responseBytes+out.bytes_written);
  f.send(f.client0,f.request(111,responseInput));
  f.send(f.client0,f.request(112,getRequest(ownerGet()))); // accepted positive control
  auto publicPacket=f.request(113,getRequest(ownerGet()));publicPacket.pki_encrypted=false;
  f.send(f.client0,publicPacket);
  auto wrongKey=f.request(114,getRequest(ownerGet()));memset(wrongKey.public_key.bytes,0xA7,32);
  f.send(f.client0,wrongKey);
  auto signedPacket=f.request(115,getRequest(ownerGet()));signedPacket.xeddsa_signed=true;
  f.send(f.client0,signedPacket);
  auto nonzeroChannel=f.request(116,getRequest(ownerGet()));nonzeroChannel.channel=1;
  f.send(f.client0,nonzeroChannel);
  assert(f.runtime.submits==1&&f.runtime.submittedIds[0]==112);
  assert(f.owners(response(mt::admin::remote_protocol::Query{mt::admin::remote_protocol::Operation::Owner,0},110))==0);
  assert(f.owners(response(mt::admin::remote_protocol::Query{mt::admin::remote_protocol::Operation::Owner,0},112))==1u);
}

void deniedFullAndFailedEnqueuesNeverInstallCorrelation() {
  {
    ClientFixture f;f.runtime.pinned=false;
    f.send(f.client0,f.request(120,getRequest(ownerGet())));
    assert(f.runtime.submits==0&&f.owners(response(Query{mt::admin::remote_protocol::Operation::Owner,0},120))==0);
  }
  {
    ClientFixture f;f.runtime.submitResult=-EIO;
    f.send(f.client0,f.request(121,getRequest(ownerGet())));
    assert(f.runtime.submits==1&&f.owners(response(Query{mt::admin::remote_protocol::Operation::Owner,0},121))==0);
  }
  {
    ClientFixture f;const Wire requestWire=getRequest(ownerGet());
    for(uint32_t id=130;id<134;++id)f.send(f.client0,f.request(id,requestWire));
    assert(f.runtime.submits==4&&f.owners(response(Query{mt::admin::remote_protocol::Operation::Owner,0},134))==0);
    f.send(f.client0,f.request(134,requestWire));
    assert(f.runtime.submits==4&&f.owners(response(Query{mt::admin::remote_protocol::Operation::Owner,0},134))==0);
  }
}

void matchedResponseGoesOnlyToRequestingCompanion() {
  ClientFixture f;const Wire requestWire=getRequest(ownerGet());
  f.send(f.client0,f.request(140,requestWire));
  const Query query{mt::admin::remote_protocol::Operation::Owner,0};
  f.incoming(response(query,140),940,50);
  assert(f.host0.adminReplies()==1&&f.host1.adminReplies()==0);
  assert(f.host0.previews==0&&f.host1.previews==0&&
         f.host0.peripheralPackets==0&&f.host1.peripheralPackets==0);
  assert(f.host0.adminKeyReads==0&&f.host0.passkeyCalls==0);
}

void ownerDisconnectCancelsLateResponse() {
  ClientFixture f;const Wire requestWire=getRequest(ownerGet());
  f.send(f.client0,f.request(150,requestWire));
  meshtastic_ToRadio disconnect{};disconnect.which_payload_variant=meshtastic_ToRadio_disconnect_tag;
  input(f.client0,disconnect,20);
  const Query query{mt::admin::remote_protocol::Operation::Owner,0};
  assert(f.owners(response(query,150),950)==0);
  f.incoming(response(query,150),950,60);
  assert(f.host0.adminReplies()==0&&f.host1.adminReplies()==0);
  assert(f.host0.previews==0&&f.host0.peripheralPackets==0);
}
} // namespace

int main() {
  remoteGetUsesOnlyDedicatedPrivateLane();
  remoteMutationResponsePublicAndForgedInputsAreRejected();
  deniedFullAndFailedEnqueuesNeverInstallCorrelation();
  matchedResponseGoesOnlyToRequestingCompanion();
  ownerDisconnectCancelsLateResponse();
  std::cout << "PASS integrated synthetic Service/Companion private Admin GET routing and correlation boundaries; no PKI/RF claim\n";
}
