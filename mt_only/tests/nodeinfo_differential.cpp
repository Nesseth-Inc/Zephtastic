// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include <algorithm>
#include "phone_heartbeat_oracle.h"

namespace stock {
using NodeNum=uint32_t;
struct NodeDB {NodeNum getNodeNum()const{return OURS;}} db;
NodeDB *nodeDB=&db;
struct {struct {uint8_t hop_limit=3;} lora;} config;
namespace Default {static uint8_t getConfiguredOrDefaultHopLimit(uint8_t configured){return configured?configured:3;}}
int8_t getHopsAway(const meshtastic_MeshPacket &,int8_t defaultIfUnknown=-1);
struct RoutingModule {uint8_t getHopLimitForResponse(const meshtastic_MeshPacket &);};
RoutingModule router;
RoutingModule *routingModule=&router;
#define USERPREFS_EVENT_MODE 0
// Whole functions extracted without body substitutions; hashes/lines recorded.
#include "upstream-nodeinfo.inc"
}

static void phoneHeartbeat(unsigned role,uint32_t now) {
  Fixture f(true,81,true);
  f.s.stop();auto cfg=f.s.config;cfg.setTxEnabled(true);
  assert(cfg.setDeviceRole(role,cfg.rebroadcastMode()));
  assert(f.s.apply(cfg)&&f.s.start(false,now)&&f.s.armEndpoint(true));
  phoneStock::history={};phoneStock::transport={};phoneStock::module={};
  phoneStock::config.device.role=static_cast<meshtastic_Config_DeviceConfig_Role>(role);
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_heartbeat_tag;
  auto send=[&](uint32_t nonce,uint32_t at,bool emitted){
    const auto before=f.s.nodeDiscovery.queued;const auto statuses=f.h.out.size();
    r.heartbeat.nonce=nonce;phoneStock::ticks=at;
    const bool queue=phoneStock::receive(r);input(f.c,r,at);
    assert(f.h.out.size()==statuses+(queue?1:0));
    if(queue){assert(f.h.out.back().which_payload_variant==meshtastic_FromRadio_queueStatus_tag);
              assert(f.h.out.back().queueStatus.mesh_packet_id==0);}
    assert(f.s.nodeDiscovery.queued==before+emitted&&f.s.nodeDiscovery.queued==phoneStock::transport.accepted);
    if(!emitted)return;
    f.s.poll(at+300,1);
    assert(f.radio.frames.size()==f.s.nodeDiscovery.queued);
    auto &wire=f.radio.frames.back();const auto &expected=phoneStock::transport.packet;
    bench::RouteMetadata actual;assert(f.s.codec.decode(wire.data(),wire.size(),actual));
    assert(bench::le32(wire.data())==expected.to&&bench::le32(wire.data()+4)==OURS&&bench::le32(wire.data()+8)!=0);
    assert(actual.portnum==meshtastic_PortNum_NODEINFO_APP&&actual.nodeInfoValid&&!actual.request_id);
    assert(mt::Discovery::wantsResponse(actual)==expected.decoded.want_response);
    assert(expected.channel==0&&wire[13]==f.s.config.node().bytes[4]&&!(wire[12]&8));
    assert(expected.priority==meshtastic_MeshPacket_Priority_DEFAULT);
    f.done(at+301);
  };
  send(0,now,false);send(2,now,false);send(UINT32_MAX,now,false);
  send(1,now,true);send(1,now+302,false);send(1,now+59999,false);
  send(1,now+60000,true);send(2,now+60400,false);
}

int main() {
  for(auto role:{1u,5u,6u})for(auto start:{750u,UINT32_MAX-30700u})phoneHeartbeat(role,start);
  std::cout<<"PASS PhoneAPI dispatch, normal QueueStatus and real role/airtime NodeInfo at first/repeat/59999/60000 milliseconds and clock wrap\n";
  unsigned vectors=0;
  for(unsigned start=0;start<8;++start)for(unsigned hop=0;hop<=start;++hop)
    for(bool ack:{false,true})for(bool empty:{false,true}) {
      Fixture f(true,vectors%2?81:127,true);
      f.s.stop();auto cfg=f.s.config;auto owner=cfg.node();
      assert(owner.setName("Node owner name 24chars!",false));
      assert(owner.setName("LCL1",true));cfg.node(owner);
      assert(f.s.apply(cfg)&&f.s.start(false,0)&&f.s.armEndpoint(true));
      const uint32_t requestId=1000+vectors;
      bench::Frame request;
      if(empty) {
        const uint8_t data[]={8,4,72,2};size_t n=0;
        assert(f.s.codec.encodeZeroHopData(PEER,OURS,requestId,data,sizeof(data),request.bytes,255,n));request.len=n;
      } else assert(mt::Discovery::encode(f.s.codec,owner,PEER,81,OURS,requestId,true,0,request));
      request.bytes[12]=uint8_t(hop|(start<<5)|(ack?8:0));
      meshtastic_MeshPacket incoming{},expected{};
      incoming.from=PEER;incoming.to=OURS;incoming.id=requestId;incoming.channel=0;
      incoming.hop_start=start;incoming.hop_limit=hop;incoming.want_ack=ack;
      incoming.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
      incoming.decoded.has_bitfield=true;
      expected.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
      stock::setReplyTo(&expected,incoming);
      f.s.rx(request.bytes,request.len,0,1000,requestId+10000);f.s.poll(1300,1);
      assert(f.radio.sends==1);auto &wire=f.radio.frames[0];bench::RouteMetadata got;
      assert(f.s.codec.decode(wire.data(),wire.size(),got));
      assert(bench::le32(wire.data())==expected.to&&bench::le32(wire.data()+4)==OURS);
      assert(got.request_id==expected.decoded.request_id&&bool(wire[12]&8)==expected.want_ack);
      assert((wire[12]&7)==expected.hop_limit);
      assert((wire[12]>>5)==(wire[12]&7)&&got.portnum==4&&!mt::Discovery::wantsResponse(got));
      meshtastic_User user{};strcpy(user.id,"!12345678");strcpy(user.long_name,"Node owner name 24chars!");strcpy(user.short_name,"LCL1");
      user.hw_model=static_cast<meshtastic_HardwareModel>(vectors%2?81:127);
      user.role=meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE;
      user.has_is_unmessagable=true;user.is_unmessagable=false;
      uint8_t payload[64];auto stream=pb_ostream_from_buffer(payload,sizeof(payload));
      assert(pb_encode(&stream,meshtastic_User_fields,&user));
      // Fixed-length User.macaddr is absent on our wire; pinned nanopb emits
      // six zero bytes when canonicalizing it. Compare decoded semantics.
      meshtastic_User actual{};auto in=pb_istream_from_buffer(got.payload,got.payloadLength);
      assert(pb_decode(&in,meshtastic_User_fields,&actual));
      uint8_t canonical[64];auto encoded=pb_ostream_from_buffer(canonical,sizeof(canonical));
      assert(pb_encode(&encoded,meshtastic_User_fields,&actual));
      assert(encoded.bytes_written==stream.bytes_written&&!memcmp(canonical,payload,stream.bytes_written));
      ++vectors;
    }
  assert(vectors==144);
  // Keep shared fixture utilities under -Werror without executing unrelated tests.
  (void)&results;(void)&group;
  std::cout<<"PASS "<<vectors<<" reply vectors against four exact pinned stock functions; decoded User matches pinned nanopb\n";
}
