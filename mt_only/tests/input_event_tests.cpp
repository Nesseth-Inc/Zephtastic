// SPDX-License-Identifier: GPL-3.0-only
// M19 local stock send_input_event admission. Memory only: no device/display,
// transport, storage, credential or radio API is opened.
#include "device_config_fixture.h"

static meshtastic_AdminMessage input(unsigned code,uint32_t key=0,uint32_t x=0,uint32_t y=0) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_send_input_event_tag;
  a.send_input_event.event_code=uint8_t(code);a.send_input_event.kb_char=uint8_t(key);
  a.send_input_event.touch_x=uint16_t(x);a.send_input_event.touch_y=uint16_t(y);return a;
}
static std::vector<uint8_t> adminBytes(const meshtastic_AdminMessage &a) {
  uint8_t bytes[512]{};auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
  assert(pb_encode(&stream,meshtastic_AdminMessage_fields,&a));return {bytes,bytes+stream.bytes_written};
}
static void rawAdmin(Fixture &f,const std::vector<uint8_t> &wire,bool ble=false,uint32_t now=1) {
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=request.packet;
  p.to=Node;p.id=0x19190001;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
  assert(wire.size()<=sizeof(p.decoded.payload.bytes));memcpy(p.decoded.payload.bytes,wire.data(),wire.size());p.decoded.payload.size=wire.size();
  f.input(request,ble,now);
}
static bool supported(unsigned code) {
  return code==10||code==17||code==18||code==19||code==20||code==27||code==28;
}
struct State {
  mt::Config config;unsigned writes,saves,starts,stops,sends,free;bool armed,receiving;
};
static State state(const Fixture &f) {
  return {f.service.config,f.backend.writes,f.usb.saves+f.ble.saves,f.radio.starts,f.radio.stops,f.radio.sends,
          f.service.endpointFree(),f.service.endpointArmed(),f.service.receiving()};
}
static void same(const Fixture &f,const State &s) {
  assert(f.service.config.same(s.config)&&f.backend.writes==s.writes&&f.usb.saves+f.ble.saves==s.saves&&
    f.radio.starts==s.starts&&f.radio.stops==s.stops&&f.radio.sends==s.sends&&
    f.service.endpointFree()==s.free&&f.service.endpointArmed()==s.armed&&f.service.receiving()==s.receiving);
}
static bool notice(const Host &host,const char *text) {
  for(const auto &record:host.out)
    if(record.which_payload_variant==meshtastic_FromRadio_clientNotification_tag&&
       record.clientNotification.has_reply_id&&strstr(record.clientNotification.message,text))return true;
  return false;
}

int main() {
  for(bool ble:{false,true}) {
    auto f=std::make_unique<Fixture>(127);auto &host=ble?f->ble:f->usb;host.inputResult=0;const auto before=state(*f);
    unsigned accepted=0;
    for(unsigned code=0;code<256;++code) {
      host.out.clear();host.uiInputs.clear();f->admin(input(code),ble,1000+code);
      assert(host.error()==(supported(code)?meshtastic_Routing_Error_NONE:meshtastic_Routing_Error_BAD_REQUEST));
      assert(host.uiInputs.size()==(supported(code)?1u:0u));
      if(supported(code)){assert(host.uiInputs[0]==std::make_pair(code,1000u+code));++accepted;}
      same(*f,before);
    }
    assert(accepted==7);
  }
  std::cout<<"PASS exhaustive uint8 event space admits exactly seven stock navigation events on USB and BLE without service mutation\n";

  {
    auto f=std::make_unique<Fixture>(127);f->usb.inputResult=0;const auto before=state(*f);
    auto canonical=adminBytes(input(10));
    std::vector<std::vector<uint8_t>> invalid;
    auto unknown=canonical;unknown.insert(unknown.end(),{0xc0,0x07,0x00});invalid.push_back(unknown);
    auto duplicate=canonical;duplicate.insert(duplicate.end(),canonical.begin(),canonical.end());invalid.push_back(duplicate);
    auto alternate=canonical;
    // A second valid admin oneof collapses in nanopb; use remove_fixed_position.
    alternate.insert(alternate.end(),{0xd0,0x02,0x01});invalid.push_back(alternate);
    auto explicitKeyZero=canonical;explicitKeyZero[2]=4;explicitKeyZero.insert(explicitKeyZero.end(),{0x10,0x00});invalid.push_back(explicitKeyZero);
    auto explicitTouchZero=canonical;explicitTouchZero[2]=4;explicitTouchZero.insert(explicitTouchZero.end(),{0x18,0x00});invalid.push_back(explicitTouchZero);
    auto noncanonical=canonical;noncanonical[2]=3;noncanonical[4]=0x8a;noncanonical.push_back(0x00);invalid.push_back(noncanonical);
    auto malformed=canonical;malformed.pop_back();invalid.push_back(malformed);
    invalid.push_back({0xda,0x01,0x03,0x08,0x80,0x02});       // 256 must not truncate to NONE.
    invalid.push_back({0xda,0x01,0x03,0x08,0x8a,0x02});       // 266 must not truncate to SELECT.
    invalid.push_back({0xda,0x01,0x06,0x08,0xff,0xff,0xff,0xff,0x0f});
    for(const auto &wire:invalid) {
      f->usb.out.clear();f->usb.uiInputs.clear();rawAdmin(*f,wire,false,2000);
      assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&f->usb.uiInputs.empty());same(*f,before);
    }
    for(auto a:{input(10,1),input(10,0,1,0),input(10,0,0,1)}) {
      f->usb.out.clear();f->usb.uiInputs.clear();f->admin(a,false,2001);
      assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&f->usb.uiInputs.empty());same(*f,before);
    }
  }
  std::cout<<"PASS overflow, unknown, duplicate, alternate, explicit-zero, noncanonical, malformed, keyboard and touch wires refuse before UI dispatch\n";

  {
    auto xiao=std::make_unique<Fixture>(81);xiao->usb.inputResult=-ENOTSUP;const auto x=state(*xiao);
    xiao->admin(input(20));assert(xiao->usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&xiao->usb.uiInputs.size()==1&&notice(xiao->usb,"Display input unavailable"));same(*xiao,x);
    auto failed=std::make_unique<Fixture>(127);failed->usb.inputResult=-ENODEV;const auto d=state(*failed);
    failed->admin(input(20));assert(failed->usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&failed->usb.uiInputs.size()==1&&notice(failed->usb,"Display input unavailable"));same(*failed,d);
    auto unready=std::make_unique<Fixture>(127);unready->usb.inputResult=0;unready->usb.readyFlag=false;const auto u=state(*unready);
    unready->admin(input(20));assert(unready->usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&unready->usb.uiInputs.empty());same(*unready,u);
  }
  std::cout<<"PASS headless, unavailable display and unready storage/config host refuse truthfully without mutation\n";

  {
    auto f=std::make_unique<Fixture>(127);f->usb.inputResult=f->ble.inputResult=0;
    f->admin(input(20),false,10);f->admin(input(19),true,11);f->admin(input(10),false,12);f->admin(input(27),true,13);
    assert(f->usb.uiInputs.size()==2&&f->ble.uiInputs.size()==2);
    assert(f->usb.uiInputs[0]==std::make_pair(20u,10u)&&f->ble.uiInputs[0]==std::make_pair(19u,11u));
    f->begin(20);assert(!f->edit.dirty());const auto batch=state(*f);f->usb.out.clear();f->admin(input(28),false,21);
    assert(f->usb.error()==0&&f->edit.open()&&!f->edit.dirty());same(*f,batch);
  }
  {
    auto f=std::make_unique<Fixture>(127);f->ble.inputResult=0;f->provision();assert(f->service.start(false,1));
    f->radio.state=0;f->service.rx(nullptr,0,0,2,0);assert(f->service.faulted());
    const auto fault=state(*f);f->ble.out.clear();f->admin(input(17),true,22);assert(f->ble.error()==0);same(*f,fault);
  }
  std::cout<<"PASS interleaved USB/BLE, clean open batch and radio-fault states preserve controller-only behavior\n";

  {
    auto f=std::make_unique<Fixture>(127);f->usb.inputResult=0;auto cfg=f->service.config;cfg.bytes[4]=1;cfg.setTxEnabled(true);
    assert(!f->edit.save(cfg,1)&&f->service.start(false,2)&&f->service.armEndpoint(true));
    text(*f,0x19190002,3);const auto queued=state(*f);f->usb.out.clear();f->admin(input(18),false,4);
    assert(f->usb.error()==0&&f->service.endpointFree()==3&&f->service.endpointArmed());same(*f,queued);
  }
  std::cout<<"PASS queued TX, arm state and RF/service counters remain unchanged by admitted local UI input\n";
}
