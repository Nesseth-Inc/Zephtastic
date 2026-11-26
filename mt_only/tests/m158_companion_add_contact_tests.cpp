// SPDX-License-Identifier: GPL-3.0-only
// Local Companion add_contact path using stock SDK bytes and fake I/O.
#include "Companion.h"
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <vector>

namespace {
constexpr uint32_t Local=0x12121212u, Node=0x10203040u;
// Exact stock Meshtastic Python 2.7.11 AdminMessage payload from GOLDEN.json.
constexpr char StockHex[]="92044d08c0e080810112430a09213130323033303430120d4f776e657220636f6e746163741a034f574e2851422009000000000000000000000000000000000000000000000000000000000000002001";
std::vector<uint8_t> fromHex(const char *hex) {
  std::vector<uint8_t> out; for(size_t i=0;hex[i];i+=2) {
    auto nib=[](char c)->uint8_t{return c<='9'?uint8_t(c-'0'):uint8_t((c|32)-'a'+10);};
    out.push_back(uint8_t((nib(hex[i])<<4)|nib(hex[i+1])));
  } return out;
}
struct ContactHost final:mt::Companion::Host {
  std::vector<meshtastic_FromRadio> output;std::vector<mt::OwnerContactCandidate> seen;
  bool isReady=true,isEditing=false;int result=0;
  bool send(const uint8_t *b,size_t n)override {
    assert(b&&n>=4&&b[0]==0x94&&b[1]==0xc3&&size_t(b[2]*256u+b[3])==n-4);
    meshtastic_FromRadio r{};auto in=pb_istream_from_buffer(b+4,n-4);
    assert(pb_decode(&in,meshtastic_FromRadio_fields,&r)&&in.bytes_left==0);output.push_back(r);return true;
  }
  bool ready()const override{return isReady;}bool editing()const override{return isEditing;}
  int save(const mt::Config &)override{return 0;}
  int addOwnerContact(const mt::OwnerContactCandidate &c,uint32_t)override{seen.push_back(c);return result;}
  void clear(){output.clear();seen.clear();}
};
struct ContactRadio final:mt::Radio {
  int state=0;
  bool ready()override{return true;}
  int start()override{state=1;return 0;}
  int restore()override{state=1;return 0;}
  int stop()override{state=0;return 0;}
  int health()override{return state;}
  bool busy()override{return false;}
  uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *,size_t)override{return 0;}
  bool completion(int &)override{return false;}
};
struct ContactFixture {
  ContactRadio radio;mt::Service service{radio,false,Local,127,true,false};ContactHost host;
  mt::companion::CompanionWorkspace workspace;mt::Companion companion{service,host,127,workspace};
  ContactFixture(){mt::Config c;c.bytes[4]=1;auto n=c.node();n.setPublic();c.node(n);
    assert(service.apply(c)&&service.start(false,0)&&service.armEndpoint(false));
    // Existing synthetic cache row lets a successful callback exercise the public NodeInfo emission.
    mt::NodeCache::Entry e;e.identity(Node,0,0);
    assert(mt::NodeCache::Entry::text(e.bytes+12,40,"Before",sizeof("Before")));
    assert(mt::NodeCache::Entry::text(e.bytes+52,5,"OLD",sizeof("OLD")));
    assert(service.nodes.accept(e,Local,0,0)>=0);
  }
  void raw(const uint8_t *b,size_t n,uint32_t id,uint32_t to=Local){
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=r.packet;p.id=id;p.to=to;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
    assert(n<=sizeof(p.decoded.payload.bytes));p.decoded.payload.size=pb_size_t(n);memcpy(p.decoded.payload.bytes,b,n);
    uint8_t encoded[meshtastic_ToRadio_size]{};auto out=pb_ostream_from_buffer(encoded,sizeof(encoded));
    assert(pb_encode(&out,meshtastic_ToRadio_fields,&r));
    companion.packet(encoded,out.bytes_written,id+100,mt::Companion::WorkspaceProof{true});
  }
  void admin(const meshtastic_AdminMessage &a,uint32_t id){uint8_t b[256]{};auto o=pb_ostream_from_buffer(b,sizeof(b));
    assert(pb_encode(&o,meshtastic_AdminMessage_fields,&a));raw(b,o.bytes_written,id);}
};
std::vector<uint8_t> encode(const meshtastic_AdminMessage &a){uint8_t b[256]{};auto o=pb_ostream_from_buffer(b,sizeof(b));
  assert(pb_encode(&o,meshtastic_AdminMessage_fields,&a));return {b,b+o.bytes_written};}
meshtastic_AdminMessage contact(uint32_t node,bool user=true,bool key=true){meshtastic_AdminMessage a{};
  a.which_payload_variant=meshtastic_AdminMessage_add_contact_tag;a.add_contact.node_num=node;a.add_contact.has_user=user;
  if(user){auto &u=a.add_contact.user;snprintf(u.id,sizeof(u.id),"!%08x",unsigned(node));strcpy(u.long_name,"Synthetic contact");strcpy(u.short_name,"SYN");
    if(key){u.public_key.size=32;u.public_key.bytes[0]=9;}a.add_contact.manually_verified=true;}
  return a;
}
unsigned countVariant(const ContactHost &h,pb_size_t variant){unsigned n=0;for(const auto &r:h.output)n+=r.which_payload_variant==variant;return n;}
bool ackIs(const ContactHost &h,uint32_t id,meshtastic_Routing_Error expected){for(const auto &r:h.output){
  if(r.which_payload_variant!=meshtastic_FromRadio_packet_tag||r.packet.decoded.portnum!=meshtastic_PortNum_ROUTING_APP||r.packet.decoded.request_id!=id)continue;
  meshtastic_Routing x{};auto in=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);
  if(pb_decode(&in,meshtastic_Routing_fields,&x)&&x.error_reason==expected)return true;}return false;}
void noCall(ContactFixture &f,const uint8_t *b,size_t n,uint32_t id,uint32_t to=Local){f.host.clear();f.raw(b,n,id,to);assert(f.host.seen.empty());}
}
int main(){
  const auto gold=fromHex(StockHex);assert(gold.size()==80);
  ContactFixture f;
  // Independent stock-serialized positive vector: non-CRC32-bound local destination key is accepted as owner input.
  f.raw(gold.data(),gold.size(),1);assert(f.host.seen.size()==1);const auto &g=f.host.seen.back();
  assert(g.node.id()==Node&&g.hasPublicKey&&g.pkiKeyAdmissible&&g.hasPeerPin&&!g.shouldIgnore&&g.manualVerificationRequested);
  assert(g.node.bytes[mt::NodeCache::Entry::KeyLength]==32&&g.node.bytes[mt::NodeCache::Entry::Key]==9);
  assert(ackIs(f.host,1,meshtastic_Routing_Error_NONE));assert(countVariant(f.host,meshtastic_FromRadio_node_info_tag)==1);

  // A valid keyless phone contact remains a contact candidate, never a fabricated pin.
  auto keyless=encode(contact(Node+1,true,false));f.host.clear();f.raw(keyless.data(),keyless.size(),2);
  assert(f.host.seen.size()==1&&!f.host.seen[0].hasPublicKey&&!f.host.seen[0].hasPeerPin);
  assert(f.host.seen[0].manualVerificationRequested);

  // Reject malformed/truncated, unknown, duplicate oneof, missing user, self, zero id and nonlocal destination.
  noCall(f,gold.data(),gold.size()-1,3);
  auto unknown=gold;unknown.insert(unknown.end(),{0x98,0x06,0x01});noCall(f,unknown.data(),unknown.size(),4);
  auto duplicate=gold;duplicate.insert(duplicate.end(),gold.begin(),gold.end());noCall(f,duplicate.data(),duplicate.size(),5);
  auto missing=encode(contact(Node+2,false,false));noCall(f,missing.data(),missing.size(),6);
  auto self=encode(contact(Local,true,false));noCall(f,self.data(),self.size(),7);
  noCall(f,gold.data(),gold.size(),0);
  noCall(f,gold.data(),gold.size(),8,Node);

  // Availability/edit-state gates run before the owner callback.
  f.host.clear();f.host.isReady=false;f.raw(gold.data(),gold.size(),9);assert(f.host.seen.empty());
  f.host.clear();f.host.isReady=true;f.host.isEditing=true;f.raw(gold.data(),gold.size(),10);assert(f.host.seen.empty());
  f.host.isEditing=false;

  // Host refusals map to explicit routing errors and never emit a cached NodeInfo as success.
  const int hostResults[]={-EINVAL,-EBUSY,-ENOTSUP,-EEXIST};
  const meshtastic_Routing_Error expected[]={meshtastic_Routing_Error_BAD_REQUEST,
    meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED,meshtastic_Routing_Error_BAD_REQUEST,meshtastic_Routing_Error_BAD_REQUEST};
  for(unsigned i=0;i<4;++i){
    f.host.clear();f.host.result=hostResults[i];const uint32_t id=20+i;
    f.raw(gold.data(),gold.size(),id);assert(f.host.seen.size()==1&&ackIs(f.host,id,expected[i]));
    assert(countVariant(f.host,meshtastic_FromRadio_node_info_tag)==0);
  }
  f.host.clear();f.host.result=mt::Companion::Host::ContactPartiallyCommitted;
  f.raw(gold.data(),gold.size(),30);
  assert(f.host.seen.size()==1&&ackIs(f.host,30,meshtastic_Routing_Error_NO_INTERFACE));
  assert(countVariant(f.host,meshtastic_FromRadio_node_info_tag)==0);
  assert(countVariant(f.host,meshtastic_FromRadio_clientNotification_tag)==1);
  bool explicitPartial=false;
  for(const auto &r:f.host.output)if(r.which_payload_variant==meshtastic_FromRadio_clientNotification_tag)
    explicitPartial=r.clientNotification.has_reply_id&&r.clientNotification.reply_id==30&&
      strstr(r.clientNotification.message,"key saved")!=nullptr;
  assert(explicitPartial);
  std::cout<<"PASS M158 public Companion add_contact stock-wire adapter\n";
}
