// SPDX-License-Identifier: GPL-3.0-only
// Independent SOURCE-ONLY proposal for frozen M95 9dfc3aa75bd55aee60931b99d2cc9025bf481197.
// Include after WireSide/received in channel_bank_cases.cpp, then call each
// function individually from main. NOT COMPILED OR EXECUTED BY THIS REVIEWER.
// Tests use synthetic keys/peers/radio only. No private crypto or durable lease claim.
namespace m95_full_review {
using Bytes=std::vector<uint8_t>;
static std::unique_ptr<WireSide> router() {
  auto s=std::make_unique<WireSide>(Node);s->service.stop();auto c=s->service.config;
  assert(c.setDeviceRole(2,0));
  assert(s->service.apply(c)&&s->service.start(false,0)&&s->service.armEndpoint(true));
  assert(s->service.relaying()&&!s->service.sameChannel(0,7));return s;
}
static Bytes data(WireSide &s,unsigned slot,uint32_t from,uint32_t to,uint32_t id,
                  const uint8_t *plain,size_t size,unsigned hop=1) {
  mt::Service::ChannelScope selected(s.service,slot);assert(selected.valid());
  uint8_t wire[255]{};size_t n=0;
  assert(s.service.codec.encodeZeroHopData(from,to,id,plain,size,wire,sizeof(wire),n));
  wire[12]=uint8_t(hop|(hop<<5));return {wire,wire+n};
}
static void known(WireSide &s,uint32_t peer) {
  bench::Frame f;const auto c=s.service.channelNode();
  assert(mt::Discovery::encode(s.service.codec,c,peer,81,0xffffffffu,1600,false,0,f));
  f.bytes[12]=uint8_t(1|(1<<5));s.service.rx(f.bytes,f.len,3,1000,0);
  assert(s.service.route.knownCount()==1);s.service.route.clearPending();
}
static void crossChannelTransitResponseMustNotCancel() {
  auto s=router();constexpr uint32_t peer=0x87654321,origin=0x33445566,id=1601;
  known(*s,peer);constexpr uint8_t text[]={8,1,18,1,'x',72,0};
  const auto request=data(*s,0,origin,peer,id,text,sizeof(text));
  s->service.rx(request.data(),request.size(),3,1100,0);
  assert(s->service.route.hasQueued(origin,id));
  uint8_t ack[]={8,5,18,2,24,0,53,0,0,0,0,72,0};
  for(unsigned i=0;i<4;++i)ack[7+i]=uint8_t(id>>(8*i));
  const auto wrong=data(*s,7,peer,origin,1602,ack,sizeof(ack),0);
  const auto cancels=s->service.route.routingStats.responseCancels;
  s->service.rx(wrong.data(),wrong.size(),3,1110,0);
  // Fails at frozen head: controlResponse cancels by to/request_id with no channel check.
  assert(s->service.route.hasQueued(origin,id));
  assert(s->service.route.routingStats.responseCancels==cancels);
  const auto correct=data(*s,0,peer,origin,1603,ack,sizeof(ack),0);
  s->service.rx(correct.data(),correct.size(),3,1120,0);
  assert(!s->service.route.hasQueued(origin,id));
  assert(s->service.route.routingStats.responseCancels==cancels+1);
}
static void crossChannelOriginRepeatMustNotReplace() {
  auto s=router();constexpr uint32_t peer=0x87654321,origin=0x33445566,id=1611;
  constexpr uint8_t text[]={8,1,18,1,'x',72,0};
  const auto first=data(*s,0,origin,peer,id,text,sizeof(text));
  s->service.rx(first.data(),first.size(),3,1100,0);
  const auto *queued=s->service.route.next(1101);assert(queued);const auto original=*queued;
  const auto wrong=data(*s,7,origin,peer,id,text,sizeof(text),2);
  s->service.rx(wrong.data(),wrong.size(),3,1110,0);
  queued=s->service.route.next(1111);assert(queued);
  // Same effective AES key but different hash/name is still a different channel.
  assert(queued->len==original.len&&!memcmp(queued->bytes,original.bytes,original.len));
}
static void primaryChangePreservesEncryptedHashZeroSecondary() {
  mt::Config c;mt::ChannelRecord inherited;
  assert(inherited.set(2,"BatchLink",nullptr,0)&&c.setSecondaryChannel(7,inherited));
  mt::PrimaryChannel::Selection before;assert(c.channelSelection(7,before));mt::PrimaryChannel::clear(before);
  // XOR("BatchLink") == 124; XOR("LongFast") == 10. This leaves primary
  // hash 118 valid and yields an encrypted secondary hash of zero.
  uint8_t key[16]{};key[0]=124;mt::PrimaryChannel::Selection primary;
  assert(mt::PrimaryChannel::normalize(key,sizeof(key),primary,c.effectivePrimaryName()));
  auto record=c.node();record.setKey(primary.hash,primary.bytes,primary.length);mt::PrimaryChannel::clear(primary);c.node(record);
  mt::PrimaryChannel::Selection after;assert(c.channelSelection(7,after));
  assert(after.hash==0&&after.length==sizeof(key)&&!after.licensedPlaintext&&!after.publicDefault&&
    !memcmp(after.bytes,key,sizeof(key)));mt::PrimaryChannel::clear(after);
  assert(c.valid());
}
static void customSecondaryCanLoadBeforePrivateRuntimeBind() {
  WireRadio radio;mt::Service service(radio,true,Node,81,true,false);
  auto c=service.config;c.bytes[4]=1;c.seal();assert(c.setSecondaryChannel(7,secondary("BootLink",2)));
  mt::PrimaryChannel::Selection key;assert(c.channelSelection(7,key)&&!key.publicDefault);mt::PrimaryChannel::clear(key);
  // Actual initializeApplication loads/applies Config before activate() binds
  // the prepared private runtime. apply is a no-RF staging operation.
  assert(service.apply(c)); // Fails at frozen head due solely to !private_.
  assert(!service.start(false,0)); // No provider must still mean no RF authority.
  RecordingPrivate provider(Node);assert(service.bindPrivateRuntime(provider));
  assert(service.start(false,0)&&service.armEndpoint(true));
  assert(!radio.sends); // This fake proves composition only, never PKI readiness.
}
} // namespace m95_full_review
