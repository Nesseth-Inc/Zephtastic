// SPDX-License-Identifier: GPL-3.0-only
#include "wall_clock_fixture.h"
static void group(const char *s){std::cout<<"PASS "<<s<<'\n';}
namespace {
constexpr uint32_t PrivateLocal=0x12345678,PrivatePeer=0x87654321;
struct PrivateClockRuntime final:mt::pki::PrivateRuntime {
  bool available=true,pending[2]{};mt::pki::PrivateEvent received{};
  uint32_t peer()const override{return PrivatePeer;}
  bool ready(uint32_t local)const override{return available&&local==PrivateLocal;}
  bool publicIdentity(uint8_t out[32])const override{memset(out,0x42,32);return available;}
  bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,uint32_t,uint32_t,bool,uint32_t,bench::Frame &)override{return false;}
  void freeze()override{available=false;clearEvent();}
  bool matchesPeer(uint32_t value)const override{return value==PrivatePeer;}
  bool revoke(uint32_t)override{return false;}
  void clearEvent()override{pending[0]=pending[1]=false;received={};}
  void sweep(mt::Endpoint &)override{}
  int submit(mt::Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,bool,const uint8_t *,size_t,bool,uint32_t)override{return -ENOTSUP;}
  void receive(mt::Endpoint &,bench::MtCodec &,const uint8_t *frame,size_t size,uint32_t,uint32_t,bool)override{
    assert(frame&&size>=17);received={};received.from=PrivatePeer;received.to=PrivateLocal;
    received.id=bench::le32(frame+8);memset(received.publicKey,0x24,32);
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
    data.payload.size=7;memcpy(data.payload.bytes,"private",7);data.has_bitfield=true;
    auto stream=pb_ostream_from_buffer(received.data,sizeof(received.data));
    assert(pb_encode(&stream,meshtastic_Data_fields,&data));received.size=stream.bytes_written;
    pending[0]=pending[1]=true;
  }
  void proof(mt::Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t)override{}
  bool event(uint8_t owner,const uint8_t *,size_t,mt::pki::PrivateEvent &out)override{
    if(owner>1||!pending[owner])return false;
    pending[owner]=false;out=received;return true;
  }
};
struct PrivateClockHost final:mt::Companion::Host {
  mt::WallClock &clock;uint64_t milliseconds=0;std::vector<meshtastic_FromRadio> out;
  explicit PrivateClockHost(mt::WallClock &value):clock(value){}
  bool ready()const override{return true;}
  int save(const mt::Config &)override{return -ENOTSUP;}
  int setWallTime(uint32_t value)override{return clock.set(value,milliseconds);}
  uint32_t wallTime()const override{return clock.at(milliseconds);}
  bool send(const uint8_t *bytes,size_t size)override{
    assert(size>=4);meshtastic_FromRadio value{};auto stream=pb_istream_from_buffer(bytes+4,size-4);
    assert(pb_decode(&stream,meshtastic_FromRadio_fields,&value));out.push_back(value);return true;
  }
};
}
int main() {
 {
  mt::WallClock c;assert(c.at(0)==0&&c.at(UINT64_MAX)==0);assert(c.set(0,0)==-EINVAL&&c.at(1000)==0);
  assert(c.set(1,42)==0&&c.at(42)==1&&c.at(1041)==1&&c.at(1042)==2);
  assert(c.at(41)==0&&c.set(100,41)==-EINVAL&&c.at(1042)==2);
  group("fresh unset, zero invalid, full nonzero range, elapsed rounding and backward monotonic rejection");
 }
 {
  mt::WallClock c;assert(c.set(1700000000,100)==0);
  assert(c.set(1700000001,1099)==-EAGAIN&&c.set(1700000000,1099)==0);
  assert(c.at(1100)==1700000001);assert(c.set(1700000001,1999)==0);
  assert(c.at(2100)==1700000002);assert(c.set(100,1800099)==-EAGAIN);
  assert(c.set(100,1800100)==0&&c.at(1800100)==100);
  assert(c.set(200,3600100)==0&&c.at(3601100)==201);
  group("same-current-second no-op keeps fractional anchor and thirty-minute correction deadline");
 }
 {
  mt::WallClock c;uint64_t start=UINT64_C(0xffffffff)-500;assert(c.set(1700000000,start)==0);
  for(uint64_t delta:{UINT64_C(1000),UINT64_C(0x100000000),UINT64_C(0x300000000)})assert(c.at(start+delta)==1700000000+delta/1000);
  mt::WallClock edge;assert(edge.set(UINT32_MAX-1,10)==0&&edge.at(1010)==UINT32_MAX&&edge.at(2010)==0);
  assert(edge.set(10,2010)==-EAGAIN&&edge.set(10,1800010)==0&&edge.at(1800010)==10);
  mt::WallClock last;assert(last.set(UINT32_MAX,UINT64_MAX-999)==0&&last.at(UINT64_MAX)==UINT32_MAX);
  assert(last.at(0)==0&&last.set(1,0)==-EINVAL);mt::WallClock reboot;assert(reboot.at(10)==0);
  group("64-bit monotonic wraps avoided; UTC overflow becomes unknown; reset starts unset");
 }
 for(uint8_t model:{81,127})for(bool ble:{false,true}) {
  auto f=std::make_unique<ClockFixture>(model);auto &h=ble?f->ble:f->usb;
  f->connect(ble);f->echo(UINT32_MAX,ble);assert(h.error()==0);
  assert(!h.out.back().packet.has_rx_time&&!h.out[h.out.size()-2].packet.has_rx_time&&h.out[h.out.size()-2].packet.rx_time==0);
  f->set(1700000000,ble);assert(h.error()==0&&h.out.back().packet.has_rx_time&&h.out.back().packet.rx_time==1700000000);
  f->tick(1999);f->echo(12,!ble);auto &opposite=ble?f->usb:f->ble;
  assert(opposite.error()==0&&opposite.out[opposite.out.size()-2].packet.rx_time==1700000001);
  f->set(0,ble);assert(h.error()==meshtastic_Routing_Error_BAD_REQUEST&&f->clock.at(1999)==1700000001);
  f->set(50,ble);assert(h.error()==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED&&h.out.back().packet.rx_time==1700000001);
  f->set(1700000001,!ble);assert(opposite.error()==0);f->tick(1800000);f->set(25,!ble);assert(opposite.error()==0&&h.wallTime()==25);
  assert(!f->backend.writes&&!f->usb.saves&&!f->ble.saves&&!f->radio.sends&&!f->radio.starts&&!f->service.endpointArmed());
  group("both board/transport paths share RAM time, reject zero and early correction, normalize echo timestamp");
 }
 {
  auto f=std::make_unique<Fixture>();f->op(meshtastic_AdminMessage_set_time_only_tag);assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE);
  group("default Host reports unavailable, never fabricates a timestamp");
 }
 {
  auto f=std::make_unique<ClockFixture>();f->provision();assert(f->service.start(false,0));f->connect();f->connect(true);
  f->receive(101);assert(!f->usb.out.back().packet.has_rx_time&&!f->ble.out.back().packet.has_rx_time);
  const auto historical=f->usb.records.back();f->set(1700000000);f->tick(3123);f->receive(102);
  assert(f->usb.out.back().packet.has_rx_time&&f->usb.out.back().packet.rx_time==1700000003&&f->ble.out.back().packet.rx_time==1700000003);
  f->milliseconds=f->usb.clockMs=3999;f->ble.clockMs=4000;f->receive(104);
  assert(f->usb.out.back().packet.rx_time==1700000003&&f->ble.out.back().packet.rx_time==1700000003);
  meshtastic_FromRadio old{};auto s=pb_istream_from_buffer(historical.data(),historical.size());assert(pb_decode(&s,meshtastic_FromRadio_fields,&old)&&!old.packet.has_rx_time);
  auto count=f->usb.records.size();f->receive(103,meshtastic_PortNum_ADMIN_APP);assert(f->usb.records.size()==count&&f->clock.at(3123)==1700000003);
 group("public RF fanout shares one captured second; history remains unset and RF admin never sets clock");
 }
 {
  mt::WallClock clock;PrivateClockRuntime runtime;Radio radio;
  mt::Service service(radio,true,PrivateLocal,81,true,false,&runtime);
  PrivateClockHost usb(clock),ble(clock);mt::companion::CompanionWorkspace workspace;mt::Companion companion(service,usb,81,workspace),bleCompanion(service,ble,81,workspace,1);
  mt::Config config;config.bytes[4]=1;auto node=config.node();node.setPublic();config.node(node);
  assert(service.apply(config)&&service.start(false,0));
  auto connect=[](mt::Companion &client){
    meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;request.want_config_id=91;
    uint8_t bytes[64];auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&stream,meshtastic_ToRadio_fields,&request));
    client.packet(bytes,stream.bytes_written,1,mt::Companion::WorkspaceProof{true});
  };
  connect(companion);connect(bleCompanion);usb.out.clear();ble.out.clear();
  auto receive=[&](uint32_t id,uint64_t capturedAt=UINT64_MAX){
    uint8_t frame[17]{};for(unsigned i=0;i<4;++i){frame[i]=uint8_t(PrivateLocal>>(8*i));frame[4+i]=uint8_t(PrivatePeer>>(8*i));frame[8+i]=uint8_t(id>>(8*i));}
    service.rx(frame,sizeof(frame),3,uint32_t(usb.milliseconds),81);
    const uint64_t at=capturedAt==UINT64_MAX?usb.milliseconds:capturedAt;
    const uint32_t captured=clock.at(at);
    companion.receive(frame,sizeof(frame),-80,3,uint32_t(usb.milliseconds),captured,mt::Companion::WorkspaceProof{true});
    bleCompanion.receive(frame,sizeof(frame),-80,3,uint32_t(ble.milliseconds),captured,mt::Companion::WorkspaceProof{true});
  };
  receive(90);assert(usb.out.size()==1&&ble.out.size()==1&&!usb.out.back().packet.has_rx_time&&!ble.out.back().packet.has_rx_time);
  assert(clock.set(1700000000,100)==0);usb.milliseconds=ble.milliseconds=3123;receive(91);
  assert(usb.out.size()==2&&ble.out.size()==2&&usb.out.back().packet.pki_encrypted&&ble.out.back().packet.pki_encrypted);
  assert(usb.out.back().packet.has_rx_time&&ble.out.back().packet.has_rx_time&&usb.out.back().packet.rx_time==1700000003&&ble.out.back().packet.rx_time==1700000003);
  usb.milliseconds=1099;ble.milliseconds=1100;receive(92,1099);
  assert(usb.out.back().packet.rx_time==1700000000&&ble.out.back().packet.rx_time==1700000000);
  group("authenticated private RX fanout uses one captured clock value across USB/BLE second boundary");
 }
 for(bool active:{false,true}) {
  auto f=std::make_unique<ClockFixture>();f->provision();assert(f->service.start(false,0)&&f->service.armEndpoint(true));text(*f,991,1);
  if(active){f->service.poll(300,700);assert(f->service.active());}
  const auto config=f->service.config;const auto free=f->service.endpointFree(),writes=f->backend.writes,stops=f->radio.stops,sends=f->radio.sends;
  f->service.nodeDiscovery.observeArm(true,1);f->service.nodeDiscovery.admitted(100,10800);f->service.nodeDiscovery.replied(0x2345,100);
  f->set(1700000000);assert(f->usb.error()==0);f->tick(1800000);f->set(50,true);assert(f->ble.error()==0);
  assert(f->service.config.same(config)&&f->service.endpointArmed()&&f->service.receiving()&&f->service.active()==active);
  assert(f->service.endpointFree()==free&&f->backend.writes==writes&&f->radio.stops==stops&&f->radio.sends==sends);
  assert(!f->service.nodeDiscovery.periodic(10800099)&&f->service.nodeDiscovery.periodic(10800100)&&!f->service.nodeDiscovery.replyAllowed(0x2345,10800100));
  group("clock corrections preserve queued/active frame, TX arm, RX, settings and uptime discovery deadlines");
 }
 {
  auto f=std::make_unique<ClockFixture>();f->begin(100);f->admin(device(7200),false,200);assert(f->edit.open()&&f->edit.dirty());
  f->set(1700000000);assert(f->usb.error()==0&&f->edit.open()&&f->edit.dirty()&&!f->backend.writes);
  f->tick(50000);f->set(1700000050,true);assert(f->ble.error()==0&&!f->edit.poll(60199,true));
  assert(!f->edit.poll(60200,true)&&!f->edit.open()&&f->backend.writes==1);
  group("clock requests neither flush settings nor extend the monotonic edit idle deadline");
 }
}
