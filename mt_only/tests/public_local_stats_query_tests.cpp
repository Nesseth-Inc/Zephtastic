// SPDX-License-Identifier: GPL-3.0-only
#include "telemetry_query_fixture.h"

static meshtastic_ToRadio statsQuery(uint32_t id=77,unsigned hop=3) {
  auto q=query(id,PEER,hop);q.packet.decoded.payload.size=2;
  q.packet.decoded.payload.bytes[0]=0x32;q.packet.decoded.payload.bytes[1]=0;
  return q;
}
static meshtastic_Telemetry stats() {
  meshtastic_Telemetry t{};t.time=UINT32_MAX;t.which_variant=meshtastic_Telemetry_local_stats_tag;
  t.variant.local_stats={UINT32_MAX,100.0f,100.0f,UINT32_MAX,UINT32_MAX,UINT32_MAX,
    UINT16_MAX,UINT16_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT16_MAX,INT32_MIN};
  return t;
}
static std::vector<uint8_t> statsFrame(uint32_t request=77,uint32_t packet=700) {
  return rawFrame(encodeData(encodeTelemetry(stats()),request),PEER,OURS,packet);
}
static void beginStats(Fixture &f,unsigned owner=0,uint32_t now=100) {
  input(owner?f.bc:f.c,statsQuery(),now);f.s.poll(now+200,0);
  assert(f.radio.sends==1);f.done(now+201);
}
int main() {
  std::cout.setf(std::ios::unitbuf);
  for(auto model:{81,127})for(unsigned owner=0;owner<2;++owner)for(unsigned hop=0;hop<=3;++hop) {
    Fixture f(true,model);const auto before=f.s.config;
    input(owner?f.bc:f.c,statsQuery(77,hop),100);f.s.poll(300,0);assert(f.radio.frames.size()==1);
    const auto &wire=f.radio.frames.front();bench::RouteMetadata m;
    assert(f.s.codec.decode(wire.data(),wire.size(),m));
    assert(wire.size()==26&&m.portnum==67&&m.wantResponse&&m.hasBitfield&&m.bitfield==2);
    assert(m.payloadLength==2&&m.payload[0]==0x32&&m.payload[1]==0);
    assert(wire[12]==uint8_t(hop|(hop<<5))&&bench::le32(wire.data()+4)==OURS);
    f.done(301);f.rx(f.wire(PEER,OURS,699,77,0),400);f.clients();
    assert(!telemetries(f.h)&&!telemetries(f.bh)&&!results(f.h,77)&&!results(f.bh,77));
    f.rx(statsFrame(),500);f.clients();std::vector<uint8_t> body;
    assert(telemetries(owner?f.bh:f.h,77,&body)==1&&body==encodeTelemetry(stats()));
    assert(!telemetries(owner?f.h:f.bh)&&results(owner?f.bh:f.h,77,0)==1);
    assert(!memcmp(before.bytes,f.s.config.bytes,sizeof(before.bytes))&&!f.s.nodes.count());
    f.rx(statsFrame(),600);f.clients();assert(telemetries(owner?f.bh:f.h)==1&&f.radio.sends==1);
  }
  group("public LocalStats both models owners hops: exact encrypted selector, distinct RF/request IDs, maximum snapshot delivered once only to requester");
  {
    Fixture f;beginStats(f);f.rx(goodFrame(),450);f.clients();assert(!telemetries(f.h)&&!results(f.h,77));
    f.rx(statsFrame(),500);f.clients();assert(telemetries(f.h)==1);
    Fixture metrics;beginQuery(metrics);metrics.rx(statsFrame(),450);metrics.clients();assert(!telemetries(metrics.h)&&!results(metrics.h,77));
    metrics.rx(goodFrame(),500);metrics.clients();assert(telemetries(metrics.h)==1);
    Fixture known;mt::NodeCache::Entry peer{};peer.identity(PEER,81,0);
    assert(mt::NodeCache::Entry::text(peer.bytes+mt::NodeCache::Entry::Name,40,"Known peer",11));
    assert(mt::NodeCache::Entry::text(peer.bytes+mt::NodeCache::Entry::Short,5,"KP",3));
    const int slot=known.s.nodes.accept(peer,OURS,3,0);assert(slot>=0);
    mt::PeerDeviceMetrics cached;assert(cached.assign(fullTelemetry().variant.device_metrics));
    assert(known.s.nodes.observeDeviceMetrics(PEER,cached));
    const auto before=known.s.nodes.get(unsigned(slot))->deviceMetrics;
    beginStats(known);known.rx(statsFrame(),500);known.clients();
    assert(telemetries(known.h)==1&&known.s.nodes.count()==1&&
      !memcmp(&before,&known.s.nodes.get(unsigned(slot))->deviceMetrics,sizeof(before)));
  }
  group("public DeviceMetrics and LocalStats queries remain isolated and LocalStats preserves a known peer metrics cache");
  {
    for(unsigned mode=0;mode<9;++mode) {
      Fixture f;beginStats(f);auto frame=statsFrame();
      if(mode==0)frame=statsFrame(78);
      if(mode==1)frame=rawFrame(encodeData(encodeTelemetry(stats())),0x98761234);
      if(mode==2)frame=rawFrame(encodeData(encodeTelemetry(stats())),PEER,0xffffffffu);
      if(mode==3)frame[13]^=1;
      if(mode==4)frame[12]=1;
      if(mode==5)frame=rawFrame(encodeData(encodeTelemetry(stats()),77,67,true));
      if(mode==6)frame=rawFrame(encodeData(encodeTelemetry(stats()),77,3));
      if(mode==7){auto body=encodeTelemetry(stats());body.insert(body.end(),{0xf8,1,1});frame=rawFrame(encodeData(body));}
      if(mode==8){auto data=encodeData(encodeTelemetry(stats()));data.insert(data.end(),{8,67});frame=rawFrame(data);}
      f.rx(frame,450);f.clients();assert(!telemetries(f.h)&&!results(f.h,77));
      f.rx(statsFrame(77,701),500);f.clients();assert(telemetries(f.h)==1);
    }
  }
  group("wrong peer destination request channel hop envelope and noncanonical response cannot retire LocalStats query");
  {
    for(const auto &body:std::vector<std::vector<uint8_t>>{{0x32},{0x32,0,0x32,0},{8,0,0x32,0},{0x32,2,8,1}}) {
      Fixture f;auto q=statsQuery();q.packet.decoded.payload.size=body.size();
      memcpy(q.packet.decoded.payload.bytes,body.data(),body.size());input(f.c,q,100);f.s.poll(300,0);f.clients();
      assert(!f.radio.sends&&results(f.h,77,meshtastic_Routing_Error_BAD_REQUEST)==1);
    }
    Fixture ack;auto q=statsQuery();q.packet.want_ack=true;input(ack.c,q,100);ack.s.poll(300,0);ack.clients();
    assert(!ack.radio.sends&&results(ack.h,77,meshtastic_Routing_Error_BAD_REQUEST)==1);
  }
  group("LocalStats requires exact empty selector and retains public RF ACK refusal");
  {
    Fixture shared;input(shared.c,statsQuery(),100);input(shared.bc,statsQuery(),100);shared.s.poll(300,0);shared.done(301);
    shared.c.resetStream();shared.rx(statsFrame(),500);shared.clients();
    assert(!telemetries(shared.h)&&telemetries(shared.bh)==1&&shared.radio.sends==1);
    Fixture f;input(f.c,statsQuery(),100);input(f.bc,query(),100);f.s.poll(300,0);f.done(301);f.rx(statsFrame(),500);f.clients();
    assert(telemetries(f.h)==1&&!telemetries(f.bh)&&results(f.bh,77,meshtastic_Routing_Error_BAD_REQUEST)==1);
  }
  group("shared query owner disconnect preserves remaining owner; same-ID different query kind cannot join");
  {
    Fixture expired;beginStats(expired);expired.rx(statsFrame(),90100);expired.clients();
    assert(!telemetries(expired.h)&&results(expired.h,77,3)==1&&expired.radio.sends==1);
    Fixture disarmed;beginStats(disarmed);assert(disarmed.s.armEndpoint(false));disarmed.rx(statsFrame(),500);
    assert(!telemetries(disarmed.h));
    Fixture early;input(early.c,statsQuery(),100);early.rx(statsFrame(),150);assert(!telemetries(early.h));
    early.s.poll(300,0);early.done(301);early.rx(statsFrame(77,701),500);assert(telemetries(early.h)==1);
  }
  group("LocalStats preserves query expiry disarm pretransmit refusal and no RF retransmission after timeout");
  {
    Fixture f;uint8_t out[26];memset(out,0xa5,sizeof(out));size_t size=99;
    assert(!f.s.codec.encodeTelemetryQuery(OURS,PEER,77,out,25,size,true)&&size==0);
    for(auto b:out)assert(b==0xa5);
    assert(!f.s.codec.encodeTelemetryQuery(OURS,OURS,77,out,sizeof(out),size,true));
    assert(!f.s.codec.encodeTelemetryQuery(OURS,PEER,0,out,sizeof(out),size,true));
    assert(f.s.codec.encodeTelemetryQuery(OURS,PEER,77,out,sizeof(out),size,true));
    bench::RouteMetadata m;assert(f.s.codec.decode(out,size,m)&&m.payload[0]==0x32);
    assert(f.s.codec.encodeTelemetryQuery(OURS,PEER,78,out,sizeof(out),size));
    assert(f.s.codec.decode(out,size,m)&&m.payload[0]==0x12);
  }
  group("public codec capacity destination ID guards unchanged and existing callers still select DeviceMetrics");
  {
    Fixture f;f.s.measuredUptime(1234);
    f.rx(rawFrame({8,67,18,2,0x32,0,24,1,72,2},PEER,OURS,701),100);
    f.s.poll(400,0);assert(f.radio.sends==1&&f.radio.frames.size()==1);
    bench::RouteMetadata m;const auto&wire=f.radio.frames[0];
    assert(f.s.codec.decode(wire.data(),wire.size(),m)&&m.portnum==67&&m.request_id==701&&!m.wantResponse&&
      !(wire[12]&8)&&bench::le32(wire.data())==PEER&&bench::le32(wire.data()+4)==OURS&&bench::le32(wire.data()+8)!=701);
    meshtastic_Telemetry expected{};expected.which_variant=meshtastic_Telemetry_local_stats_tag;
    expected.variant.local_stats.uptime_seconds=1234;expected.variant.local_stats.num_total_nodes=1;
    const auto payload=encodeTelemetry(expected);
    assert(m.payloadLength==payload.size()&&!memcmp(m.payload,payload.data(),payload.size()));
  }
  group("Public LocalStats responder returns measured uptime and NodeDB count without fabricating unavailable fields");
  return 0;
}
