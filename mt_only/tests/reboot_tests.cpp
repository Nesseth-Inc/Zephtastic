// SPDX-License-Identifier: GPL-3.0-only
// Actual companion/codec/scheduler, fake clock and radio; no reset syscall or IO.
#include "conformance_fixture.h"
#include "RebootSchedule.h"
#include <climits>

struct RebootHost:Host {
  mt::RebootSchedule &schedule;uint64_t &now;unsigned calls=0,saves=0;
  bool available=true,outputReady=true;int32_t last=0;
  RebootHost(mt::Service &s,mt::RebootSchedule &r,uint64_t &n):Host(s),schedule(r),now(n){}
  bool ready()const override{return available;}
  int save(const mt::Config &)override{++saves;return -EIO;}
  bool scheduleReboot(int32_t seconds)override {
    ++calls;last=seconds;schedule.request(seconds,now);return true;
  }
  bool send(const uint8_t *p,size_t n)override{return outputReady&&Host::send(p,n);}
};
struct RebootFixture {
  Radio radio;mt::Service s;mt::RebootSchedule schedule;uint64_t now=0;
  RebootHost h,bh;mt::companion::CompanionWorkspace workspace;mt::Companion c,bc;
  explicit RebootFixture(uint8_t model):s(radio,true,OURS,model,true),h(s,schedule,now),bh(s,schedule,now),c(s,h,model,workspace,0),bc(s,bh,model,workspace,1){}
  void send(bool ble,const meshtastic_ToRadio &r) {
    if(ble){input(bc,r,uint32_t(now));return;}
    uint8_t data[512];auto b=pb_ostream_from_buffer(data,sizeof(data));assert(pb_encode(&b,meshtastic_ToRadio_fields,&r));
    c.byte(0x94,uint32_t(now),0,mt::Companion::WorkspaceProof{true});c.byte(0xc3,uint32_t(now),0,mt::Companion::WorkspaceProof{true});c.byte(uint8_t(b.bytes_written>>8),uint32_t(now),0,mt::Companion::WorkspaceProof{true});c.byte(uint8_t(b.bytes_written),uint32_t(now),0,mt::Companion::WorkspaceProof{true});
    for(size_t i=0;i<b.bytes_written;++i)c.byte(data[i],uint32_t(now),0,mt::Companion::WorkspaceProof{true});
  }
  static meshtastic_ToRadio request(int32_t seconds,uint32_t id=42) {
    meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_reboot_seconds_tag;a.reboot_seconds=seconds;
    return admin(a,id);
  }
  static meshtastic_ToRadio admin(const meshtastic_AdminMessage &a,uint32_t id=42) {
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
    p.to=OURS;p.id=id;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
    auto b=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
    assert(pb_encode(&b,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=b.bytes_written;return r;
  }
};
int main() {
  {
    mt::RebootSchedule r;assert(!r.pending()&&!r.takeDue(UINT64_MAX));
    for(int32_t n:{-1,-2,INT32_MIN}){r.request(n,0);assert(!r.pending()&&!r.takeDue(100));}
    r.request(0,100);assert(r.pending()&&!r.takeDue(199)&&r.takeDue(200)&&!r.takeDue(201));
    r.request(1,100);assert(!r.takeDue(1099)&&r.takeDue(1100));
  }
  std::cout<<"PASS reboot inert, negative cancellation, minimum delay and one-shot consumption\n";
  {
    mt::RebootSchedule r;r.request(1,0);r.request(2,500);
    assert(!r.takeDue(1000)&&!r.takeDue(2499)&&r.takeDue(2500));
    r.request(10,0);r.request(0,100);assert(!r.takeDue(199)&&r.takeDue(200));
    r.request(0,0);r.request(INT32_MIN,99);assert(!r.pending()&&!r.takeDue(100));
    r.request(INT32_MAX,123);const uint64_t due=123+uint64_t(INT32_MAX)*1000;
    assert(!r.takeDue(due-1)&&r.takeDue(due));
  }
  std::cout<<"PASS reboot replacement and int32 extremes avoid arithmetic overflow\n";
  {
    mt::RebootSchedule r;const uint64_t start=uint64_t(UINT32_MAX)-50;
    r.request(1,start);assert(!r.takeDue(start+999)&&r.takeDue(start+1000));
    r.request(0,UINT64_MAX-50);assert(!r.takeDue(UINT64_MAX)&&!r.takeDue(48)&&r.takeDue(49));
    r.request(INT32_MAX,UINT64_MAX-500);const uint64_t due=uint64_t(INT32_MAX)*1000-501;
    assert(!r.takeDue(due-1)&&r.takeDue(due));
  }
  std::cout<<"PASS reboot deadlines cross both 32-bit and 64-bit clock wrap\n";
  for(uint8_t model:{uint8_t(81),uint8_t(127)})for(bool ble:{false,true}) {
    auto f=std::make_unique<RebootFixture>(model);auto &h=ble?f->bh:f->h;
    const auto original=f->s.config;f->now=uint64_t(UINT32_MAX)+700;
    for(int32_t seconds:{0,1,INT32_MAX,-1,INT32_MIN}) {
      h.out.clear();const auto calls=h.calls;f->send(ble,RebootFixture::request(seconds));
      assert(h.calls==calls+1&&h.last==seconds&&results(h,42,0)==1);
      assert(!h.saves&&f->s.config.same(original)&&!f->radio.sends&&!f->s.receiving()&&!f->s.endpointArmed());
      assert(f->schedule.pending()==(seconds>=0));
      if(seconds>=0) {
        const uint64_t delay=seconds?uint64_t(seconds)*1000:100;
        assert(!f->schedule.takeDue(f->now+delay-1)&&f->schedule.takeDue(f->now+delay));
      }
    }
    std::cout<<"PASS reboot protocol acceptance/ACK with no flash or RF mutation "<<unsigned(model)<<" "<<ble<<"\n";
    for(unsigned v=0;v<6;++v) {
      h.out.clear();auto r=RebootFixture::request(0);
      if(v==0)r.packet.channel=1;
      if(v==1)r.packet.to=PEER;
      if(v==2)r.packet.to=0xffffffffu;
      if(v==3){r.packet.which_payload_variant=meshtastic_MeshPacket_encrypted_tag;r.packet.encrypted.size=1;}
      if(v==4){r.packet.decoded.payload.size=1;r.packet.decoded.payload.bytes[0]=0x80;}
      if(v==5)h.available=false;
      const auto calls=h.calls;f->send(ble,r);assert(h.calls==calls&&!f->schedule.pending()&&results(h,42,0)==0);
      h.available=true;
    }
    for(unsigned v=0;v<4;++v) {
      meshtastic_AdminMessage a{};
      if(v==0){a.which_payload_variant=meshtastic_AdminMessage_shutdown_seconds_tag;a.shutdown_seconds=0;}
      if(v==1){a.which_payload_variant=meshtastic_AdminMessage_enter_dfu_mode_request_tag;a.enter_dfu_mode_request=true;}
      if(v==2){a.which_payload_variant=meshtastic_AdminMessage_factory_reset_device_tag;a.factory_reset_device=1;}
      if(v==3){a.which_payload_variant=meshtastic_AdminMessage_reboot_ota_seconds_tag;a.reboot_ota_seconds=0;}
      const auto calls=h.calls;h.out.clear();f->send(ble,RebootFixture::admin(a));
      assert(h.calls==calls&&results(h,42,32)==1&&!f->schedule.pending()&&!h.saves);
    }
    std::cout<<"PASS reboot existing admission and unsupported destructive/DFU operations stay refused "<<unsigned(model)<<" "<<ble<<"\n";
  }
  {
    auto f=std::make_unique<RebootFixture>(127);f->send(false,RebootFixture::request(10));
    f->now=50;f->send(true,RebootFixture::request(-1));assert(!f->schedule.pending());
    f->send(true,RebootFixture::request(1));f->now=100;f->send(false,RebootFixture::request(2));
    f->c.resetStream();f->bc.resetStream();assert(f->schedule.pending());
    assert(!f->schedule.takeDue(1050)&&!f->schedule.takeDue(2099)&&f->schedule.takeDue(2100));
    f->h.outputReady=false;f->send(false,RebootFixture::request(0));
    assert(f->c.outputDrops>0&&f->schedule.takeDue(200)); // Acceptance cannot promise transport delivery.
  }
  std::cout<<"PASS reboot shared USB/BLE replacement, disconnect persistence and best-effort ACK\n";
  {
    auto f=std::make_unique<RebootFixture>(127);mt::Config cfg;cfg.bytes[4]=1;cfg.seal();
    assert(f->s.apply(cfg)&&f->s.start(false,0));
    auto r=RebootFixture::request(0);r.packet.decoded.has_bitfield=true;r.packet.decoded.want_response=false;
    uint8_t data[239],wire[255];auto b=pb_ostream_from_buffer(data,sizeof(data));size_t length=0;
    assert(pb_encode(&b,meshtastic_Data_fields,&r.packet.decoded));
    assert(f->s.codec.encodeZeroHopData(PEER,OURS,42,data,b.bytes_written,wire,sizeof(wire),length));
    f->s.rx(wire,length,3,50,0);f->c.receive(wire,length,-90,3,50,mt::Companion::WorkspaceProof{true});f->bc.receive(wire,length,-90,3,50,mt::Companion::WorkspaceProof{true});
    assert(!f->h.calls&&!f->bh.calls&&!f->schedule.pending()&&!f->radio.sends);
  }
  std::cout<<"PASS reboot RF admin cannot schedule a local reset\n";
  {
    struct FailingStopRadio:Radio {int stop()override{return -EIO;}} radio;
    mt::Service s(radio,true,OURS,127,true);mt::Config cfg;cfg.bytes[4]=1;cfg.seal();
    assert(s.apply(cfg)&&s.start(false,0)&&s.armEndpoint(true));
    mt::RebootSchedule r;unsigned stops=0,resets=0;r.request(0,0);
    auto stop=[&]{++stops;assert(!resets);s.stop();};
    auto reset=[&]{++resets;assert(stops==1&&s.faulted()&&!s.receiving()&&!s.endpointArmed());};
    r.poll(99,stop,reset);assert(!stops&&!resets);
    r.poll(100,stop,reset);assert(stops==1&&resets==1&&!r.pending());
    r.poll(101,stop,reset);assert(stops==1&&resets==1);
  }
  std::cout<<"PASS reboot main-loop execution stops RF before reset even on stop failure\n";
}
