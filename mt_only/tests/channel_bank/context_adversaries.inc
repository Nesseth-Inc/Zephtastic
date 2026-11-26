// SPDX-License-Identifier: GPL-3.0-only
// SOURCE-ONLY PROPOSAL: insert after WireSide/received in channel_bank_cases.cpp.
// Not compiled or executed. RecordingPrivate proves no durable nonce/private crypto.
namespace m95_early_proposal {
using Bytes=std::vector<uint8_t>;
static Bytes trace(WireSide &s,unsigned slot,uint32_t from,uint32_t to,uint32_t id,
                   unsigned remaining,const uint8_t *payload=nullptr,size_t size=0) {
  mt::Service::ChannelScope selected(s.service,slot);assert(selected.valid());
  uint8_t data[239]{},wire[255]{};size_t n=0,length=0;
  assert(mt::TraceRoute::requestData(payload,size,data,sizeof(data),n));
  assert(s.service.codec.encodeTraceRouteRequest(from,to,id,data,n,wire,sizeof(wire),length));
  wire[12]=uint8_t(remaining|(3u<<5));return {wire,wire+length};
}
static bench::Frame queued(WireSide &s,uint32_t now){auto *p=s.service.route.next(now);assert(p);return *p;}
static void sameQueued(const bench::Frame &a,const bench::Frame &b){assert(a.len==b.len&&!memcmp(a.bytes,b.bytes,a.len)&&a.dueAt==b.dueAt&&a.attempts==b.attempts);}
static void traceCollision() {
  auto s=std::make_unique<WireSide>(Node,true);s->service.stop();auto cfg=s->service.config;
  uint8_t a[16]{},b[16]{};a[0]=1;b[1]=1;mt::ChannelRecord ca,cb;
  assert(ca.set(2,"Twin",a,sizeof(a))&&cb.set(2,"Twin",b,sizeof(b)));
  assert(cfg.setSecondaryChannel(6,ca)&&cfg.setSecondaryChannel(7,cb)&&cfg.setDeviceRole(2,0));
  assert(s->service.apply(cfg)&&s->service.start(false,0)&&s->service.armEndpoint(true));
  mt::PrimaryChannel::Selection ka,kb;assert(cfg.channelSelection(6,ka)&&cfg.channelSelection(7,kb));
  assert(ka.hash==kb.hash&&ka.length==kb.length&&memcmp(ka.bytes,kb.bytes,ka.length));
  mt::PrimaryChannel::clear(ka);mt::PrimaryChannel::clear(kb);
  constexpr uint32_t origin=0x87654321,destination=0x33445566,id=1401;
  auto first=trace(*s,6,origin,destination,id,1);s->service.rx(first.data(),first.size(),3,1000,0);
  assert(s->service.route.pending()==1);const auto original=queued(*s,1001);const auto upgrades=s->service.route.stats.upgrades;
  // Same outer IDs/header hash/initial semantics, a higher remaining-hop path,
  // but a DIFFERENT effective key. It must not take over the existing queue.
  auto wrong=trace(*s,7,origin,destination,id,2);s->service.rx(wrong.data(),wrong.size(),3,1010,0);
  sameQueued(original,queued(*s,1011));assert(s->service.route.stats.upgrades==upgrades);
  // Encode the associated outgoing route (which already contains us) under
  // the other colliding key. It must not gain a legitimate loop observation.
  bench::RouteMetadata m;
  {mt::Service::ChannelScope selected(s->service,6);assert(selected.valid()&&s->service.codec.decode(original.bytes,original.len,m));}
  const auto loops=s->service.traceLoopObservations;
  auto wrongLoop=trace(*s,7,origin,destination,id,2,m.payload,m.payloadLength);
  s->service.rx(wrongLoop.data(),wrongLoop.size(),3,1020,0);
  assert(s->service.traceLoopObservations==loops);sameQueued(original,queued(*s,1021));
  auto goodLoop=trace(*s,6,origin,destination,id,2,m.payload,m.payloadLength);
  s->service.rx(goodLoop.data(),goodLoop.size(),3,1030,0);assert(s->service.traceLoopObservations==loops+1);
  // Positive control: the same-key higher-hop variant still upgrades normally.
  auto good=trace(*s,6,origin,destination,id,2);s->service.rx(good.data(),good.size(),3,1040,0);
  assert(s->service.route.stats.upgrades==upgrades+1&&(queued(*s,1041).bytes[12]&7)==1);
  assert(s->radio.frames.empty()&&s->service.selectedChannel()==0);
}
// Actual unsigned discovery path: normalDiscovery=true, no private fake needed.
struct DiscoverySide {
  mt::test::KnownChannelFixtureClock knownChannelClock;
  WireRadio radio;mt::Service service;WireHost host;mt::companion::CompanionWorkspace workspace;mt::Companion companion;
  DiscoverySide():service(radio,true,Node,81,true,true),companion(service,host,81,workspace,0){
    knownChannelClock.bind(service);
    auto cfg=service.config;cfg.bytes[4]=1;cfg.seal();assert(cfg.setSecondaryChannel(7,secondary()));
    assert(service.apply(cfg)&&service.start(false,0)&&service.armEndpoint(true));
  }
  Bytes ack(unsigned slot,uint32_t peer,uint32_t id,uint32_t request){
    mt::Service::ChannelScope selected(service,slot);assert(selected.valid());
    uint8_t data[13]={8,5,18,2,24,0,53,0,0,0,0,72,0},wire[255]{};size_t n=0;
    for(unsigned i=0;i<4;++i)data[7+i]=uint8_t(request>>(8*i));
    assert(service.codec.encodeZeroHopData(peer,Node,id,data,sizeof(data),wire,sizeof(wire),n));return {wire,wire+n};
  }
};
static void generatedNodeInfoACK(){
  auto s=std::make_unique<DiscoverySide>();constexpr uint32_t peer=0x87654321;bench::Frame query;
  {mt::Service::ChannelScope selected(s->service,7);assert(selected.valid());const auto record=s->service.channelNode();
   assert(mt::Discovery::encode(s->service.codec,record,peer,81,Node,1501,true,0,query));}
  query.bytes[12]=uint8_t(1|(1u<<5)|8);s->service.rx(query.bytes,query.len,3,1000,700);
  s->service.poll(1300,0);assert(s->radio.frames.size()==1&&s->service.endpointControls()==1);
  const auto response=s->radio.frames[0];assert(response[13]==query.bytes[13]);
  const uint32_t responseId=bench::le32(response.data()+8);s->radio.done=true;s->service.poll(1400,0);
  assert(s->service.endpointControls()==1);auto wrong=s->ack(0,peer,1502,responseId);
  s->service.rx(wrong.data(),wrong.size(),3,1500,0);assert(s->service.endpointControls()==1);
  auto correct=s->ack(7,peer,1503,responseId);s->service.rx(correct.data(),correct.size(),3,1600,0);
  assert(!s->service.endpointControls());s->service.poll(5000,0);assert(s->radio.frames.size()==1&&s->service.selectedChannel()==0);
}
} // namespace m95_early_proposal
// Suggested calls from main after wireCases():
// m95_early_proposal::traceCollision();
// m95_early_proposal::generatedNodeInfoACK();
