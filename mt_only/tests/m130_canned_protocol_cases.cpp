// SPDX-License-Identifier: GPL-3.0-only
// Stock canned-list Admin wire contract over Companion; fake radio/storage only.
#include "Companion.h"

#include <assert.h>
#include <string.h>

#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kLocal = 0x12345678u;
constexpr uint32_t kPeer = 0x87654321u;

struct FakeRadio final : mt::Radio {
  unsigned sends=0;
  bool ready() override { return true; }
  int start() override { return 0; }
  int stop() override { return 0; }
  int restore() override { return 0; }
  int health() override { return 1; }
  bool busy() override { return false; }
  uint32_t airtime(size_t) override { return 100; }
  int send(uint8_t *,size_t) override { ++sends; return 0; }
  bool completion(int &) override { return false; }
};

struct FakeHost final : mt::Companion::Host {
  std::vector<meshtastic_FromRadio> output;
  std::string messages="Hi|Bye|Yes|No|Ok";
  unsigned reads=0,save_calls=0,writes=0;
  bool getter_available=true,ready_state=true;

  bool send(const uint8_t *bytes,size_t size) override {
    assert(bytes&&size>=4&&bytes[0]==0x94&&bytes[1]==0xc3);
    assert(size_t(bytes[2]*256u+bytes[3])==size-4);
    meshtastic_FromRadio row{};
    auto stream=pb_istream_from_buffer(bytes+4,size-4);
    assert(pb_decode(&stream,meshtastic_FromRadio_fields,&row)&&stream.bytes_left==0);
    output.push_back(row);
    return true;
  }
  bool ready() const override { return ready_state; }
  int save(const mt::Config &) override { return 0; }
  int cannedMessageMessages(char *out,size_t capacity) const override {
    auto *self=const_cast<FakeHost *>(this);
    ++self->reads;
    if(!getter_available)return -EIO;
    if(!out||capacity<201||messages.size()>200)return -EINVAL;
    memcpy(out,messages.data(),messages.size());out[messages.size()]='\0';
    return 0;
  }
  int saveCannedMessageMessages(const char *value,size_t length,uint32_t) override {
    ++save_calls;
    if(!value||length>200)return -EINVAL;
    if(messages.size()==length&&memcmp(messages.data(),value,length)==0)return 0;
    messages.assign(value,length);++writes;return 0;
  }
};

struct Fixture {
  FakeRadio radio;
  mt::Service service{radio,true,kLocal,127,true,false};
  FakeHost host;
  mt::companion::CompanionWorkspace workspace;mt::Companion companion{service,host,127,workspace};

  Fixture() {
    mt::Config config;config.bytes[4]=1;
    auto node=config.node();node.setPublic();config.node(node);
    assert(service.apply(config));
    assert(service.start(false,0));
    assert(service.armEndpoint(true));
  }

  void sendToRadio(const meshtastic_ToRadio &request,uint32_t now=100) {
    uint8_t bytes[512]{};
    auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
    assert(pb_encode(&stream,meshtastic_ToRadio_fields,&request));
    companion.packet(bytes,stream.bytes_written,now,mt::Companion::WorkspaceProof{true});
  }

  void admin(const meshtastic_AdminMessage &message,uint32_t id,bool wantResponse=true) {
    uint8_t raw[400]{};
    auto stream=pb_ostream_from_buffer(raw,sizeof(raw));
    assert(pb_encode(&stream,meshtastic_AdminMessage_fields,&message));
    adminRaw(raw,stream.bytes_written,id,wantResponse);
  }

  void adminRaw(const uint8_t *raw,size_t size,uint32_t id,bool wantResponse=true) {
    meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &packet=request.packet;packet.id=id;packet.to=kLocal;packet.want_ack=true;
    packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum=meshtastic_PortNum_ADMIN_APP;packet.decoded.want_response=wantResponse;
    assert(raw&&size<=sizeof(packet.decoded.payload.bytes));packet.decoded.payload.size=pb_size_t(size);
    memcpy(packet.decoded.payload.bytes,raw,size);sendToRadio(request,id+100);
  }

  void text(uint32_t id,const char *value,uint32_t now) {
    meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &packet=request.packet;packet.id=id;packet.from=0xabcdef01u;packet.to=kPeer;
    packet.want_ack=true;packet.hop_limit=3;packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
    packet.decoded.payload.size=pb_size_t(strlen(value));
    memcpy(packet.decoded.payload.bytes,value,strlen(value));sendToRadio(request,now);
  }
};

std::vector<uint8_t> stringField36(const std::string &value) {
  const uint32_t key=(36u<<3)|2u;std::vector<uint8_t> out;
  out.push_back(uint8_t(key));out.push_back(uint8_t(key>>8));out.push_back(uint8_t(value.size()));
  out.insert(out.end(),value.begin(),value.end());return out;
}

bool routingAck(const FakeHost &host,uint32_t id,meshtastic_Routing_Error expected) {
  for(const auto &row:host.output) {
    if(row.which_payload_variant!=meshtastic_FromRadio_packet_tag||
       row.packet.decoded.portnum!=meshtastic_PortNum_ROUTING_APP||row.packet.decoded.request_id!=id)continue;
    meshtastic_Routing routing{};auto stream=pb_istream_from_buffer(row.packet.decoded.payload.bytes,
      row.packet.decoded.payload.size);
    if(pb_decode(&stream,meshtastic_Routing_fields,&routing)) {
      if(routing.error_reason==expected)return true;
      std::cerr<<"unexpected routing result id="<<id<<" code="<<unsigned(routing.error_reason)<<" expected="<<unsigned(expected)<<'\n';
    }
  }
  return false;
}

bool queueStatus(const FakeHost &host,uint32_t id,unsigned free,unsigned maxlen) {
  for(const auto &row:host.output) {
    if(row.which_payload_variant==meshtastic_FromRadio_queueStatus_tag&&
       row.queueStatus.mesh_packet_id==id&&row.queueStatus.free==free&&row.queueStatus.maxlen==maxlen)return true;
  }
  return false;
}

bool adminReply(const FakeHost &host,uint32_t id,const std::string &expected) {
  for(const auto &row:host.output) {
    if(row.which_payload_variant!=meshtastic_FromRadio_packet_tag||
       row.packet.decoded.portnum!=meshtastic_PortNum_ADMIN_APP||row.packet.decoded.request_id!=id)continue;
    meshtastic_AdminMessage admin{};auto stream=pb_istream_from_buffer(row.packet.decoded.payload.bytes,
      row.packet.decoded.payload.size);
    if(pb_decode(&stream,meshtastic_AdminMessage_fields,&admin)&&
       admin.which_payload_variant==meshtastic_AdminMessage_get_canned_message_module_messages_response_tag&&
       std::string(admin.get_canned_message_module_messages_response)==expected)return true;
  }
  return false;
}

void group(const char *name) { std::cout<<"PASS "<<name<<'\n'; }

void cannedListCases() {
  Fixture f;
  meshtastic_AdminMessage get{};
  get.which_payload_variant=meshtastic_AdminMessage_get_canned_message_module_messages_request_tag;
  get.get_canned_message_module_messages_request=true;
  f.admin(get,10,false);
  assert(routingAck(f.host,10,meshtastic_Routing_Error_NONE));
  assert(f.host.reads==0&&f.host.writes==0&&!adminReply(f.host,10,f.host.messages));
  f.host.output.clear();f.admin(get,11,true);
  assert(adminReply(f.host,11,"Hi|Bye|Yes|No|Ok"));
  assert(f.host.reads==1&&f.host.writes==0&&f.radio.sends==0);

  meshtastic_AdminMessage set{};
  set.which_payload_variant=meshtastic_AdminMessage_set_canned_message_module_messages_tag;
  f.admin(set,12);
  assert(routingAck(f.host,12,meshtastic_Routing_Error_NONE));
  assert(f.host.save_calls==0&&f.host.writes==0);

  set={};set.which_payload_variant=meshtastic_AdminMessage_set_canned_message_module_messages_tag;
  std::string maximal="A|"+std::string(196,'x')+"\xc3\xa9";
  assert(maximal.size()==200);memcpy(set.set_canned_message_module_messages,maximal.data(),maximal.size());
  set.set_canned_message_module_messages[maximal.size()]='\0';
  f.admin(set,13);assert(routingAck(f.host,13,meshtastic_Routing_Error_NONE));
  assert(f.host.messages==maximal&&f.host.writes==1&&f.radio.sends==0);
  f.admin(set,14);assert(routingAck(f.host,14,meshtastic_Routing_Error_NONE));
  assert(f.host.save_calls==2&&f.host.writes==1); // unchanged SET is a durable no-op.
  get={};get.which_payload_variant=meshtastic_AdminMessage_get_canned_message_module_messages_request_tag;
  get.get_canned_message_module_messages_request=true;f.admin(get,15,true);
  assert(adminReply(f.host,15,maximal));

  const unsigned savesBefore=f.host.save_calls,writesBefore=f.host.writes;
  auto duplicate=stringField36("first");auto second=stringField36("second");
  duplicate.insert(duplicate.end(),second.begin(),second.end());
  f.host.output.clear();f.adminRaw(duplicate.data(),duplicate.size(),16);
  assert(routingAck(f.host,16,meshtastic_Routing_Error_BAD_REQUEST));
  assert(f.host.save_calls==savesBefore&&f.host.writes==writesBefore);

  const uint8_t malformed[]={0xa2,0x02,0x05,'x'};
  f.host.output.clear();f.adminRaw(malformed,sizeof(malformed),17);
  assert(routingAck(f.host,17,meshtastic_Routing_Error_BAD_REQUEST));
  assert(f.host.save_calls==savesBefore&&f.host.writes==writesBefore);

  const std::string invalidUtf8="\xc0\xaf";
  auto invalid=stringField36(invalidUtf8);
  f.host.output.clear();f.adminRaw(invalid.data(),invalid.size(),18);
  assert(routingAck(f.host,18,meshtastic_Routing_Error_BAD_REQUEST));
  assert(f.host.save_calls==savesBefore&&f.host.writes==writesBefore);

  auto overlong=stringField36(std::string(201,'z'));
  f.host.output.clear();f.adminRaw(overlong.data(),overlong.size(),19);
  assert(routingAck(f.host,19,meshtastic_Routing_Error_BAD_REQUEST));
  assert(f.host.save_calls==savesBefore&&f.host.writes==writesBefore&&f.radio.sends==0);
  group("stock canned-list Admin GET/SET preserves UTF-8 pipes and 200-byte bound; silent GET, empty/unchanged no-op, and malformed/noncanonical SETs do not write or send RF");
}

void ordinaryTextQueueCases() {
  Fixture f;
  for(uint32_t id=100;id<104;++id)f.text(id,"Stock app text",id+100);
  assert(f.service.endpointFree()==0&&f.radio.sends==0);
  assert(queueStatus(f.host,100,3,4)&&queueStatus(f.host,103,0,4));
  f.text(104,"Queue full",500);
  assert(routingAck(f.host,104,meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED));
  assert(f.radio.sends==0&&f.host.writes==0);
  assert(f.service.endpointFree()==0); // No poll: the fake radio remains untouched.
  group("stock-shaped client TEXT uses the existing bounded endpoint queue and reports queue-full without any hardware or RF");
}
} // namespace

int main() {
  cannedListCases();
  ordinaryTextQueueCases();
  std::cout<<"PASS M130 canned protocol suite (2 groups)\n";
}
