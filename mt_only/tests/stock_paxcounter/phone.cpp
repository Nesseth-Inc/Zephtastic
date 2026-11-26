// SPDX-License-Identifier: GPL-3.0-only
#include "pki_runtime_fixture.h"
using namespace mt;
int main(){
  Fixture f(true);f.s.stop();
  struct Snapshot {bool ready=true;unsigned reads=0;};
  Snapshot snap;
  assert(f.s.bindPaxSnapshot([](void *ctx,meshtastic_Paxcount &out){
    auto &s=*static_cast<Snapshot*>(ctx);++s.reads;
    out=meshtastic_Paxcount{};out.uptime=42;return s.ready;
  },&snap));
  assert(f.s.start(false,0));
  for(unsigned test=0;test<3;++test){
    meshtastic_ToRadio request{};
    request.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=request.packet;p.id=601+test;p.to=f.local.binding.node;
    p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_PAXCOUNTER_APP;
    p.decoded.want_response=true;
    if(test==1){p.decoded.payload.size=1;p.decoded.payload.bytes[0]=0;}
    snap.ready=test!=2;
    f.usb.out.clear();input(f.c,request);assert(f.radio.frames.empty());
    bool correlated=false;
    for(auto &event:f.usb.out)if(event.which_payload_variant==meshtastic_FromRadio_packet_tag&&event.packet.decoded.request_id==p.id){
      auto &d=event.packet.decoded;
      if(!test){
        assert(d.portnum==meshtastic_PortNum_PAXCOUNTER_APP);
        auto stream=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
        meshtastic_Paxcount c{};assert(pb_decode(&stream,meshtastic_Paxcount_fields,&c)&&!c.wifi&&!c.ble&&c.uptime==42);
      }else{
        assert(d.portnum==meshtastic_PortNum_ROUTING_APP);meshtastic_Routing r{};
        auto stream=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
        assert(pb_decode(&stream,meshtastic_Routing_fields,&r)&&r.which_variant==meshtastic_Routing_error_reason_tag);
        assert(r.error_reason==(test==1?meshtastic_Routing_Error_BAD_REQUEST:meshtastic_Routing_Error_NO_RESPONSE));
      }
      correlated=true;
    }
    assert(correlated);
  }
  assert(snap.reads==2);
  puts("PASS USB self-addressed Paxcounter empty report, disabled and malformed responses, no radio admissions");
}
