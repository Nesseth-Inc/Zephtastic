// SPDX-License-Identifier: GPL-3.0-only
#include "telemetry_query_fixture.h"
#include <limits>
int main() {
  std::cout.setf(std::ios::unitbuf);
  for(auto model:{81,127})for(unsigned owner=0;owner<2;++owner) {
    Fixture f(true,model);beginQuery(f,100,owner);auto before=f.s.config;
    bench::RouteMetadata m;assert(f.s.codec.decode(f.radio.frames[0].data(),f.radio.frames[0].size(),m));
    assert(m.portnum==67&&m.wantResponse&&m.payloadLength==2&&m.payload[0]==18&&m.payload[1]==0&&m.hasBitfield&&m.bitfield==2);
    assert((f.radio.frames[0][12]&8)==0&&bench::le32(f.radio.frames[0].data()+4)==OURS);
    f.rx(f.wire(PEER,OURS,699,77,0),400);f.clients();assert(!results(f.h,77)&&!results(f.bh,77));
    f.rx(goodFrame(),500);f.clients();std::vector<uint8_t> body;
    assert(telemetries(owner?f.bh:f.h,77,&body)==1&&telemetries(owner?f.h:f.bh)==0&&body==encodeTelemetry(fullTelemetry()));
    assert(!memcmp(before.bytes,f.s.config.bytes,sizeof(before.bytes))&&!f.s.nodes.count()&&!f.s.route.routeCount());
    f.rx(goodFrame(),600);assert(telemetries(owner?f.bh:f.h)==1&&f.radio.sends==1);
  }
  group("both models and owners: cold-cache query normalized, ACK ignored, full remote metrics preserved once without local state mutation");
  {
    for(unsigned mask=0;mask<32;++mask){Fixture f;auto q=query();auto t=fullTelemetry();t.time=0;auto &m=t.variant.device_metrics;
      m.has_battery_level=mask&1;m.has_voltage=mask&2;m.has_channel_utilization=mask&4;m.has_air_util_tx=mask&8;m.has_uptime_seconds=mask&16;
      auto p=encodeTelemetry(t);q.packet.decoded.payload.size=p.size();memcpy(q.packet.decoded.payload.bytes,p.data(),p.size());
      input(f.c,q,100);f.s.poll(300,0);assert(f.radio.sends==1);bench::RouteMetadata wire;
      assert(f.s.codec.decode(f.radio.frames[0].data(),f.radio.frames[0].size(),wire));assert(wire.payloadLength==2&&wire.payload[0]==18&&wire.payload[1]==0);
    }
  }
  group("all cached DeviceMetrics presence combinations become exactly empty selector; no client values leak");
  {
    for(unsigned mask=1;mask<32;++mask){Fixture f;beginQuery(f);auto t=fullTelemetry();t.time=0;auto &m=t.variant.device_metrics;
      m.has_battery_level=mask&1;m.battery_level=0;m.has_voltage=mask&2;m.voltage=0;
      m.has_channel_utilization=mask&4;m.channel_utilization=0;m.has_air_util_tx=mask&8;m.air_util_tx=0;m.has_uptime_seconds=mask&16;m.uptime_seconds=0;
      auto payload=encodeTelemetry(t);f.rx(rawFrame(encodeData(payload)),500);std::vector<uint8_t> out;assert(telemetries(f.h,77,&out)==1&&out==payload);
    }
    for(uint32_t battery:{100u,101u,102u,0xffffffffu}){Fixture f;beginQuery(f);auto t=fullTelemetry();t.variant.device_metrics.battery_level=battery;f.rx(rawFrame(encodeData(encodeTelemetry(t))),500);assert(telemetries(f.h)==1);}
  }
  group("optional presence and present zeros retained; protocol powered battery values above100 preserved");
  {
    std::vector<meshtastic_Telemetry> bad;meshtastic_Telemetry empty{};bad.push_back(empty);empty.which_variant=meshtastic_Telemetry_device_metrics_tag;bad.push_back(empty);
    for(auto val:{-1.0f,std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
      auto t=fullTelemetry();t.variant.device_metrics.voltage=val;bad.push_back(t);
      t=fullTelemetry();t.variant.device_metrics.channel_utilization=val;bad.push_back(t);
      t=fullTelemetry();t.variant.device_metrics.air_util_tx=val;bad.push_back(t);
    }
    for(unsigned field=0;field<2;++field){auto t=fullTelemetry();if(field)t.variant.device_metrics.air_util_tx=100.01f;else t.variant.device_metrics.channel_utilization=100.01f;bad.push_back(t);}
    auto env=fullTelemetry();env.which_variant=meshtastic_Telemetry_environment_metrics_tag;env.variant={};bad.push_back(env);
    for(const auto &t:bad){Fixture f;beginQuery(f);f.rx(rawFrame(encodeData(encodeTelemetry(t))),500);assert(!telemetries(f.h)&&!results(f.h,77));f.rx(goodFrame(),600);assert(telemetries(f.h)==1);}
  }
  group("empty selectors, other modules, negative/nonfinite metrics and out-of-range percentages cannot complete query");
  {
    for(float voltage:{-0.0f,std::numeric_limits<float>::denorm_min(),std::numeric_limits<float>::min(),std::numeric_limits<float>::max()}){
      auto t=fullTelemetry();t.time=0;auto &m=t.variant.device_metrics;
      m.voltage=voltage;m.channel_utilization=100.0f;m.air_util_tx=-0.0f;
      auto payload=encodeTelemetry(t);assert(mt::TelemetryQuery::request(payload.data(),payload.size()));
      Fixture f;beginQuery(f);f.rx(rawFrame(encodeData(payload)),500);std::vector<uint8_t> out;
      assert(telemetries(f.h,77,&out)==1&&out==payload);
    }
    for(float invalid:{std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
      for(unsigned field=0;field<3;++field){auto t=fullTelemetry();t.time=0;auto &m=t.variant.device_metrics;
        if(field==0)m.voltage=invalid;
        if(field==1)m.channel_utilization=invalid;
        if(field==2)m.air_util_tx=invalid;
        auto payload=encodeTelemetry(t);assert(!mt::TelemetryQuery::request(payload.data(),payload.size()));
      }
    }
  }
  group("finite float boundaries and signed zero preserved; nonfinite cached requests rejected for every metric");
  {
    auto body=encodeTelemetry(fullTelemetry());std::vector<std::vector<uint8_t>> bad;auto v=body;v.pop_back();bad.push_back(v);
    v=body;v.insert(v.end(),{0xf8,1,1});bad.push_back(v);v=body;v.insert(v.end(),body.begin(),body.end());bad.push_back(v);
    v=body;v.insert(v.end(),{8,0});bad.push_back(v);
    for(const auto &p:bad){Fixture f;beginQuery(f);f.rx(rawFrame(encodeData(p)),500);assert(!telemetries(f.h));f.rx(goodFrame(),600);assert(telemetries(f.h)==1);}
    auto d=encodeData(body);d.insert(d.end(),{8,67});Fixture f;beginQuery(f);f.rx(rawFrame(d),500);assert(!telemetries(f.h));f.rx(goodFrame(),600);assert(telemetries(f.h)==1);
  }
  group("malformed, unknown, duplicate and noncanonical Telemetry/Data rejected before client emission, pending request retained");
  {
    for(unsigned mode=0;mode<8;++mode){Fixture f;beginQuery(f);auto v=goodFrame();
      if(mode==0)v=rawFrame(encodeData(encodeTelemetry(fullTelemetry())),0x98761234);
      if(mode==1)v=rawFrame(encodeData(encodeTelemetry(fullTelemetry())),PEER,0xffffffffu);
      if(mode==2)v=rawFrame(encodeData(encodeTelemetry(fullTelemetry()),78));
      if(mode==3)v[13]^=1;
      if(mode==4)v[12]=1;
      if(mode==5)v=rawFrame(encodeData(encodeTelemetry(fullTelemetry()),77,67,true));
      if(mode==6)v=rawFrame(encodeData(encodeTelemetry(fullTelemetry()),77,3));
      if(mode==7)v=rawFrame(encodeData(encodeTelemetry(fullTelemetry())),PEER,0x88888888);
      f.rx(v,500);f.clients();assert(!telemetries(f.h)&&!results(f.h,77));f.rx(goodFrame(77,701),600);assert(telemetries(f.h)==1);
    }
    Fixture f;auto p=query();p.packet.decoded.portnum=meshtastic_PortNum_POSITION_APP;input(f.c,p,100);f.s.poll(300,0);f.done(301);
    f.rx(goodFrame(),500);f.clients();assert(!telemetries(f.h)&&!results(f.h,77));
  }
  group("peer, destination, request, channel, hop and query-kind isolation before callback");
  {
    for(unsigned mode=0;mode<18;++mode){Fixture f;auto r=query();auto &p=r.packet;
      switch(mode){case 0:p.to=0xffffffffu;break;case 1:p.to=OURS;break;case 2:p.to=1;break;case 3:p.id=0;break;
      case 4:p.hop_limit=8;break;case 5:p.want_ack=true;break;case 6:p.decoded.want_response=false;break;
      case 7:p.decoded.payload.size=2;p.decoded.payload.bytes[0]=0x18;p.decoded.payload.bytes[1]=0;break;
      case 8:p.pki_encrypted=true;break;case 9:p.channel=1;break;case 10:p.from=PEER;break;
      case 11:p.decoded.request_id=2;break;case 12:p.decoded.has_bitfield=true;break;case 13:p.pki_encrypted=true;p.public_key.size=32;break;
      case 14:p.ack_proof_status=static_cast<meshtastic_MeshPacket_AckProofStatus>(255);break;
      case 15:p.decoded.payload.size=4;memcpy(p.decoded.payload.bytes,"\x08\x01\x12\x00",4);break;
      case 16:p.decoded.payload.size=4;memcpy(p.decoded.payload.bytes,"\x12\x00\x12\x00",4);break;
      case 17:p.decoded.reply_id=5;break;}
      input(f.c,r,100);f.s.poll(300,0);f.clients();assert(!f.radio.sends&&results(f.h,p.id)>0);
    }
  }
  group("bounded hint-only public-key lengths preserve canonical selector query bytes for USB and BLE");
  {
    for(size_t hint:{size_t(1),size_t(31),size_t(32)})for(bool bluetooth:{false,true}){
      Fixture baseline;auto plain=query();input(bluetooth?baseline.bc:baseline.c,plain,100);baseline.s.poll(300,0);
      assert(baseline.radio.sends==1&&baseline.radio.frames.size()==1);
      Fixture hinted;auto request=query();request.packet.public_key.size=hint;
      memset(request.packet.public_key.bytes,0x5a,hint);
      input(bluetooth?hinted.bc:hinted.c,request,100);hinted.s.poll(300,0);
      assert(hinted.radio.sends==1&&hinted.radio.frames.size()==1&&
        hinted.radio.frames[0]==baseline.radio.frames[0]);
      bench::RouteMetadata metadata;
      assert(hinted.s.codec.decode(hinted.radio.frames[0].data(),hinted.radio.frames[0].size(),metadata));
      assert(metadata.portnum==67&&metadata.wantResponse&&metadata.payloadLength==2&&
        metadata.payload[0]==18&&metadata.payload[1]==0);
    }
  }
  group("request envelope rejects publishing, private/broadcast/self/reserved destinations, timestamps, other variants and duplicate selector");
  {
    Fixture f;input(f.c,query(),100);f.rx(goodFrame(),150);assert(!telemetries(f.h));f.s.poll(300,0);f.done(301);f.rx(goodFrame(),500);assert(telemetries(f.h)==1);
    Fixture timed;beginQuery(timed);timed.rx(goodFrame(),90100);timed.clients();assert(!telemetries(timed.h)&&results(timed.h,77,3)==1&&timed.radio.sends==1);
    Fixture wrap;const uint32_t start=0xfffffff0u;wrap.s.stop();assert(wrap.s.start(false,start-100)&&wrap.s.armEndpoint(true));beginQuery(wrap,start);wrap.rx(goodFrame(),start+500);assert(telemetries(wrap.h)==1);
    // Explicit fixture airtime is 100 ms. RadioTiming resolves driver=10000
    // (minimum), host=11000; PacketLifetime therefore gives queue=max(11000+
    // 10000,30000)=30000 ms. The packet was admitted at t=100, so it remains
    // live at age 29999 and expires at the exact 30000 ms boundary.
    Fixture queued;queued.radio.occupied=true;input(queued.c,query(),100);queued.s.poll(300,0);
    queued.s.poll(30099,0);queued.clients();assert(!queued.radio.sends&&!results(queued.h,77));
    queued.s.poll(30100,0);queued.clients();assert(!queued.radio.sends&&results(queued.h,77,3)==1);
  }
  group("pre-transmit,90s overall/airtime-derived queue expiry and uptime wrap preserve bounded behavior without retransmission");
  {
    Fixture f;input(f.c,query(),100);input(f.bc,query(),100);f.s.poll(300,0);f.done(301);f.rx(goodFrame(),500);assert(f.radio.sends==1&&telemetries(f.h)==1&&telemetries(f.bh)==1);
    input(f.c,query(),600);f.s.poll(900,0);assert(f.radio.sends==1&&telemetries(f.h)==1);
    Fixture full;for(uint32_t i=1;i<=5;++i)input(full.c,query(i),100);assert(full.s.endpointFree()==0&&results(full.h,5,meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED)==1);
    input(full.c,query(1,0x88888888),100);assert(results(full.h,1,32)==1);
  }
  group("shared owners, queue capacity, changed ID collision and completed-ID no retransmit");
  {
    Fixture f;beginQuery(f);f.c.resetStream();f.rx(goodFrame(),500);assert(!telemetries(f.h));
    Fixture shared;input(shared.c,query(),100);input(shared.bc,query(),100);shared.s.poll(300,0);shared.done(301);shared.c.resetStream();shared.rx(goodFrame(),500);assert(!telemetries(shared.h)&&telemetries(shared.bh)==1);
    Fixture cancel;beginQuery(cancel);assert(cancel.s.armEndpoint(false));cancel.rx(goodFrame(),500);assert(!telemetries(cancel.h));
  }
  group("disconnect and disarm cancel ownership without stealing remaining client response");
  {
    for(unsigned mode=0;mode<3;++mode){Fixture f(mode!=0);if(mode==1)assert(f.s.armEndpoint(false));if(mode==2){f.radio.healthStatus=-EIO;f.s.poll(50,0);}
      input(f.c,query(),100);f.s.poll(300,0);f.clients();assert(!f.radio.sends&&results(f.h,77,4)>0);}
    for(unsigned flag:{0u,1u,2u,3u,4u,255u}){Fixture f;beginQuery(f);auto d=encodeData(encodeTelemetry(fullTelemetry()));d.back()=uint8_t(flag);f.rx(rawFrame(d),500);assert(telemetries(f.h)==unsigned(flag<=1));}
  }
  group("RX-only, disarm and faults refuse; MQTT permission retained without permitting response/unknown bit flags");
  {
    for(uint32_t code:{8u,38u,255u,256u,65536u,0x7fffffffu}){
      Fixture f;beginQuery(f);std::vector<uint8_t> payload{24};auto value=code;
      do{payload.push_back(uint8_t((value&127)|(value>127?128:0)));value>>=7;}while(value);
      f.rx(rawFrame(encodeData(payload,77,5)),500);f.clients();assert(results(f.h,77,(code<=9||(code>=32&&code<=39))?int(code):2)==1);
      f.rx(goodFrame(),600);assert(!telemetries(f.h));
    }
    Fixture f;beginQuery(f);f.rx(f.wire(0x99999999,OURS,701,77,8),450);f.clients();assert(!results(f.h,77));f.rx(goodFrame(),600);assert(telemetries(f.h)==1);
  }
  group("peer NAK terminates, unknown wide NAK maps safely, unrelated NAK cannot cancel");
  {
    Fixture f;f.rx(rawFrame(encodeData({18,0},0,67,true)),100);f.s.poll(400,0);assert(!f.radio.sends);
    Fixture busy;busy.radio.sendStatus=-EBUSY;input(busy.c,query(),100);busy.s.poll(300,0);busy.radio.sendStatus=0;busy.s.poll(1000,0);assert(busy.radio.sends==2);busy.done(1001);busy.rx(goodFrame(),1200);assert(telemetries(busy.h)==1);
    mt::RadioTiming::Deadline timeout;assert(mt::RadioTiming::resolve(100,timeout));
    for(unsigned mode=0;mode<4;++mode){Fixture x;if(mode==0)x.radio.sendStatus=-EIO;input(x.c,query(),100);x.s.poll(300,0);
      if(mode==1){x.radio.doneStatus=-EIO;x.done(301);}if(mode==2){x.radio.restoreStatus=-EIO;x.done(301);}if(mode==3)x.s.poll(300+timeout.hostMs,0);
      x.clients();assert(x.s.faulted()&&results(x.h,77,4)==1&&!telemetries(x.h));x.rx(goodFrame(),300+timeout.hostMs+100);assert(x.radio.sends==1&&!telemetries(x.h));}
  }
  group("no automatic responder; native busy retry and radio send/completion/restore/timeout faults handled");
  return 0;
}
