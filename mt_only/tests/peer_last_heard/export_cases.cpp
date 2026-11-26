// SPDX-License-Identifier: GPL-3.0-only
// Real Service/Companion/nanopb exports with deterministic radio/time inputs.
// PrivateRuntime fakes prove the admitted-event boundary, not cryptography.
#include "Companion.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

namespace {
using Bytes=std::vector<uint8_t>;
constexpr uint32_t Self=0x12345678,Peer=0x87654321,Other=0x76543210;
unsigned groups=0;
void pass(const char *name){++groups;std::cout<<"PASS "<<name<<'\n';}
void put(uint8_t *out,uint32_t value){for(unsigned i=0;i<4;++i)out[i]=uint8_t(value>>(8*i));}
struct Radio final:mt::Radio {
  unsigned sends=0;int state=0;
  bool ready()override{return true;}
  int start()override{state=1;return 0;}
  int stop()override{state=0;return 0;}
  int restore()override{state=1;return 0;}
  int health()override{return state;}
  bool busy()override{return false;}
  uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *,size_t)override{++sends;return 0;}
  bool completion(int &)override{return false;}
};
struct Host final:mt::Companion::Host {
  mt::Service &service;std::vector<meshtastic_FromRadio> out;
  mutable unsigned clockReads=0;unsigned previews=0,edits=0,saves=0,radioObservations=0;
  uint32_t clock=0;
  explicit Host(mt::Service &value):service(value){}
  bool ready()const override{return true;}
  int save(const mt::Config &)override{++saves;return -ENOTSUP;}
  uint32_t wallTime()const override{++clockReads;return clock;}
  int editNode(uint32_t id,mt::NodeCache::Edit op,uint32_t)override{
    ++edits;return service.nodes.edit(id,Self,op)<0?-EINVAL:0;
  }
  void observeRadioPeer(uint32_t,unsigned,bool,float,uint32_t)override{++radioObservations;}
  void preview(uint32_t,uint32_t,uint32_t,unsigned,const uint8_t *,size_t,int16_t,int8_t,uint32_t,bool)override{++previews;}
  bool send(const uint8_t *bytes,size_t size)override{
    assert(bytes&&size>=4&&bytes[0]==0x94&&bytes[1]==0xc3);
    assert((size_t(bytes[2])<<8|bytes[3])==size-4);
    meshtastic_FromRadio decoded{};auto stream=pb_istream_from_buffer(bytes+4,size-4);
    assert(pb_decode(&stream,meshtastic_FromRadio_fields,&decoded));out.push_back(decoded);return true;
  }
};
void input(mt::Companion &client,const meshtastic_ToRadio &request){
  uint8_t bytes[512];auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
  assert(pb_encode(&stream,meshtastic_ToRadio_fields,&request));client.packet(bytes,stream.bytes_written,1,mt::Companion::WorkspaceProof{true});
}
void connect(mt::Companion &client){
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
  request.want_config_id=42;input(client,request);
}
struct Fixture {
  Radio radio;mt::Service service;Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion a,b;
  explicit Fixture(uint8_t model=81,mt::pki::PrivateRuntime *runtime=nullptr):
    service(radio,false,Self,model,true,false,runtime),usb(service),ble(service),
    a(service,usb,model,workspace,0),b(service,ble,model,workspace,1){
    mt::Config config;config.bytes[4]=1;auto node=config.node();node.setPublic();config.node(node);
    assert(service.apply(config)&&service.start(false,0));
    connect(a);connect(b);clear();usb.clock=123;ble.clock=456;
  }
  void clear(){usb.out.clear();ble.out.clear();}
  void rx(const Bytes &wire,uint32_t captured,uint32_t now=1000,bool throughService=true,bool radioReceive=true){
    if(throughService)service.rx(wire.data(),wire.size(),3,now,77);
    const auto before=usb.clockReads+ble.clockReads;
    const auto observations=usb.radioObservations+ble.radioObservations;
    a.receive(wire.data(),wire.size(),-90,3,now,captured,true,3,radioReceive,mt::Companion::WorkspaceProof{true});
    b.receive(wire.data(),wire.size(),-90,3,now,captured,true,3,radioReceive,mt::Companion::WorkspaceProof{true});
    assert(before==usb.clockReads+ble.clockReads&&radio.sends==0&&!usb.saves&&!ble.saves);
    if(!radioReceive)assert(observations==usb.radioObservations+ble.radioObservations);
  }
  uint32_t last(uint32_t id=Peer)const{
    const int slot=service.nodes.find(id);assert(slot>=0);return service.nodes.get(unsigned(slot))->lastHeard();
  }
};
Bytes frame(Fixture &f,const meshtastic_Data &data,uint32_t id,uint32_t from=Peer,uint32_t to=0xffffffffu,bool viaMqtt=false){
  uint8_t encoded[239],wire[255];size_t length=0;
  auto stream=pb_ostream_from_buffer(encoded,sizeof(encoded));assert(pb_encode(&stream,meshtastic_Data_fields,&data));
  assert(f.service.codec.encodeZeroHopData(from,to,id,encoded,stream.bytes_written,wire,sizeof(wire),length));
  if(viaMqtt)wire[12]|=0x10u;
  return {wire,wire+length};
}
Bytes text(Fixture &f,uint32_t id,uint32_t from=Peer,uint32_t to=0xffffffffu,bool viaMqtt=false){
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;data.has_bitfield=true;
  data.payload.size=5;memcpy(data.payload.bytes,"hello",5);return frame(f,data,id,from,to,viaMqtt);
}
Bytes node(Fixture &f,uint32_t id,uint8_t key=0,uint32_t from=Peer,bool viaMqtt=false){
  meshtastic_User user{};strcpy(user.id,"!not-trusted");strcpy(user.long_name,"Synthetic peer");strcpy(user.short_name,"SYN");
  user.hw_model=meshtastic_HardwareModel_XIAO_NRF52_KIT;
  if(key){user.public_key.size=32;memset(user.public_key.bytes,key,32);}
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_NODEINFO_APP;data.has_bitfield=true;
  auto stream=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
  assert(pb_encode(&stream,meshtastic_User_fields,&user));data.payload.size=stream.bytes_written;
  return frame(f,data,id,from,0xffffffffu,viaMqtt);
}
void seed(Fixture &f,uint32_t captured=10,uint8_t key=0,uint32_t from=Peer){f.rx(node(f,1,key,from),captured);f.clear();}
unsigned nodeRecords(const Host &host,uint32_t expected,uint32_t id=Peer,bool viaMqtt=false){
  unsigned count=0;
  for(const auto &out:host.out)if(out.which_payload_variant==meshtastic_FromRadio_node_info_tag&&out.node_info.num==id){
    ++count;assert(out.node_info.last_heard==expected&&out.node_info.via_mqtt==viaMqtt);
  }
  return count;
}
unsigned packets(const Host &host,uint32_t id,uint32_t captured,bool privatePacket=false,bool viaMqtt=false){
  unsigned count=0;
  for(const auto &out:host.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.id==id){
    ++count;assert(out.packet.has_rx_time==(captured!=0)&&out.packet.rx_time==captured);
    assert(out.packet.pki_encrypted==privatePacket&&out.packet.via_mqtt==viaMqtt);
  }
  return count;
}
void policy(Fixture &f,uint32_t id=Peer){
  meshtastic_AdminMessage admin{};admin.which_payload_variant=meshtastic_AdminMessage_set_favorite_node_tag;
  admin.set_favorite_node=id;
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;
  auto &packet=request.packet;packet.id=100;packet.to=Self;
  packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;packet.decoded.portnum=meshtastic_PortNum_ADMIN_APP;
  auto stream=pb_ostream_from_buffer(packet.decoded.payload.bytes,sizeof(packet.decoded.payload.bytes));
  assert(pb_encode(&stream,meshtastic_AdminMessage_fields,&admin));packet.decoded.payload.size=stream.bytes_written;
  input(f.a,request);
}
// This provider emulates only already-admitted immutable private events. It
// cannot establish cryptographic validity, replay persistence or live RF proof.
struct AdmittedEvent final:mt::pki::PrivateRuntime {
  bool available=true,issue=true,malformed=false,pending[2]{};uint32_t source=Peer;
  mt::pki::PrivateEvent received{};unsigned takes=0;
  uint32_t peer()const override{return Peer;}
  bool ready(uint32_t local)const override{return available&&local==Self;}
  bool publicIdentity(uint8_t out[32])const override{memset(out,0x42,32);return available;}
  bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,uint32_t,uint32_t,bool,uint32_t,bench::Frame &)override{return false;}
  void freeze()override{available=false;clearEvent();}
  bool matchesPeer(uint32_t id)const override{return id==Peer;}
  bool revoke(uint32_t)override{return false;}
  void clearEvent()override{pending[0]=pending[1]=false;received={};}
  void sweep(mt::Endpoint &)override{}
  int submit(mt::Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,bool,const uint8_t *,size_t,bool,uint32_t)override{return -ENOTSUP;}
  void receive(mt::Endpoint &,bench::MtCodec &,const uint8_t *wire,size_t size,uint32_t,uint32_t,bool)override{
    assert(wire&&size>=17);received={};received.from=source;received.to=Self;received.id=bench::le32(wire+8);
    memset(received.publicKey,0x24,32);
    if(malformed){received.data[0]=0x12;received.data[1]=0x7f;received.size=2;}
    else {
      meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;data.has_bitfield=true;
      data.payload.size=7;memcpy(data.payload.bytes,"private",7);
      auto stream=pb_ostream_from_buffer(received.data,sizeof(received.data));
      assert(pb_encode(&stream,meshtastic_Data_fields,&data));received.size=stream.bytes_written;
    }
    pending[0]=pending[1]=issue;
  }
  void proof(mt::Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t)override{}
  bool event(uint8_t owner,const uint8_t *,size_t,mt::pki::PrivateEvent &out)override{
    ++takes;if(owner>1||!pending[owner])return false;pending[owner]=false;out=received;return true;
  }
};
Bytes privateFrame(uint32_t id,uint32_t from=Peer){Bytes wire(17,0);put(wire.data(),Self);put(wire.data()+4,from);put(wire.data()+8,id);return wire;}
}

int main(){
  for(uint8_t model:{81,127}){
    Fixture f(model);f.rx(node(f,1),1700000000);
    assert(f.service.nodes.count()==1&&f.last()==1700000000);
    assert(nodeRecords(f.usb,1700000000)==1&&nodeRecords(f.ble,1700000000)==1);
    assert(packets(f.usb,1,1700000000)==1&&packets(f.ble,1,1700000000)==1);
    pass("board NodeInfo admission precedes both real nanopb cached-node and packet exports");
  }
  {
    Fixture f;seed(f,0);assert(f.last()==0);
    connect(f.a);connect(f.b);assert(nodeRecords(f.usb,0)==1&&nodeRecords(f.ble,0)==1);
    f.clear();f.rx(node(f,2),50);assert(f.last()==50&&nodeRecords(f.usb,50)==1);
    f.clear();f.rx(node(f,3),0);assert(f.last()==50&&nodeRecords(f.usb,50)==1&&nodeRecords(f.ble,50)==1);
    assert(packets(f.usb,3,0)==1&&packets(f.ble,3,0)==1);
    pass("startup unknown is zero; later unknown retains cache but packet UTC stays absent");
  }
  {
    Fixture f;seed(f,1700000000);f.rx(node(f,2),100);
    assert(f.last()==100&&nodeRecords(f.usb,100)==1&&nodeRecords(f.ble,100)==1);
    f.clear();f.rx(text(f,3),UINT32_MAX);assert(f.last()==UINT32_MAX);
    connect(f.a);assert(nodeRecords(f.usb,UINT32_MAX)==1);
    pass("nonzero backwards correction and maximum UTC replace the previous observation");
  }
  {
    Fixture f;seed(f);const auto before=f.service.nodes.count();f.rx(text(f,2),20);
    assert(f.last()==20&&f.service.nodes.count()==before&&!nodeRecords(f.usb,20));
    assert(packets(f.usb,2,20)==1&&packets(f.ble,2,20)==1);
    f.clear();connect(f.a);connect(f.b);assert(nodeRecords(f.usb,20)==1&&nodeRecords(f.ble,20)==1);
    pass("accepted known-peer public traffic updates handshake exports without new node events");
  }
  {
    Fixture f;f.rx(text(f,1),77);assert(!f.service.nodes.count());
    assert(packets(f.usb,1,77)==1&&packets(f.ble,1,77)==1);
    pass("unknown public text remains deliverable and never allocates a peer");
  }
  {
    Fixture f;seed(f);f.a.resetStream();f.b.resetStream();f.clear();
    f.rx(text(f,2),60);assert(f.last()==60&&f.usb.out.empty()&&f.ble.out.empty());
    f.rx(node(f,3),70);assert(f.last()==70&&f.usb.out.empty()&&f.ble.out.empty());
    connect(f.a);connect(f.b);assert(nodeRecords(f.usb,70)==1&&nodeRecords(f.ble,70)==1);
    pass("disconnected transports still observe admitted public traffic before later handshake");
  }
  {
    Fixture f;seed(f,20);auto duplicate=text(f,2);f.rx(duplicate,30,2000);f.clear();
    f.rx(duplicate,80,3000);assert(f.last()==30&&f.usb.out.empty()&&f.ble.out.empty());
    auto duplicateNode=node(f,3);f.rx(duplicateNode,40,4000);f.clear();
    f.rx(duplicateNode,90,5000);assert(f.last()==40&&f.usb.out.empty()&&f.ble.out.empty());
    pass("public text and NodeInfo duplicate guards precede timestamp mutation");
  }
  {
    Fixture f;seed(f,20,0x42);f.rx(node(f,2,0x43),90);f.rx(node(f,3),100);
    assert(f.last()==20&&f.usb.out.empty()&&f.ble.out.empty());
    f.rx(node(f,4,0x42),30);assert(f.last()==30&&nodeRecords(f.usb,30)==1);
    pass("changed or removed pinned identity key cannot refresh time; admitted identity can");
  }
  {
    Fixture f;seed(f,20);meshtastic_Data data{};data.has_bitfield=true;data.portnum=meshtastic_PortNum_NODEINFO_APP;
    data.payload.size=2;data.payload.bytes[0]=0x12;data.payload.bytes[1]=0x7f;
    f.rx(frame(f,data,2),100);assert(f.last()==20&&f.usb.out.empty()&&f.ble.out.empty());
    pass("malformed NodeInfo protobuf cannot refresh the known peer");
  }
  {
    Fixture f;seed(f,20);meshtastic_Data data{};data.has_bitfield=true;data.portnum=meshtastic_PortNum_NODEINFO_APP;
    // User.hw_model=-1 is parseable nanopb but outside the cache's admitted range.
    const uint8_t invalid[]={0x28,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x01};
    data.payload.size=sizeof(invalid);memcpy(data.payload.bytes,invalid,sizeof(invalid));
    f.rx(frame(f,data,2),100);assert(f.last()==20&&f.usb.out.empty()&&f.ble.out.empty());
    pass("parseable invalid User admission cannot refresh the known peer");
  }
  {
    Fixture f;seed(f,20);meshtastic_Data data{};data.has_bitfield=true;
    data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;data.payload.size=1;data.payload.bytes[0]='x';
    auto wire=frame(f,data,2);
    // Local origination correctly bans signatures. An external adversarial
    // frame uses its real header and actual Public AES, not that trusted API.
    data.xeddsa_signature.size=64;memset(data.xeddsa_signature.bytes,0xa5,64);
    uint8_t encoded[239],nonce[16];auto stream=pb_ostream_from_buffer(encoded,sizeof(encoded));
    assert(pb_encode(&stream,meshtastic_Data_fields,&data));wire.resize(16+stream.bytes_written);
    memcpy(wire.data()+16,encoded,stream.bytes_written);bench::MtCodec::nonce(Peer,2,nonce);
    assert(bench::mtAesCtr(nullptr,bench::MtNodeRecord::PublicKey,16,nonce,wire.data()+16,stream.bytes_written));
    f.rx(wire,100);assert(f.last()==20&&f.usb.out.empty()&&f.ble.out.empty());
    pass("unverified signed public frame cannot cross signature guard to refresh time");
  }
  {
    Fixture f;seed(f,20);assert(f.service.nodes.edit(Peer,Self,mt::NodeCache::Edit::Ignore)==1);
    f.rx(node(f,2),90);f.rx(text(f,3),100);assert(f.last()==20&&f.usb.out.empty()&&f.ble.out.empty());
    pass("ignored peers cannot refresh time through public NodeInfo or text");
  }
  {
    Fixture f;assert(f.service.nodes.edit(Peer,Self,mt::NodeCache::Edit::Ignore)==1);
    assert(f.service.nodes.edit(Peer,Self,mt::NodeCache::Edit::Unignore)==1);
    assert(f.service.nodes.get(0)->placeholder());f.rx(text(f,1),90);
    assert(f.last()==0&&f.service.nodes.get(0)->placeholder()&&packets(f.usb,1,90)==1);
    f.clear();f.rx(node(f,2),100);assert(f.last()==100&&!f.service.nodes.get(0)->placeholder());
    assert(nodeRecords(f.usb,100)==1);
    pass("unignored placeholder text delivers without time; full NodeInfo admission enables time");
  }
  for(unsigned mode=0;mode<9;++mode){
    Fixture f;seed(f,20);auto wire=text(f,2);
    if(mode==0)wire.resize(16);
    if(mode==1)put(wire.data()+4,1);
    if(mode==2)put(wire.data()+4,Self);
    if(mode==3)put(wire.data(),Other);
    if(mode==4)put(wire.data()+8,0);
    if(mode==5)wire[12]=uint8_t(1<<5|2);
    if(mode==6)wire[13]^=0xff;
    if(mode==7){wire.resize(17);wire.back()=0xff;}
    if(mode==8)f.service.stop();
    f.rx(wire,90);assert(f.last()==20&&f.usb.out.empty()&&f.ble.out.empty());
    pass("existing header/codec/service guard rejects without time mutation");
  }
  {
    Fixture f;seed(f,20);for(auto port:{meshtastic_PortNum_ADMIN_APP,meshtastic_PortNum_ROUTING_APP,
                                     meshtastic_PortNum_TRACEROUTE_APP,meshtastic_PortNum_POSITION_APP,
                                     meshtastic_PortNum_TELEMETRY_APP}){
      meshtastic_Data data{};data.portnum=port;data.has_bitfield=true;
      if(port!=meshtastic_PortNum_ADMIN_APP&&port!=meshtastic_PortNum_ROUTING_APP)data.request_id=900;
      if(port==meshtastic_PortNum_ROUTING_APP){data.payload.size=2;data.payload.bytes[0]=24;data.payload.bytes[1]=0;}
      f.rx(frame(f,data,10+unsigned(port),Peer,Self),90);
      assert(f.last()==20&&f.usb.out.empty()&&f.ble.out.empty());
    }
    pass("RF admin/routing and unmatched application-response guards precede time mutation");
  }
  for(uint8_t model:{81,127}){
    Fixture f(model);seed(f,20);f.rx(node(f,2,0,Peer,true),30,1000,false,false);
    assert(f.last()==30&&f.service.nodes.get(0)->viaMqtt);
    assert(nodeRecords(f.usb,30,Peer,true)==1&&nodeRecords(f.ble,30,Peer,true)==1);
    f.clear();connect(f.a);connect(f.b);
    assert(nodeRecords(f.usb,30,Peer,true)==1&&nodeRecords(f.ble,30,Peer,true)==1);
    f.rx(text(f,3,Peer,0xffffffffu,true),40,1000,false,false);
    assert(f.last()==40&&f.service.nodes.get(0)->viaMqtt);
    assert(packets(f.usb,3,40,false,true)==1&&packets(f.ble,3,40,false,true)==1);
    f.clear();f.rx(text(f,3),90);
    assert(f.last()==40&&f.service.nodes.get(0)->viaMqtt&&f.usb.out.empty()&&f.ble.out.empty());
    meshtastic_Data malformed{};malformed.has_bitfield=true;malformed.portnum=meshtastic_PortNum_NODEINFO_APP;
    malformed.payload.size=2;malformed.payload.bytes[0]=0x12;malformed.payload.bytes[1]=0x7f;
    f.rx(frame(f,malformed,4,Peer,0xffffffffu,true),100,1000,false,false);
    assert(f.last()==40&&f.service.nodes.get(0)->viaMqtt&&f.usb.out.empty()&&f.ble.out.empty());
    assert(f.service.nodes.edit(Peer,Self,mt::NodeCache::Edit::Ignore)==1);
    f.rx(text(f,5,Peer,0xffffffffu,true),110,1000,false,false);
    assert(f.last()==40&&f.service.nodes.get(0)->viaMqtt);
    const auto count=f.service.nodes.count();f.rx(text(f,6,Other,0xffffffffu,true),120,1000,false,false);
    assert(f.service.nodes.count()==count&&f.service.nodes.find(Other)<0);
    assert(packets(f.usb,6,120,false,true)==1&&packets(f.ble,6,120,false,true)==1);
    pass("MQTT origin exports and RF duplicate guard on both models; malformed, ignored and absent peers do not mutate provenance");
  }
  {
    Fixture f;seed(f,70);policy(f);assert(f.usb.edits==1&&f.service.nodes.get(0)->favorite());
    assert(nodeRecords(f.usb,70)==1&&f.last()==70&&!f.radio.sends&&!f.usb.saves);
    pass("local policy NodeInfo export retains admitted UTC through the shared cachedNode encoder");
  }
  {
    AdmittedEvent runtime;Fixture f(81,&runtime);seed(f,20);f.clear();
    f.rx(privateFrame(2),50);assert(f.last()==50&&packets(f.usb,2,50,true)==1&&packets(f.ble,2,50,true)==1);
    f.clear();f.rx(privateFrame(3),0);assert(f.last()==50&&packets(f.usb,3,0,true)==1);
    f.clear();f.rx(privateFrame(4),10);assert(f.last()==10&&packets(f.ble,4,10,true)==1);
    connect(f.a);assert(nodeRecords(f.usb,10)==1);
    pass("admitted private event boundary uses captured UTC/unknown/backwards semantics on both owners");
  }
  {
    AdmittedEvent runtime;Fixture f(127,&runtime);seed(f,20);
    f.a.resetStream();f.b.resetStream();f.clear();f.rx(privateFrame(2),90);
    assert(f.last()==90&&f.usb.out.empty()&&f.ble.out.empty());
    connect(f.a);connect(f.b);assert(nodeRecords(f.usb,90)==1&&nodeRecords(f.ble,90)==1);
    pass("admitted private event boundary updates while disconnected before later handshake");
  }
  {
    AdmittedEvent runtime;Fixture f(81,&runtime);seed(f,20);seed(f,30,0,Other);
    runtime.source=Other;f.rx(privateFrame(2),90);
    assert(f.last()==20&&f.last(Other)==90);
    pass("private event boundary uses immutable event.from rather than raw frame identity");
  }
  {
    AdmittedEvent runtime;Fixture f(81,&runtime);f.rx(privateFrame(1),90);
    assert(!f.service.nodes.count()&&packets(f.usb,1,90,true)==1&&packets(f.ble,1,90,true)==1);
    pass("unknown admitted private sender still delivers without allocating a peer");
  }
  for(unsigned mode=0;mode<4;++mode){
    AdmittedEvent runtime;Fixture f(81,&runtime);seed(f,20);
    if(mode==0)runtime.issue=false;
    if(mode==1)runtime.malformed=true;
    if(mode==2)runtime.available=false;
    if(mode==3)assert(f.service.nodes.edit(Peer,Self,mt::NodeCache::Edit::Ignore)==1);
    f.rx(privateFrame(2),90);assert(f.last()==20&&f.usb.out.empty()&&f.ble.out.empty());
    pass("private event absence/decoding/readiness/ignore guard rejects without time mutation");
  }
  {
    AdmittedEvent runtime;Fixture f(81,&runtime);seed(f,20);auto wire=privateFrame(2);f.rx(wire,50);f.clear();
    f.rx(wire,90,2000,false);assert(f.last()==50&&f.usb.out.empty()&&f.ble.out.empty());
    pass("consumed private event cannot refresh again without a new admitted Service event");
  }
  {
    AdmittedEvent runtime;Fixture f(81,&runtime);seed(f,20);auto mqtt=privateFrame(2);mqtt[12]|=0x10u;
    f.rx(mqtt,50,1000,true,false);assert(f.service.nodes.get(0)->viaMqtt&&packets(f.usb,2,50,true,true)==1);
    f.clear();f.rx(privateFrame(3),60);
    assert(!f.service.nodes.get(0)->viaMqtt&&packets(f.ble,3,60,true,false)==1);
    f.clear();runtime.malformed=true;auto invalid=privateFrame(4);invalid[12]|=0x10u;f.rx(invalid,70,1000,true,false);
    assert(!f.service.nodes.get(0)->viaMqtt&&f.usb.out.empty()&&f.ble.out.empty());
    pass("admitted private MQTT origin flips to RF; malformed private payload cannot mutate provenance");
  }
  assert(groups==37);std::cout<<"EXPORT_GROUPS "<<groups<<'\n';
}
