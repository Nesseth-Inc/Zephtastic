// SPDX-License-Identifier: GPL-3.0-only
#include "position_query_fixture.h"
int main() {
  std::cout.setf(std::ios::unitbuf);
  for(auto model:{81,127})for(unsigned owner=0;owner<2;++owner) {
    Fixture f(true,model);beginQuery(f,100,owner);auto before=f.s.config;
    bench::RouteMetadata m;assert(f.s.codec.decode(f.radio.frames[0].data(),f.radio.frames[0].size(),m));
    assert(m.portnum==3&&m.wantResponse&&!m.payloadLength&&!m.request_id&&m.hasBitfield&&m.bitfield==2);
    assert((f.radio.frames[0][12]&8)==0&&bench::le32(f.radio.frames[0].data()+4)==OURS);
    f.rx(f.wire(PEER,OURS,699,77,0),400);f.clients();assert(!results(f.h,77)&&!results(f.bh,77));
    f.rx(goodFrame(),500);f.clients();std::vector<uint8_t> body;
    assert(positions(owner?f.bh:f.h,77,&body)==1&&positions(owner?f.h:f.bh)==0&&body==encodePosition(fullPosition()));
    assert(!memcmp(before.bytes,f.s.config.bytes,sizeof(before.bytes))&&!f.s.nodes.count()&&!f.s.route.routeCount());
    f.rx(goodFrame(),600);f.clients();assert(positions(owner?f.bh:f.h)==1);assert(f.radio.sends==1);
  }
  group("four board/owner cases: exact empty query, ACK ignored, full-field response once to owner, no state mutation");
  {
    std::vector<meshtastic_Position> bad;auto p=fullPosition();p.has_latitude_i=false;bad.push_back(p);
    p=fullPosition();p.has_longitude_i=false;bad.push_back(p);p=fullPosition();p.latitude_i=900000001;bad.push_back(p);
    p=fullPosition();p.latitude_i=-900000001;bad.push_back(p);p=fullPosition();p.longitude_i=1800000001;bad.push_back(p);
    p=fullPosition();p.longitude_i=-1800000001;bad.push_back(p);p=fullPosition();p.precision_bits=33;bad.push_back(p);
    p=fullPosition();p.location_source=static_cast<meshtastic_Position_LocSource>(4);bad.push_back(p);
    p=fullPosition();p.altitude_source=static_cast<meshtastic_Position_AltSource>(5);bad.push_back(p);
    for(const auto &v:bad){Fixture f;beginQuery(f);f.rx(rawFrame(encodeData(encodePosition(v))),500);f.clients();assert(!positions(f.h)&&!results(f.h,77));f.rx(goodFrame(),600);assert(positions(f.h)==1);}
  }
  group("invalid coordinates, optional pair, enum domains and precision reject without consuming query");
  {
    std::vector<std::vector<uint8_t>> bad;auto pos=encodePosition(fullPosition());
    bad.push_back({});auto b=pos;b.pop_back();bad.push_back(b);b=pos;b.insert(b.end(),{0x88,0x02,1});bad.push_back(b);
    b=pos;b.insert(b.end(),pos.begin(),pos.begin()+5);bad.push_back(b);b=pos;b.insert(b.end(),{0x20,0});bad.push_back(b);
    b={0x0d,0,0,0,0,0x15,0,0,0,0,0x28,0x81,0};bad.push_back(b);
    for(const auto &v:bad){Fixture f;beginQuery(f);f.rx(rawFrame(encodeData(v)),500);assert(!positions(f.h));f.rx(goodFrame(),600);assert(positions(f.h)==1);}
    auto data=encodeData(pos);std::vector<std::vector<uint8_t>> outer;
    b=data;b.insert(b.end(),{8,3});outer.push_back(b);b=data;b.insert(b.end(),{0xa0,1,1});outer.push_back(b);
    b=data;b.insert(b.end(),{0x25,1,0,0,0});outer.push_back(b);b=data;b.insert(b.end(),{24,1});outer.push_back(b);
    for(const auto &v:outer){Fixture f;beginQuery(f);f.rx(rawFrame(v),500);assert(!positions(f.h));f.rx(goodFrame(),600);assert(positions(f.h)==1);}
  }
  group("malformed/truncated/unknown/duplicate/noncanonical Position and Data reject, then same valid response is accepted");
  {
    for(unsigned mode=0;mode<8;++mode){Fixture f;beginQuery(f);auto v=goodFrame();
      if(mode==0)v=rawFrame(encodeData(encodePosition(fullPosition())),0x98761234);
      if(mode==1)v=rawFrame(encodeData(encodePosition(fullPosition())),PEER,0xffffffffu);
      if(mode==2)v=rawFrame(encodeData(encodePosition(fullPosition()),78));
      if(mode==3)v[13]^=1;
      if(mode==4)v[12]=1;
      if(mode==5)v=rawFrame(encodeData(encodePosition(fullPosition()),77,3,true));
      if(mode==6)v=rawFrame(encodeData(encodePosition(fullPosition()),77,4));
      if(mode==7)v=rawFrame(encodeData(encodePosition(fullPosition())),PEER,0x88888888);
      f.rx(v,500);f.clients();assert(!positions(f.h)&&!results(f.h,77));f.rx(goodFrame(77,701),600);assert(positions(f.h)==1);
    }
  }
  group("wrong peer/destination/request/channel/port/hops/response flags do not complete or consume valid query");
  {
    for(unsigned mode:{0u,1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,14u,15u}){Fixture f;auto r=query();auto &p=r.packet;
      switch(mode){case 0:p.to=0xffffffffu;break;case 1:p.to=OURS;break;case 2:p.to=1;break;case 3:p.id=0;break;
      case 4:p.hop_limit=8;break;case 5:p.want_ack=true;break;case 6:p.decoded.want_response=false;break;
      case 7:p.decoded.payload.size=2;p.decoded.payload.bytes[0]=0x20;p.decoded.payload.bytes[1]=1;break;
      case 8:p.pki_encrypted=true;break;case 9:p.channel=1;break;case 10:p.from=PEER;break;
      case 11:p.decoded.request_id=2;break;case 12:p.decoded.has_bitfield=true;break;
      case 14:p.ack_proof_status=static_cast<meshtastic_MeshPacket_AckProofStatus>(255);break;case 15:p.to=OURS;p.ack_proof_status=static_cast<meshtastic_MeshPacket_AckProofStatus>(255);break;}
      input(f.c,r,100);f.s.poll(300,0);f.clients();assert(!f.radio.sends&&results(f.h,p.id)>0);
    }
    for(unsigned keyLength:{1u,32u}) {
      Fixture baseline,hinted;auto ordinary=query();auto withHint=ordinary;
      withHint.packet.public_key.size=keyLength;memset(withHint.packet.public_key.bytes,0xa5,keyLength);
      input(baseline.c,ordinary,100);input(hinted.c,withHint,100);
      baseline.s.poll(300,0);hinted.s.poll(300,0);
      assert(baseline.radio.frames.size()==1&&hinted.radio.frames==baseline.radio.frames);
      bench::RouteMetadata m;assert(hinted.s.codec.decode(hinted.radio.frames[0].data(),hinted.radio.frames[0].size(),m));
      assert(m.portnum==meshtastic_PortNum_POSITION_APP&&m.wantResponse&&!m.payloadLength&&!m.request_id&&m.hasBitfield&&m.bitfield==2);
      baseline.done(301);hinted.done(301);
      baseline.rx(goodFrame(),500);hinted.rx(goodFrame(),500);baseline.clients();hinted.clients();
      assert(positions(baseline.h)==1&&positions(hinted.h)==1&&positions(baseline.bh)==0&&positions(hinted.bh)==0);
    }
    Fixture f;auto r=query();uint8_t bytes[512]{};auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&out,meshtastic_ToRadio_fields,&r));
    std::vector<uint8_t> dup(bytes,bytes+out.bytes_written);dup.insert(dup.end(),bytes,bytes+out.bytes_written);f.c.packet(dup.data(),dup.size(),100,mt::Companion::WorkspaceProof{true});
    f.s.poll(300,0);assert(!f.radio.sends);
  }
  group("request shape rejects broadcast/self/reserved/zero-ID/ACK/explicit-PKI/coordinates/metadata and duplicate envelopes; key hints preserve exact channel query wire");
  {
    Fixture f;auto r=query();input(f.c,r,100);f.rx(goodFrame(),150);assert(!positions(f.h));f.s.poll(300,0);f.done(301);f.rx(goodFrame(),500);assert(positions(f.h)==1);
    Fixture timed;beginQuery(timed);timed.rx(goodFrame(),90100);timed.clients();assert(!positions(timed.h)&&results(timed.h,77,3)==1&&timed.radio.sends==1);
    Fixture wrap;const uint32_t start=0xfffffff0u;wrap.s.stop();assert(wrap.s.start(false,start-100)&&wrap.s.armEndpoint(true));beginQuery(wrap,start);wrap.rx(goodFrame(),start+500);assert(positions(wrap.h)==1);
  }
  group("pre-transmit response rejected, 90-second deadline expires without retransmit, timer wrap accepts timely response");
  {
    Fixture f;auto r=query();input(f.c,r,100);input(f.bc,r,100);f.s.poll(300,0);f.done(301);f.rx(goodFrame(),500);
    assert(f.radio.sends==1&&positions(f.h)==1&&positions(f.bh)==1);
    Fixture full;for(uint32_t i=1;i<=5;++i){auto q=query(i);input(full.c,q,100);}assert(full.s.endpointFree()==0&&results(full.h,5,meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED)==1);
    auto changed=query(1,0x88888888);input(full.c,changed,100);assert(results(full.h,1,32)==1);
  }
  group("identical active ID joins two owners once; bounded queue capacity and changed-content ID collision reject");
  {
    Fixture f;beginQuery(f);f.c.resetStream();meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=3;
    input(f.c,hello,450);f.h.out.clear();f.rx(goodFrame(),500);assert(!positions(f.h));
    Fixture shared;auto q=query();input(shared.c,q,100);input(shared.bc,q,100);shared.s.poll(300,0);shared.done(301);
    shared.c.resetStream();shared.rx(goodFrame(),500);assert(!positions(shared.h)&&positions(shared.bh)==1);
    Fixture cancel;beginQuery(cancel);assert(cancel.s.armEndpoint(false));cancel.rx(goodFrame(),500);assert(!positions(cancel.h));
  }
  group("disconnect/reconnect cannot inherit old response, remaining owner survives, disarm cancels approval");
  {
    for(unsigned mode=0;mode<3;++mode){Fixture f(mode!=0);if(mode==1)assert(f.s.armEndpoint(false));if(mode==2){f.radio.healthStatus=-EIO;f.s.poll(50,0);}
      input(f.c,query(),100);f.s.poll(300,0);f.clients();assert(!f.radio.sends&&results(f.h,77,4)>0);}
  }
  group("RX-only/disarmed/faulted radio rejects without mock TX");
  {
    Fixture f;beginQuery(f);f.rx(f.wire(0x99999999,OURS,701,77,meshtastic_Routing_Error_NO_RESPONSE),450);f.clients();assert(!results(f.h,77));
    auto malformed=f.wire(PEER,OURS,702,77,meshtastic_Routing_Error_NO_RESPONSE);auto d=std::vector<uint8_t>{8,5,18,2,24,8,53,77,0,0,0,72,0,8,5};
    f.rx(rawFrame(d),500);f.clients();assert(!results(f.h,77));f.rx(f.wire(PEER,OURS,703,77,meshtastic_Routing_Error_NO_RESPONSE),550);f.clients();assert(results(f.h,77,meshtastic_Routing_Error_NO_RESPONSE)==1);
    f.rx(goodFrame(),600);assert(!positions(f.h));
    Fixture unknown;beginQuery(unknown);unknown.rx(unknown.wire(PEER,OURS,704,77,255),500);unknown.clients();assert(results(unknown.h,77,2)==1);
    (void)malformed;
  }
  group("peer NO_RESPONSE and unknown NAK map once; wrong-peer or malformed NAK does not cancel query");
  {
    for(auto lat:{-900000000,0,900000000})for(auto lon:{-1800000000,0,1800000000}){
      Fixture f;beginQuery(f);auto p=fullPosition();p.latitude_i=lat;p.longitude_i=lon;p.precision_bits=32;
      f.rx(rawFrame(encodeData(encodePosition(p))),500);assert(positions(f.h)==1);
    }
  }
  group("coordinate boundaries and present-zero coordinates preserve full payload without fabricated fixes");
  {
    Fixture f;auto p=f.s.config;assert(p.setFixedPosition(123456789,-987654321,true,-25));f.s.stop();assert(f.s.apply(p)&&f.s.start(false,0)&&f.s.armEndpoint(true));
    // Incoming queries never invoke a responder, including on armed devices.
    f.rx(rawFrame(encodeData({},0,3,true)),500);f.s.poll(900,0);assert(!f.radio.sends);
    beginQuery(f,1000);f.rx(goodFrame(),1500);f.clients();input(f.c,query(),1600);f.s.poll(1900,0);assert(f.radio.sends==1);
  }
  group("incoming query has no automatic responder; completed ID replay causes no further transmission");
  {
    for(unsigned flag:{0u,1u,2u,3u,4u,255u}){
      Fixture f;beginQuery(f);auto d=encodeData(encodePosition(fullPosition()));assert(d[d.size()-2]==72);d.back()=uint8_t(flag);
      f.rx(rawFrame(d),500);assert(positions(f.h)==unsigned(flag<=1));
      if(flag>1){f.rx(goodFrame(77,701),600);assert(positions(f.h)==1);}
    }
    for(uint32_t code:{8u,38u,255u,256u,65536u,0x7fffffffu})for(uint8_t mqtt:{uint8_t(0),uint8_t(1)}){
      Fixture f;beginQuery(f);std::vector<uint8_t> payload{24};auto value=code;
      do{payload.push_back(uint8_t((value&127)|(value>127?128:0)));value>>=7;}while(value);
      auto data=encodeData(payload,77,5);data.back()=mqtt;f.rx(rawFrame(data),500);f.clients();
      assert(results(f.h,77,(code<=9||(code>=32&&code<=39))?int(code):2)==1);
    }
  }
  group("stock MQTT permission bit preserved; response request/unknown bits reject; wide positive unknown NAKs never narrow with short enums");
  {
    Fixture busy;busy.radio.sendStatus=-EBUSY;input(busy.c,query(),100);busy.s.poll(300,0);
    assert(busy.radio.sends==1&&!busy.s.faulted());busy.radio.sendStatus=0;busy.s.poll(1000,0);assert(busy.radio.sends==2);
    busy.done(1001);busy.rx(goodFrame(),1200);busy.clients();assert(positions(busy.h)==1&&results(busy.h,77,0)==1&&!busy.s.faulted());
    Fixture capped;capped.radio.sendStatus=-EBUSY;input(capped.c,query(),100);
    for(auto now:{300u,1000u,2000u,3000u})capped.s.poll(now,0);
    capped.clients();assert(capped.radio.sends==3&&results(capped.h,77,5)==1&&!capped.s.faulted());
    Fixture queued;queued.radio.occupied=true;input(queued.c,query(),100);queued.s.poll(90100,0);queued.clients();
    assert(!queued.radio.sends&&results(queued.h,77,3)==1&&!positions(queued.h));
    for(unsigned mode=0;mode<4;++mode){
      Fixture f;if(mode==0)f.radio.sendStatus=-EIO;input(f.c,query(),100);f.s.poll(300,0);
      if(mode==1){f.radio.doneStatus=-EIO;f.done(301);}
      if(mode==2){f.radio.restoreStatus=-EIO;f.done(301);}
      if(mode==3){
        mt::RadioTiming::Deadline timing;assert(mt::RadioTiming::resolve(f.radio.air,timing));
        const uint32_t deadline=300+timing.hostMs;
        f.s.poll(deadline-1,0);assert(!f.s.faulted()&&f.s.active());
        f.s.poll(deadline,0);
      }
      f.clients();assert(f.s.faulted()&&results(f.h,77,4)==1&&!positions(f.h));
      f.rx(goodFrame(),2000);f.s.poll(100000,0);f.clients();assert(f.radio.sends==1&&!positions(f.h)&&results(f.h,77,4)==1);
    }
    for(const auto &payload:std::vector<std::vector<uint8_t>>{{24,0x80,0x80,0x80,0x80,8},{24,0xff,0xff,0xff,0xff,15},{24,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,1}}){
      Fixture f;beginQuery(f);f.rx(rawFrame(encodeData(payload,77,5)),500);f.clients();assert(!results(f.h,77)&&!positions(f.h));
      f.rx(goodFrame(77,701),600);assert(positions(f.h)==1);
    }
  }
  group("position query native busy recovery/cap, queued expiry, send/completion/restore/timeout faults; out-of-domain NAK cannot cancel");
  return 0;
}
