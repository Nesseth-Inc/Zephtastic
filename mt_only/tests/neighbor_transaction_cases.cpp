// SPDX-License-Identifier: GPL-3.0-only
// Actual public Service/Companion transaction integration; memory radios only.
#include "endpoint_fixture.h"
#include "NeighborQuery.h"
#include "radio_diagnostic_cases.inc"

namespace {
using Bytes=std::vector<uint8_t>;
meshtastic_ToRadio query(uint32_t id=77,uint32_t to=PEER) {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
  auto &p=r.packet;p.id=id;p.to=to;p.hop_limit=0;
  p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_NEIGHBORINFO_APP;p.decoded.want_response=true;return r;
}
struct Snapshot {
  unsigned calls=0;bool available=true;mt::Service *service=nullptr;
  meshtastic_NeighborInfo value{};
  Snapshot(){value.node_id=OURS;value.last_sent_by_id=OURS;value.node_broadcast_interval_secs=3600;}
  static bool read(void *ctx,meshtastic_NeighborInfo &out){
    auto &self=*static_cast<Snapshot *>(ctx);++self.calls;out=self.value;return self.available;
  }
  void bind(Fixture &f){f.s.stop();service=&f.s;assert(f.s.bindNeighborSnapshot(read,this));assert(f.s.start(false,0)&&f.s.armEndpoint(true));}
};
Bytes encode(const meshtastic_Data &d){uint8_t bytes[239]{};auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&out,meshtastic_Data_fields,&d));return {bytes,bytes+out.bytes_written};}
Bytes response(uint32_t request=77){
  Snapshot s;uint8_t bytes[239]{};size_t size=0;
  assert(mt::NeighborQuery::response(request,s.value,bytes,sizeof(bytes),size));return {bytes,bytes+size};
}
Bytes wire(Fixture &f,const Bytes &plain,uint32_t from=PEER,uint32_t to=OURS,uint32_t id=700){
  uint8_t frame[255]{};size_t n=0;assert(f.s.codec.encodeZeroHopData(from,to,id,plain.data(),plain.size(),frame,sizeof(frame),n));return {frame,frame+n};
}
Bytes requestWire(Fixture &f,uint32_t from=PEER,uint32_t id=900){
  uint8_t frame[22]{};size_t size=0;assert(f.s.codec.encodeNeighborQuery(from,OURS,id,frame,sizeof(frame),size));return {frame,frame+size};
}
unsigned replies(const Host &host,uint32_t request=77){
  unsigned n=0;for(const auto &r:host.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&
    r.packet.decoded.portnum==meshtastic_PortNum_NEIGHBORINFO_APP&&r.packet.decoded.request_id==request)++n;
  return n;
}
void begin(Fixture &f,bool both=false){auto q=query();input(f.c,q,100);if(both)input(f.bc,q,101);f.s.poll(300,1);assert(f.radio.sends==1);f.done(301);}
}
int main(){
  std::cout.setf(std::ios::unitbuf);
  for(uint8_t model:{uint8_t(81),uint8_t(127)})for(unsigned owner=0;owner<2;++owner){
    Fixture f(true,model);auto q=query();input(owner?f.bc:f.c,q,100);f.s.poll(300,1);assert(f.radio.sends==1);f.done(301);
    bench::RouteMetadata m{};assert(f.s.codec.decode(f.radio.frames[0].data(),f.radio.frames[0].size(),m));
    assert(m.portnum==71&&m.wantResponse&&!m.payloadLength&&!m.request_id&&m.hasBitfield&&m.bitfield==2);
    assert(f.radio.frames[0].size()==22&&!(f.radio.frames[0][12]&8));
    f.rx(wire(f,response()),500);f.clients();assert(replies(owner?f.bh:f.h)==1&&replies(owner?f.h:f.bh)==0);
    f.rx(wire(f,response()),600);assert(replies(owner?f.bh:f.h)==1&&f.radio.sends==1&&!f.s.nodes.count());
  }
  group("four board/owner paths select Neighbor transaction before opaque port71 and deliver once");
  {
    Fixture f;begin(f,true);f.rx(f.wire(PEER,OURS,690,77,0),400);f.clients();assert(!replies(f.h)&&!results(f.h,77));
    f.rx(wire(f,response()),500);f.clients();assert(replies(f.h)==1&&replies(f.bh)==1&&f.radio.sends==1);
  }
  group("identical USB/BLE owners share one query; successful public Routing ACK cannot complete it");
  {
    for(unsigned which=0;which<9;++which){Fixture f;begin(f);auto plain=response();auto frame=wire(f,plain);
      switch(which){case 0:frame=wire(f,plain,0x99887766);break;case 1:frame=wire(f,plain,PEER,0xffffffffu);break;
        case 2:frame=wire(f,response(78));break;case 3:frame[13]^=1;break;case 4:frame[12]=1;break;
        case 5:plain.insert(plain.end(),{8,71});frame=wire(f,plain);break;
        case 6:plain.insert(plain.end(),{0xa0,1,1});frame=wire(f,plain);break;
        case 7:frame.back()^=2;break;
        default:{meshtastic_Data d{};d.portnum=meshtastic_PortNum_NEIGHBORINFO_APP;d.has_bitfield=true;d.request_id=77;
          d.payload.size=2;d.payload.bytes[0]=8;d.payload.bytes[1]=0x80;frame=wire(f,encode(d));break;}}
      f.rx(frame,500);f.clients();assert(!replies(f.h)&&!results(f.h,77));
      f.rx(wire(f,response(),PEER,OURS,701),600);assert(replies(f.h)==1);
    }
  }
  group("wrong peer/destination/request/channel/hops and noncanonical whole Data cannot consume query");
  {
    Fixture early;auto q=query();input(early.c,q,100);early.rx(wire(early,response()),150);assert(!replies(early.h));
    early.s.poll(300,1);early.done(301);early.rx(wire(early,response(),PEER,OURS,701),500);assert(replies(early.h)==1);
    Fixture timed;begin(timed);timed.rx(wire(timed,response()),90100);timed.clients();assert(!replies(timed.h)&&results(timed.h,77,3)==1&&timed.radio.sends==1);
    Fixture shared;begin(shared,true);shared.c.resetStream();shared.rx(wire(shared,response()),500);assert(!replies(shared.h)&&replies(shared.bh)==1);
    Fixture nak;begin(nak);nak.rx(nak.wire(PEER,OURS,703,77,8),500);nak.clients();assert(results(nak.h,77,8)==1);nak.rx(wire(nak,response()),600);assert(!replies(nak.h));
  }
  group("pre-TX/expired/disconnected owners and correlated NAK isolate Neighbor response lifecycle");
  {
    for(unsigned bad=0;bad<10;++bad){Fixture f;auto q=query();auto &p=q.packet;
      switch(bad){case 0:p.to=0xffffffffu;break;case 1:p.to=1;break;case 2:p.from=PEER;break;case 3:p.want_ack=true;break;
        case 4:p.decoded.payload.size=1;p.decoded.payload.bytes[0]=0;break;case 5:p.decoded.has_bitfield=true;break;
        case 6:p.decoded.request_id=3;break;case 7:p.decoded.xeddsa_signature.size=64;break;case 8:p.hop_start=1;break;default:p.xeddsa_signed=true;break;}
      input(f.c,q,100);f.s.poll(300,1);f.clients();assert(!f.radio.sends&&results(f.h,p.id)>0);
    }
  }
  group("remote selectors reject caller body/provenance/broadcast/ACK/metadata without sending");
  {
    Fixture f;Snapshot snapshot;snapshot.bind(f);auto req=requestWire(f);f.rx(req,1000);assert(snapshot.calls==1&&f.s.endpointControls()==1);
    f.s.poll(1300,1);assert(f.radio.sends==1);bench::RouteMetadata m{};assert(f.s.codec.decode(f.radio.frames.back().data(),f.radio.frames.back().size(),m));
    assert(m.portnum==71&&m.request_id==900&&!m.wantResponse&&m.hasBitfield&&m.bitfield==0);
    meshtastic_NeighborInfo info{};auto in=pb_istream_from_buffer(m.payload,m.payloadLength);assert(pb_decode(&in,meshtastic_NeighborInfo_fields,&info)&&!in.bytes_left);
    assert(info.node_id==OURS&&info.last_sent_by_id==OURS&&info.neighbors_count==0);f.done(1301);
    f.rx(requestWire(f,0x99887766,901),180999);assert(snapshot.calls==1&&!f.s.endpointControls());
    f.rx(requestWire(f,0x99887766,902),181000);assert(snapshot.calls==2&&f.s.endpointControls()==1);
  }
  group("public responder returns canonical available-empty snapshot; one 3-minute window spans peers");
  {
    Fixture f;Snapshot snapshot;snapshot.available=false;snapshot.bind(f);f.rx(requestWire(f),1000);assert(snapshot.calls==1&&!f.s.endpointControls());
    snapshot.available=true;f.rx(requestWire(f,PEER,901),1001);assert(snapshot.calls==2&&f.s.endpointControls()==1);
    Fixture full;Snapshot other;other.bind(full);
    for(uint32_t i=0;i<4;++i){auto q=full.wire(PEER,OURS,1000+i,0,0,8,true);full.rx(q,1000+2001*i,900+i);}
    assert(full.s.endpointControls()==4);full.rx(requestWire(full),8000);assert(other.calls==0&&full.s.endpointControls()==4);
    full.s.stop();assert(full.s.start(false,9000)&&full.s.armEndpoint(true));full.rx(requestWire(full,PEER,901),10000);assert(other.calls==1&&full.s.endpointControls()==1);
  }
  group("unavailable producer and full control queue do not consume the shared reply window");
  {
    Fixture f;Snapshot snapshot;assert(!f.s.bindNeighborSnapshot(Snapshot::read,&snapshot));snapshot.bind(f);
    assert(!f.s.bindNeighborSnapshot(Snapshot::read,&snapshot));f.s.stop();assert(!f.s.bindNeighborSnapshot(Snapshot::read,&snapshot));
    meshtastic_NeighborInfo out{};snapshot.available=false;assert(!f.s.neighborSnapshot(out));assert(!out.node_id&&!out.neighbors_count);
    snapshot.available=true;assert(f.s.neighborSnapshot(out)&&out.node_id==OURS&&snapshot.calls==2);
  }
  group("snapshot provider binds once off-RX and clears partial output on producer refusal");
  radioDiagnosticCases();
  std::cout<<"PASS Neighbor public transactions complete\n";
}
