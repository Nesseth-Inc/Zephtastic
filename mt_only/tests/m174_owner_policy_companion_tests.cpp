// SPDX-License-Identifier: GPL-3.0-only
// M174 stock-Python owner optional-field compatibility; memory-only Companion fixture.
#include "device_config_fixture.h"

static std::vector<uint8_t> pythonOwner(bool hasUnmessagable,bool unmessagable,
                                        unsigned extra=0) {
  // Empty User name plus the proto3 optional User.is_unmessagable field 9.
  std::vector<uint8_t> user;
  if(hasUnmessagable)user.insert(user.end(),{0x48,uint8_t(unmessagable)});
  switch(extra) {
  case 1:user.insert(user.end(),{0x48,0});break; // duplicate optional field
  case 2:user.insert(user.end(),{0x22,0x06,1,0,0,0,0,0});break; // nonzero MAC
  case 3:user.insert(user.end(),{0x98,0x06,0});break; // unknown field
  case 4:user.insert(user.end(),{0x0a,1,'x'});break; // imported node ID
  case 5:user.insert(user.end(),{0x28,1});break; // hardware model
  case 6:user.insert(user.end(),{0x38,1});break; // role
  case 7:user.insert(user.end(),{0x42,1,1});break; // public key
  case 8:user.insert(user.end(),{0x30,1});break; // licensed flag
  default:break;
  }
  std::vector<uint8_t> wire{0x82,0x02,uint8_t(user.size())};
  wire.insert(wire.end(),user.begin(),user.end());return wire;
}

static void rawOwner(Fixture &f,const std::vector<uint8_t> &admin,uint32_t now) {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
  auto &p=r.packet;p.to=Node;p.id=71;p.want_ack=true;
  p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
  assert(admin.size()<=sizeof(p.decoded.payload.bytes));
  memcpy(p.decoded.payload.bytes,admin.data(),admin.size());p.decoded.payload.size=admin.size();
  f.input(r,false,now);
}

int main() {
  auto f=std::make_unique<Fixture>(81);f->provision();
  const auto original=f->service.config.node();
  const auto originalName=std::string(reinterpret_cast<const char *>(original.bytes+bench::MtNodeRecord::Name));
  assert(f->service.route.identity()==Node&&!f->service.config.ownerUnmessagable());

  rawOwner(*f,pythonOwner(true,true),20);
  assert(f->usb.error()==meshtastic_Routing_Error_NONE&&f->service.config.ownerUnmessagable());
  rawOwner(*f,pythonOwner(true,false),21);
  assert(f->usb.error()==meshtastic_Routing_Error_NONE&&!f->service.config.ownerUnmessagable());
  auto current=f->service.config.node();
  assert(!strcmp(reinterpret_cast<const char *>(current.bytes+bench::MtNodeRecord::Name),originalName.c_str())&&
    !memcmp(current.bytes+bench::MtNodeRecord::Key,original.bytes+bench::MtNodeRecord::Key,32)&&
    f->service.route.identity()==Node);
  const auto unchanged=f->service.config;

  for(unsigned extra=1;extra<=8;++extra) {
    rawOwner(*f,pythonOwner(true,false,extra),30+extra);
    assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&f->service.config.same(unchanged));
  }
  std::cout<<"PASS Python optional owner flag false/true and strict nonidentity policy\n";
}
