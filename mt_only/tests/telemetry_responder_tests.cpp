// SPDX-License-Identifier: GPL-3.0-only
#include "telemetry_query_fixture.h"
#include "known_channel_fixture.h"
static std::vector<uint8_t> selector(uint32_t from=PEER,uint32_t to=OURS,uint32_t id=100,uint8_t flags=0) {
 return rawFrame({8,67,18,2,18,0,24,1,72,2},from,to,id,flags);
}
static meshtastic_Telemetry decodeReply(Fixture &f,uint32_t request,uint32_t uptime,bool ack=false,size_t frames=1) {
 assert(f.radio.frames.size()==frames);const auto &p=f.radio.frames.back();bench::RouteMetadata m;assert(f.s.codec.decode(p.data(),p.size(),m));
 assert(m.portnum==67&&m.request_id==request&&!m.wantResponse&&m.hasBitfield&&m.bitfield==0&&bool(p[12]&8)==ack);assert(bench::le32(p.data())==PEER&&bench::le32(p.data()+4)==OURS);
 meshtastic_Telemetry t{};auto in=pb_istream_from_buffer(m.payload,m.payloadLength);assert(pb_decode(&in,meshtastic_Telemetry_fields,&t));auto &v=t.variant.device_metrics;
 assert(t.which_variant==meshtastic_Telemetry_device_metrics_tag&&!t.time&&v.has_uptime_seconds&&v.uptime_seconds==uptime);
 // The M97 fixture explicitly binds an empty, supported channel logger for
 // normal TX admission. Its present zero estimate is valid metadata; battery,
 // voltage and the separately unbound TX-airtime logger remain unavailable.
 assert(v.has_channel_utilization&&v.channel_utilization==0);
 assert(!v.has_battery_level&&!v.has_voltage&&!v.has_air_util_tx);return t;
}
int main(){std::cout.setf(std::ios::unitbuf);
 for(auto model:{81,127})for(uint64_t uptime:{0ull,123ull,0xffffffffull}){Fixture f(true,model);f.s.measuredUptime(uptime);auto before=f.s.config;f.rx(selector(),100,700);f.s.poll(400,0);decodeReply(f,100,uint32_t(uptime));assert(f.s.config.same(before));f.done(401);f.s.poll(20000,0);assert(f.radio.sends==1);}
 group("both models: measured uptime zero/max and explicitly bound idle channel zero; no invented sensors or reliability retries");
 for(unsigned mode=0;mode<4;++mode){Fixture f(mode!=1);if(mode!=0)f.s.measuredUptime(mode==2?uint64_t(UINT32_MAX)+1:55);if(mode==3)f.s.armEndpoint(false);f.rx(selector());f.s.poll(1000,0);assert(!f.radio.sends);}
 group("unknown/out-of-range measurement, RX-only and disarmed gates remain closed");
 for(unsigned mode=0;mode<10;++mode){Fixture f;f.s.measuredUptime(77);auto p=selector();if(mode==0)p=selector(PEER,0xffffffffu);if(mode==1)p=selector(OURS);if(mode==2)p[13]^=1;if(mode==3)p[12]=0x10;if(mode==4)p[12]=0x23;if(mode==5)p=rawFrame({8,67,18,2,18,0,24,1,72,2,8,67});if(mode==6)p=rawFrame({8,67,18,0,24,1,72,2});if(mode==7)p=rawFrame({8,67,18,2,18,0,24,1,72,0});if(mode==8)p=rawFrame({8,67,18,2,18,0,24,1,53,1,0,0,0,72,2});if(mode==9)p=goodFrame();f.rx(p);f.s.poll(1000,0);assert(!f.radio.sends);}
 group("broadcast/self/wrong channel/reserved-bit/bad hop and noncanonical selectors cannot trigger responses");
 for(unsigned hop=4;hop<=7;++hop){Fixture f;f.s.measuredUptime(77);auto p=selector();p[12]=uint8_t(hop|(hop<<5));f.rx(p,100,700);f.s.poll(400,0);decodeReply(f,100,77);assert(f.radio.frames[0][12]==0x42);f.done(401);}
 group("four-through-seven hop selectors are valid and retain the stock direct-response two-hop budget");
 {Fixture f;f.s.measuredUptime(1);f.rx(selector(),100,700);f.s.poll(400,0);f.done(401);f.rx(selector(),600,701);f.rx(selector(PEER,OURS,101),700,702);f.s.poll(1200,0);assert(f.radio.sends==1);f.rx(selector(PEER,OURS,102),5200,703);f.s.poll(5500,0);assert(f.radio.sends==2);}
 group("duplicate requests suppressed for replay window and fresh peer requests rate-limited for five seconds");
 {Fixture f;f.s.measuredUptime(1);for(unsigned i=0;i<8;++i)f.rx(selector(PEER+i,OURS,100+i),100+i,700+i);f.s.poll(1000,0);assert(f.radio.sends==1);for(unsigned i=0;i<6;++i){f.done(1100+i*500);f.s.poll(1400+i*500,0);}assert(f.radio.sends==4);}
 group("generated responses share four bounded control slots without allocation or queue amplification");
 {Fixture requester;beginQuery(requester);Radio radio;mt::test::KnownChannelFixtureClock knownChannelResponder;mt::Service responder(radio,true,PEER,127,true,false);knownChannelResponder.bind(responder);mt::Config cfg;cfg.bytes[4]=1;auto n=cfg.node();n.setPublic();cfg.node(n);assert(responder.apply(cfg)&&responder.start(false,0)&&responder.armEndpoint(true));responder.measuredUptime(321);const auto &q=requester.radio.frames[0];responder.rx(q.data(),q.size(),3,400,700);responder.poll(800,0);assert(radio.frames.size()==1);requester.rx(radio.frames[0],1000);requester.clients();std::vector<uint8_t> payload;assert(telemetries(requester.h,77,&payload)==1&&results(requester.h,77,0));meshtastic_Telemetry t{};auto in=pb_istream_from_buffer(payload.data(),payload.size());assert(pb_decode(&in,meshtastic_Telemetry_fields,&t)&&t.variant.device_metrics.uptime_seconds==321);requester.rx(radio.frames[0],1100);assert(telemetries(requester.h,77)==1);}
 group("two real service instances complete correlated query exactly once without synthetic response construction");
 for(auto model:{81,127}){Fixture f(true,model);f.s.measuredUptime(77);auto p=selector();p[12]=8;f.rx(p,100,700);f.s.poll(400,0);decodeReply(f,100,77,true);
  const auto reply=f.radio.frames[0];const auto replyId=bench::le32(reply.data()+8);f.done(401);assert(!f.s.storageStopSafe());
  f.rx(f.wire(PEER,OURS,705,replyId,0,8),500,0);f.s.poll(1000,0);
  assert(f.radio.sends==1&&f.s.storageStopSafe());assert(!results(f.h,100)&&!telemetries(f.h,100));}
 group("explicit RF ACK selector generates reliable telemetry control; matching reply-ID ACK releases it without user delivery or ACK loop");

 {Fixture f;f.s.measuredUptime(77);auto p=selector();p[12]=8;f.radio.sendStatus=-EBUSY;f.rx(p,100,700);f.s.poll(400,0);assert(f.radio.sends==1);
  const auto first=f.radio.frames[0];f.radio.sendStatus=0;f.s.poll(650,0);assert(f.radio.sends==2&&f.radio.frames[1]==first);decodeReply(f,100,77,true,2);f.done(651);
  f.rx(f.wire(PEER,OURS,705,bench::le32(first.data()+8)),700,0);assert(f.s.storageStopSafe());}
 group("reliable telemetry busy-send retry retains exact RF ID/ciphertext and completes only on matching ACK");
 {Fixture f;f.s.measuredUptime(77);auto p=selector();p[12]=8;f.rx(p,100,700);f.s.poll(400,0);decodeReply(f,100,77,true);const auto first=f.radio.frames[0];auto id=bench::le32(first.data()+8);f.done(401);
  f.rx(f.wire(PEER+1,OURS,705,id),500,0);assert(!f.s.storageStopSafe());f.rx(f.wire(PEER,OURS,706,id+1),600,0);assert(!f.s.storageStopSafe());
  f.rx(f.wire(PEER,OURS+1,707,id),700,0);assert(!f.s.storageStopSafe());f.s.poll(20000,0);f.s.poll(20300,0);assert(f.radio.sends==2&&f.radio.frames[1]==first);f.done(20301);
  f.rx(f.wire(PEER,OURS,708,id,0,8),21000,0);f.s.poll(22000,0);assert(f.radio.sends==2&&f.s.storageStopSafe()&&!results(f.h,100)&&!telemetries(f.h,100));}
 group("wrong peer/request/destination ACK cannot release telemetry control; retry and exact ACK preserve no ACK loop or user result");
 {Fixture f;f.s.measuredUptime(77);auto p=selector();p[12]=8;f.rx(p,100,700);f.s.poll(400,0);decodeReply(f,100,77,true);const auto first=f.radio.frames[0];f.done(401);
  f.s.poll(20000,0);f.s.poll(20300,0);assert(f.radio.sends==2&&f.radio.frames[1]==first);f.done(20301);
  f.s.poll(40000,0);f.s.poll(40300,0);assert(f.radio.sends==3&&f.radio.frames[2]==first);f.done(40301);
  f.s.poll(60000,0);f.s.poll(60300,0);assert(f.radio.sends==4&&f.radio.frames[3]==first);f.done(60301);
  f.s.poll(80000,0);f.s.poll(80300,0);assert(f.radio.sends==5&&f.radio.frames[4]==first);f.done(80301);
  f.s.poll(100000,0);f.s.poll(101000,0);assert(f.radio.sends==5&&f.s.storageStopSafe());}
 group("lost ACK permits at most five total byte-identical directed telemetry attempts and then frees control storage");

 // Public LocalStats shares the existing reply admission, control queue and rate record.
 for(auto model:{81,127})for(bool ack:{false,true}){
  Fixture f(true,model);f.s.measuredUptime(123);const auto before=f.s.config;
  auto query=rawFrame({8,67,18,2,0x32,0,24,1,72,2},PEER,OURS,100,ack?8:0);
  f.rx(query,100,700);f.s.poll(400,0);assert(f.radio.sends==1);
  const auto &wire=f.radio.frames[0];bench::RouteMetadata m{};
  assert(f.s.codec.decode(wire.data(),wire.size(),m)&&m.portnum==67&&m.request_id==100&&m.hasBitfield&&!m.bitfield);
  assert(bool(wire[12]&8)==ack);meshtastic_Telemetry t{};auto in=pb_istream_from_buffer(m.payload,m.payloadLength);
  assert(pb_decode(&in,meshtastic_Telemetry_fields,&t)&&!in.bytes_left&&t.which_variant==meshtastic_Telemetry_local_stats_tag);
  assert(t.variant.local_stats.uptime_seconds==123&&t.variant.local_stats.num_total_nodes==1&&!t.time);
  assert(!t.variant.local_stats.heap_total_bytes&&!t.variant.local_stats.heap_free_bytes&&!t.variant.local_stats.noise_floor);
  f.done(401);f.rx(query,600,701);f.s.poll(1000,0);assert(f.radio.sends==1&&f.s.config.same(before));
 }
 group("Public LocalStats: both models, canonical variant, exact request ID, actual nodes, ACK policy and replay suppression");
 {Fixture f;f.s.measuredUptime(123);f.rx(rawFrame({8,67,18,2,0x32,0,24,1,72,2},PEER,OURS,100),100,700);
  f.s.poll(400,0);assert(f.s.localStats().packetsTx==0);f.done(401);assert(f.s.localStats().packetsTx==1);
  f.s.creditTxPending();assert(f.s.localStats().packetsTx==1);
  f.rx(rawFrame({8,67,18,2,0x32,0,24,1,72,2},PEER,OURS,101),5200,701);f.s.poll(5500,0);
  const auto &wire=f.radio.frames.back();bench::RouteMetadata m{};assert(f.s.codec.decode(wire.data(),wire.size(),m));
  meshtastic_Telemetry t{};auto in=pb_istream_from_buffer(m.payload,m.payloadLength);assert(pb_decode(&in,meshtastic_Telemetry_fields,&t));
  assert(t.variant.local_stats.num_packets_tx==1);f.radio.doneStatus=-EIO;f.done(5501);assert(f.s.localStats().packetsTx==1);
 }
 group("LocalStats TX counts only known clean native completions once; current send and failed completion are not credited");

}


