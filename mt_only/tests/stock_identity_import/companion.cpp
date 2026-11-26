// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
#include "monocypher.h"
#include <array>
#include <algorithm>
using Data32=std::array<uint8_t,32>;
struct ImportHost final:Host {
  using Host::Host;
  unsigned calls=0;uint32_t capturedEpoch=0;bool capturedUsb=false,armed=false,authenticated=false,accepted=true;
  Security security{};Data32 testScalar{},testPublic{};
  bool stockIdentityBleLink()const override{return authenticated;}
  Security bluetoothSecurity()const override{return security;}
  int stageStockIdentity(const uint8_t *scalar,const uint8_t *pub,bool isUsb,uint32_t e)override{
    ++calls;capturedEpoch=e;capturedUsb=isUsb;
    assert(!memcmp(scalar,testScalar.data(),32)&&!memcmp(pub,testPublic.data(),32));return armed?0:-EACCES;
  }
  bool acceptTransportFrame(uint32_t)override{return accepted;}
};
class Side{
  Fixture f{81};ImportHost host{f.edit,f.journal};
  mt::companion::CompanionWorkspace workspace;mt::Companion c;
  unsigned owner;uint32_t now=0;
  void input(const meshtastic_ToRadio &m,bool serial){
    host.out.clear();++now;
    uint8_t wire[512];auto out=pb_ostream_from_buffer(wire,sizeof(wire));assert(pb_encode(&out,meshtastic_ToRadio_fields,&m));
    if(!serial)c.packet(wire,out.bytes_written,now,mt::Companion::WorkspaceProof{true});
    else{
      auto byte=[&](uint8_t v){c.byte(v,now,44,mt::Companion::WorkspaceProof{true});};
      byte(0x94);byte(0xc3);byte(static_cast<uint8_t>(out.bytes_written>>8));byte(static_cast<uint8_t>(out.bytes_written));
      for(size_t i=0;i<out.bytes_written;i++)byte(wire[i]);
    }
    crypto_wipe(wire,sizeof(wire));
  }
  int send(const meshtastic_AdminMessage &a,bool serial,bool forceAck=true){
    meshtastic_ToRadio m{};m.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=m.packet;p.id=915+now;p.to=Node;p.want_ack=forceAck;p.priority=meshtastic_MeshPacket_Priority_RELIABLE;
    p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;
    auto w=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));assert(pb_encode(&w,meshtastic_AdminMessage_fields,&a));
    p.decoded.payload.size=w.bytes_written;input(m,serial);return host.out.empty()?-1:host.error();
  }
public:
  explicit Side(unsigned o):c(f.service,host,81,workspace,o),owner(o){
    for(size_t i=0;i<32;i++)host.testScalar[i]=uint8_t(19*i+13);
    crypto_x25519_public_key(host.testPublic.data(),host.testScalar.data());
  }
  void test(){
    const bool serial=owner==0;
    meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=72;
    input(hello,serial);
    meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
    a.set_config.which_payload_variant=meshtastic_Config_security_tag;auto &v=a.set_config.payload_variant.security;
    v.public_key.size=v.private_key.size=32;memcpy(v.public_key.bytes,host.testPublic.data(),32);memcpy(v.private_key.bytes,host.testScalar.data(),32);
    const unsigned bad=meshtastic_Routing_Error_BAD_REQUEST;
    host.security.connected=true;host.security.level=2;
    assert(send(a,serial)==int(bad));
    host.armed=true;
    if(serial){unsigned old=host.calls;assert(send(a,false)==int(bad)&&host.calls==old);}
    if(owner==1){assert(send(a,false)==int(bad));host.security.level=3;assert(send(a,false)==int(bad));host.authenticated=true;}
    const auto stored=f.backend.entries;const auto cfg=f.service.config;
    if(owner<2){
      assert(send(a,serial)==0&&host.capturedUsb==serial);
      if(serial)assert(host.capturedEpoch==44);
      if(!serial){host.security.pending=true;assert(send(a,false)==int(bad));host.security.pending=false;}
      unsigned old=host.calls;
      v.serial_enabled=true;assert(send(a,serial)==int(bad));v.serial_enabled=false;
      v.is_managed=true;assert(send(a,serial)==int(bad));v.is_managed=false;
      v.admin_key_count=1;v.admin_key[0].size=32;memcpy(v.admin_key[0].bytes,host.testPublic.data(),32);
      assert(send(a,serial)==int(bad));v.admin_key_count=0;
      v.public_key.size=0;assert(send(a,serial)==int(bad));v.public_key.size=32;
      assert(f.edit.begin(100)==0);assert(send(a,serial)==int(bad));assert(f.edit.commit(101)==0);
      assert(host.calls==old);
    }else {host.security.level=4;host.authenticated=true;assert(send(a,false)==int(bad)&&host.calls==0);}
    assert(f.service.config.same(cfg)&&f.backend.entries==stored&&f.radio.sends==0);
    for(const auto &frame:host.records)assert(std::search(frame.begin(),frame.end(),host.testScalar.begin(),host.testScalar.end())==frame.end());
  }
};
int main(){for(unsigned owner=0;owner<4;owner++){auto p=std::make_unique<Side>(owner);p->test();}
  puts("PASS stock Android setter over USB and authenticated BLE only; packet injection, weak/mismatched links, TCP/UART, mixed settings and edit batches refuse; output redacted");}
