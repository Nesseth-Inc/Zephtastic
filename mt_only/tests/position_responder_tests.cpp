// Synthetic coordinates only. Memory-backed radio: no device or RF access.
#include "position_query_fixture.h"
#include "known_channel_fixture.h"
static std::vector<uint8_t> selector(uint32_t from=PEER,uint32_t id=100) {return rawFrame({8,3,24,1,72,2},from,OURS,id);}
static void configure(Fixture &f,bool share=true) {
 f.s.stop();auto c=f.s.config;assert(c.setFixedPosition(123456789,-987654321,true,-35));c.setPositionSharing(share);
 assert(f.s.apply(c)&&f.s.start(false,0)&&f.s.armEndpoint(true));
}
static void check(Fixture &f,uint32_t id=100,bool ack=false,size_t frames=1) {
 assert(f.radio.frames.size()==frames);auto &p=f.radio.frames.back();bench::RouteMetadata m;assert(f.s.codec.decode(p.data(),p.size(),m));
 assert(m.portnum==3&&m.request_id==id&&!m.wantResponse&&m.hasBitfield&&m.bitfield==0&&bool(p[12]&8)==ack);
 meshtastic_Position v{};auto in=pb_istream_from_buffer(m.payload,m.payloadLength);assert(pb_decode(&in,meshtastic_Position_fields,&v));
 assert(v.has_latitude_i&&v.has_longitude_i&&v.latitude_i==123404288&&v.longitude_i==-987693056&&v.has_altitude&&v.altitude==-35);
 assert(v.location_source==meshtastic_Position_LocSource_LOC_MANUAL&&v.precision_bits==15&&!v.time&&!v.timestamp&&!v.fix_quality&&!v.sats_in_view);
}
int main(){std::cout.setf(std::ios::unitbuf);
 for(unsigned version:{4,5,6}) {
  mt::Config c;c.bytes[4]=1;assert(c.setFixedPosition(123456789,-987654321,false,0));
  // Original v4-v6 layout:112 bytes, zero historical tail and CRC at110.
  uint8_t legacy[112]{};memcpy(legacy,c.bytes,97);legacy[2]=uint8_t(version);
  auto sealLegacy=[&](){const auto crc=mt::Config::checksum(legacy,110);legacy[110]=uint8_t(crc);legacy[111]=uint8_t(crc>>8);};
  sealLegacy();mt::Config migrated;
  assert(mt::Config::decode(legacy,sizeof(legacy),migrated)&&migrated.fixedPosition()&&!migrated.positionSharing());
  assert(migrated.latitudeI()==123456789&&migrated.longitudeI()==-987654321);
  const auto before=migrated;legacy[110]^=1;
  assert(!mt::Config::decode(legacy,sizeof(legacy),migrated)&&migrated.same(before));
  legacy[110]^=1;legacy[109]=1;sealLegacy();
  assert(!mt::Config::decode(legacy,sizeof(legacy),migrated)&&migrated.same(before));
  Fixture f;f.s.stop();assert(f.s.apply(migrated)&&f.s.start(false,0)&&f.s.armEndpoint(true));
  f.rx(selector());f.s.poll(1000,0);assert(!f.radio.sends);
 }
 group("v4/v5/v6 stored synthetic coordinates migrate private and cannot produce replies");
 for(auto model:{81,127}){Fixture f(true,model);configure(f);f.rx(selector(),100,700);f.s.poll(400,0);check(f);f.done(401);f.s.poll(20000,0);assert(f.radio.sends==1);}
 group("both models emit correlated manual position only after explicit sharing; no retry");
 for(unsigned mode=0;mode<11;++mode){Fixture f;configure(f);auto p=selector();if(mode==0){f.s.stop();auto c=f.s.config;c.setPositionSharing(false);assert(f.s.apply(c)&&f.s.start(false,0));}if(mode==1)f.s.armEndpoint(false);if(mode==2)p=rawFrame({8,3,24,1,72,2},PEER,0xffffffffu);if(mode==3)p[13]^=1;if(mode==4)p[12]=0x10;if(mode==5)p[12]=0x23;if(mode==6)p=rawFrame({8,3,24,1,72,2,8,3});if(mode==7)p=rawFrame({8,3,18,0,24,1,72,0});if(mode==8)p=goodFrame();if(mode==9)p=selector(OURS);if(mode==10)p=rawFrame(encodeData(encodePosition(fullPosition()),0,3,true));f.rx(p);f.s.poll(1000,0);assert(!f.radio.sends);}
 group("disabled/disarmed/broadcast/self/channel/reserved-bit/hop/payload/noncanonical and response-shaped requests do not disclose");
 {Fixture f;configure(f);f.rx(selector(),100,700);f.s.poll(400,0);f.done(401);
  f.rx(selector(PEER,101),5200,701);f.s.poll(5500,0);
  f.rx(selector(PEER+1,102),180099,702);assert(f.s.endpointControls()==0&&f.radio.sends==1);
  f.rx(selector(),180100,703);assert(f.s.endpointControls()==0); // Same request ID still inside replay TTL.
  f.rx(selector(PEER+1,103),180100,704);f.s.poll(180400,0);assert(f.radio.sends==2);}
 group("three-minute global Position reply throttle expires exactly; ten-minute per-ID retention survives that boundary");
 {Fixture f;configure(f);for(unsigned i=0;i<8;++i)f.rx(selector(PEER+i,100+i),100+i,700+i);
  f.s.poll(1000,0);f.done(1100);f.s.poll(1400,0);assert(f.radio.sends==1);}
 group("only one fresh Position response is admitted across a burst from different senders");
 {Fixture f;configure(f);f.rx(selector(),100,700);f.s.stop();auto c=f.s.config;c.setPositionSharing(false);assert(f.s.apply(c)&&f.s.start(false,200)&&f.s.armEndpoint(true));f.s.poll(1000,0);assert(!f.radio.sends);c.setPositionSharing(true);assert(c.setFixedPosition(0,0,false,0)&&!c.positionSharing());c.setPositionSharing(true);c.clearFixedPosition();assert(!c.positionSharing());}
 group("revocation cancels unsent replies; replacing/removing coordinates revokes consent");
 {mt::Config c;c.setFixedPosition(0,0,false,0);c.setPositionSharing(true);uint8_t b[64];size_t n;assert(mt::PositionQuery::fixedResponse(c,1,b,sizeof(b),n));mt::Config restored;assert(mt::Config::decode(c.bytes,c.Size,restored)&&restored.positionSharing());c.bytes[102]=2;c.seal();assert(!mt::Config::decode(c.bytes,c.Size,restored));}
 group("explicit sharing round-trips in current settings layout, zero coordinates supported, invalid flag rejected");
 {Fixture requester;beginQuery(requester);Radio radio;mt::test::KnownChannelFixtureClock knownChannelResponder;mt::Service responder(radio,true,PEER,127,true,false);knownChannelResponder.bind(responder);mt::Config cfg;cfg.bytes[4]=1;cfg.setFixedPosition(123456789,-987654321,false,0);cfg.setPositionSharing(true);assert(responder.apply(cfg)&&responder.start(false,0)&&responder.armEndpoint(true));const auto &q=requester.radio.frames[0];responder.rx(q.data(),q.size(),3,400,700);responder.poll(800,0);assert(radio.frames.size()==1);requester.rx(radio.frames[0],1000);requester.clients();assert(positions(requester.h)==1&&results(requester.h,77,0));requester.rx(radio.frames[0],1100);assert(positions(requester.h)==1);}
 group("two actual services complete query once with synthetic fixed-position responder");

 {Fixture f;configure(f);f.rx(selector(),100,700);f.s.poll(30200,0);assert(!f.radio.sends);}
 group("unsent response expires at the captured thirty-second normal queue bound without transmission");
 {Fixture f;configure(f);mt::Endpoint endpoint;endpoint.positionConfig(f.s.config);
  endpoint.positionReplyAccepted(UINT32_MAX-150);
  assert(!endpoint.positionReplyAllowed(UINT32_MAX-149)&&!endpoint.positionReplyAllowed(179848));
  assert(endpoint.positionReplyAllowed(179849));
  endpoint.positionReplyAccepted(0);assert(!endpoint.positionReplyAllowed(0)&&!endpoint.positionReplyAllowed(179999)&&endpoint.positionReplyAllowed(180000));}
 group("Position reply window is exact through tick wrap and a successful admission at tick zero");
 {Fixture f;mt::Config c=f.s.config;c.setFixedPosition(123,456,false,0);c.setPositionSharing(true);mt::Endpoint e;e.positionConfig(c);auto p=selector();bench::RouteMetadata m;assert(f.s.codec.decode(p.data(),p.size(),m));e.receive(f.s.codec,f.s.route,OURS,p.data(),p.size(),m,3,100,700,true);assert(!e.idle());c.setPositionSharing(false);e.positionConfig(c);assert(e.idle()&&!e.next(1000));}
 group("direct config publication revokes queued location data without relying on radio stop");
 for(auto model:{81,127}){Fixture f(true,model);configure(f);auto p=selector();p[12]=8;f.rx(p,100,700);f.s.poll(400,0);check(f,100,true);
  const auto reply=f.radio.frames[0];const auto replyId=bench::le32(reply.data()+8);f.done(401);assert(!f.s.storageStopSafe());
  f.rx(f.wire(PEER,OURS,705,replyId,0,8),500,0);f.s.poll(1000,0);
  assert(f.radio.sends==1&&f.s.storageStopSafe());assert(!results(f.h,100)&&!positions(f.h,100));}
 group("explicit RF ACK position selector preserves consent and reliable control; reply-ID ACK releases without user delivery or ACK loop");

 {Fixture f;configure(f);auto p=selector();p[12]=8;f.radio.sendStatus=-EBUSY;f.rx(p,100,700);f.s.poll(400,0);assert(f.radio.sends==1);
  const auto first=f.radio.frames[0];f.radio.sendStatus=0;f.s.poll(650,0);assert(f.radio.sends==2&&f.radio.frames[1]==first);check(f,100,true,2);f.done(651);
  f.rx(f.wire(PEER,OURS,705,bench::le32(first.data()+8)),700,0);assert(f.s.storageStopSafe());}
 group("reliable position busy-send retry preserves privacy-limited payload and exact RF ID/ciphertext");
 {Fixture f;configure(f);auto p=selector();p[12]=8;f.rx(p,100,700);f.s.poll(400,0);check(f,100,true);const auto first=f.radio.frames[0];auto id=bench::le32(first.data()+8);
  f.rx(p,450,701);assert(f.radio.sends==1);f.done(451);f.rx(p,600,702);f.s.poll(1000,0);assert(f.radio.sends==1&&!f.s.storageStopSafe());
  f.rx(f.wire(PEER,OURS,705,id),1100,0);f.s.poll(1200,0);assert(f.radio.sends==1&&f.s.storageStopSafe()&&!positions(f.h,100));}
 group("duplicate position selector during active TX and ACK wait cannot allocate another reply");
 {Fixture f;configure(f);auto p=selector();p[12]=8;f.rx(p,100,700);f.s.poll(400,0);check(f,100,true);const auto first=f.radio.frames[0];f.done(401);
  f.s.poll(20000,0);f.s.poll(20300,0);assert(f.radio.sends==2&&f.radio.frames[1]==first);f.done(20301);
  f.s.poll(40000,0);f.s.poll(40300,0);assert(f.radio.sends==3&&f.radio.frames[2]==first);f.done(40301);
  f.s.poll(60000,0);f.s.poll(60300,0);assert(f.radio.sends==4&&f.radio.frames[3]==first);f.done(60301);
  f.s.poll(80000,0);f.s.poll(80300,0);assert(f.radio.sends==5&&f.radio.frames[4]==first);f.done(80301);
  f.s.poll(100000,0);f.s.poll(110000,0);assert(f.radio.sends==5&&f.s.storageStopSafe());}
 group("lost ACK permits five total byte-identical directed-unicast position attempts then frees control storage");
 {Fixture f;auto c=f.s.config;assert(c.setFixedPosition(123,456,false,0));c.setPositionSharing(true);mt::Endpoint e;e.positionConfig(c);auto p=selector();p[12]=8;bench::RouteMetadata m;assert(f.s.codec.decode(p.data(),p.size(),m));
  e.receive(f.s.codec,f.s.route,OURS,p.data(),p.size(),m,3,100,700,true);auto *j=e.next(400);assert(j);e.started(*j,0,400,100,0);e.complete(*j,401,0);assert(!e.idle());
  c.setPositionSharing(false);e.positionConfig(c);assert(e.idle()&&!e.next(20000));c.setPositionSharing(true);e.positionConfig(c);e.receive(f.s.codec,f.s.route,OURS,p.data(),p.size(),m,3,600,701,true);assert(e.idle());}
 group("revocation cancels a position reply waiting for ACK and retains replay suppression after sharing is re-enabled");
 {Fixture f;auto c=f.s.config;assert(c.setFixedPosition(123,456,false,0));c.setPositionSharing(true);mt::Endpoint e;e.positionConfig(c);auto p=selector();p[12]=8;bench::RouteMetadata m;assert(f.s.codec.decode(p.data(),p.size(),m));
  e.receive(f.s.codec,f.s.route,OURS,p.data(),p.size(),m,3,100,700,true);auto *j=e.next(400);assert(j);e.started(*j,0,400,100,0);const auto frame=j->frame;
  c.setPositionSharing(false);e.positionConfig(c);assert(!e.idle()&&j->frame.len==frame.len&&!memcmp(j->frame.bytes,frame.bytes,frame.len));e.complete(*j,401,0);assert(e.idle());e.tick(20000,0);assert(!e.next(20300));}
 group("revocation retains an active position TX buffer until completion and suppresses all later retries");

}

