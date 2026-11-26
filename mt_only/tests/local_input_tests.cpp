// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include <array>

static void basicText() {
  Fixture fixture;
  const uint8_t text[]={'H','i',7};
  uint32_t id=0;
  assert(!fixture.s.submitLocalInput(meshtastic_PortNum_TEXT_MESSAGE_APP,text,sizeof(text),
    0,true,100,501,id));
  assert(id==501&&fixture.s.endpointFree()==3&&!fixture.radio.sends);
  fixture.s.disconnect(0);fixture.s.disconnect(1);
  assert(fixture.s.endpointFree()==3);
  fixture.s.poll(300,0);
  assert(fixture.radio.frames.size()==1);
  const auto &frame=fixture.radio.frames.front();
  bench::RouteMetadata decoded;
  assert(fixture.s.codec.decode(frame.data(),frame.size(),decoded));
  assert(decoded.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP&&decoded.payloadLength==sizeof(text));
  assert(!memcmp(decoded.payload,text,sizeof(text))&&decoded.hasBitfield&&!decoded.bitfield);
  assert(bench::le32(frame.data())==UINT32_MAX&&bench::le32(frame.data()+4)==OURS);
  assert(bench::le32(frame.data()+8)==id&&!(frame[12]&8));
  assert((frame[12]&7)==fixture.s.config.hopLimit());
  assert(!fixture.s.submitMessage(0,frame.data(),frame.size(),true,350));
  fixture.s.disconnect(0);
  fixture.done(400);
  auto echo=frame;
  echo[12]=uint8_t((echo[12]&0xf8u)|((echo[12]&7u)-1u));
  echo[15]=uint8_t(PEER);
  fixture.rx(echo,500);
  mt::Endpoint::Result result;
  assert(fixture.s.takeLocalInputResult(result)&&result.id==id&&result.kind==mt::Endpoint::Kind::Relay);
  assert(!fixture.s.takeLocalInputResult(result));
  assert(!fixture.s.takeMessageResult(0,result)&&!fixture.s.takeMessageResult(1,result));
  fixture.clients();
  assert(!results(fixture.h,id)&&!results(fixture.bh,id));
  group("local text uses the ordinary broadcast queue without another client's completion events");
}

static void serialBoundary() {
  Fixture fixture;
  std::array<uint8_t,233> bytes{};
  for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t(i);
  uint32_t id=0;
  assert(fixture.s.submitLocalInput(meshtastic_PortNum_SERIAL_APP,bytes.data(),bytes.size(),
    0,false,100,502,id)==-EMSGSIZE&&!id&&fixture.s.endpointFree()==4);
  assert(!fixture.s.submitLocalInput(meshtastic_PortNum_SERIAL_APP,bytes.data(),232,
    0,false,100,503,id));
  fixture.s.poll(300,0);
  assert(fixture.radio.frames.size()==1);
  const auto &frame=fixture.radio.frames.front();
  bench::RouteMetadata decoded;
  assert(fixture.s.codec.decode(frame.data(),frame.size(),decoded));
  assert(frame.size()==255&&decoded.portnum==meshtastic_PortNum_SERIAL_APP);
  assert(decoded.payloadLength==232&&!memcmp(decoded.payload,bytes.data(),232));
  assert(decoded.hasBitfield&&!decoded.bitfield&&mt::PublicApplication::received(
    fixture.s.codec,frame.data(),frame.size(),decoded));
  uint8_t data[239]{};size_t size=0;
  assert(mt::PublicApplication::encode(64,bytes.data(),232,0,0,data,sizeof(data),size));
  assert(size==239&&mt::PublicApplication::canonical(data,size));
  assert(!mt::PublicApplication::encode(64,bytes.data(),233,0,0,data,sizeof(data),size));
  fixture.done(400);
  mt::Endpoint::Result result;
  assert(fixture.s.takeLocalInputResult(result)&&result.id==id&&result.kind==mt::Endpoint::Kind::Local);
  group("stock 232-byte serial RF boundary includes the present zero MQTT bitfield; 233 raw bytes reject before queueing");
}

static void admissionAndChannels() {
  Fixture fixture;
  mt::ChannelRecord channel;
  const uint8_t alias=1;
  assert(channel.set(2,"serial",&alias,1));
  auto config=fixture.s.config;
  assert(config.setSecondaryChannel(1,channel));
  fixture.s.stop();
  assert(fixture.s.apply(config)&&fixture.s.start(false,0)&&fixture.s.armEndpoint(true));
  const uint8_t text[]={'o','k'};
  uint32_t id=123;
  assert(fixture.s.submitLocalInput(5,text,2,0,false,100,1,id)==-EINVAL&&!id);
  assert(fixture.s.submitLocalInput(1,nullptr,2,0,false,100,1,id)==-EINVAL&&!id);
  assert(fixture.s.submitLocalInput(1,text,0,0,false,100,1,id)==-EINVAL&&!id);
  assert(fixture.s.submitLocalInput(1,text,2,8,false,100,1,id)==-EINVAL&&!id);
  assert(fixture.s.submitLocalInput(1,text,2,7,false,100,1,id)==-ENODEV&&!id);
  assert(fixture.s.selectedChannel()==0&&fixture.s.endpointFree()==4);
  assert(!fixture.s.submitLocalInput(64,text,2,1,false,100,503,id));
  assert(fixture.s.selectedChannel()==0);
  for(uint32_t next=504;next<507;++next)
    assert(!fixture.s.submitLocalInput(1,text,2,0,false,100,next,id));
  assert(fixture.s.endpointFree()==0);
  assert(fixture.s.submitLocalInput(1,text,2,0,false,100,507,id)==-ENOBUFS&&!id);
  assert(!fixture.radio.sends);
  Fixture disabled(false);
  assert(disabled.s.submitLocalInput(1,text,2,0,false,100,508,id)==-EACCES&&!id);
  group("invalid input, unavailable channels, disarmed radio and full queues reject before RF and restore channel selection");
}

static void serialReceiveDispatch() {
  Fixture fixture;
  struct SerialHost:Host {
    unsigned packets=0;
    explicit SerialHost(mt::Service &service):Host(service){}
    void peripheralPacket(uint32_t from,uint32_t to,uint32_t id,unsigned port,unsigned channel,
                          const uint8_t *bytes,size_t size,bool local)override {
      assert(from==PEER&&to==UINT32_MAX&&id==701&&port==64&&channel==0&&!local);
      assert(size==3&&bytes[0]==0&&bytes[1]==0xff&&bytes[2]==0x80);
      ++packets;
    }
  } host(fixture.s);
  mt::companion::CompanionWorkspace workspace;mt::Companion companion(fixture.s,host,127,workspace);
  const uint8_t bytes[]={0,0xff,0x80};
  uint8_t data[239]{},frame[255]{};
  size_t dataSize=0,frameSize=0;
  assert(mt::PublicApplication::encode(64,bytes,sizeof(bytes),0,0,data,sizeof(data),dataSize));
  assert(fixture.s.codec.encodeZeroHopData(PEER,UINT32_MAX,701,data,dataSize,frame,sizeof(frame),frameSize));
  fixture.s.rx(frame,frameSize,3,500,700);
  companion.receive(frame,frameSize,-90,3,500,mt::Companion::WorkspaceProof{true});
  companion.receive(frame,frameSize,-90,3,501,mt::Companion::WorkspaceProof{true});
  assert(host.packets==1&&!fixture.radio.sends);
  group("stock-shaped zero-hop serial data reaches the admitted peripheral callback once with binary bytes intact");
}

static void localPhoneCopy() {
  Fixture fixture;
  const uint8_t text[]={'H','i'};
  uint8_t data[239]{};
  size_t size=0;
  assert(mt::Service::encodeLocalInputData(1,text,sizeof(text),data,sizeof(data),size));
  assert(fixture.c.modulePacket(data,size,801,2,mt::Companion::WorkspaceProof{true}));
  assert(fixture.bc.modulePacket(data,size,801,2,mt::Companion::WorkspaceProof{true}));
  for(const auto *host:{&fixture.h,&fixture.bh}) {
    const auto &row=host->out.back();
    assert(row.which_payload_variant==meshtastic_FromRadio_packet_tag);
    assert(row.packet.from==OURS&&row.packet.to==UINT32_MAX&&row.packet.id==801&&row.packet.channel==2);
    assert(row.packet.decoded.portnum==1&&row.packet.decoded.payload.size==2);
    assert(!row.packet.pki_encrypted&&!row.packet.xeddsa_signed);
  }
  assert(!fixture.c.modulePacket(data,size,802,8,mt::Companion::WorkspaceProof{true})&&!fixture.radio.sends);
  group("accepted local input has a decoded USB/BLE phone copy with its actual selected channel");
}

static void localUiOwnerLane() {
  mt::Endpoint endpoint;
  auto frame=[](uint32_t id) {
    bench::Frame value{};auto put=[](uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));};
    put(value.bytes,PEER);put(value.bytes+4,OURS);put(value.bytes+8,id);
    value.bytes[16]=0x5a;value.len=17;return value;
  };
  auto first=frame(901),other=frame(902),changed=frame(901);changed.bytes[16]^=1;
  assert(endpoint.submit(mt::Endpoint::NoClientOwner,first.bytes,first.len,false,100,false,true,false,nullptr,true)==-EINVAL);
  assert(endpoint.submit(0xfd,first.bytes,first.len,false,100,false,true,false,nullptr,true)==-EINVAL);
  assert(endpoint.submit(mt::Endpoint::LocalUiOwner,first.bytes,first.len,false,100)==-EINVAL);
  assert(!endpoint.submit(mt::Endpoint::LocalUiOwner,first.bytes,first.len,false,100,false,true,false,nullptr,true));
  assert(!endpoint.submit(mt::Endpoint::LocalUiOwner,first.bytes,first.len,false,101,false,true,false,nullptr,true));
  assert(endpoint.submit(mt::Endpoint::LocalUiOwner,changed.bytes,changed.len,false,102,false,true,false,nullptr,true)==-EINVAL);
  assert(endpoint.submit(mt::Endpoint::LocalUiOwner,other.bytes,other.len,false,103,false,true,false,nullptr,true)==-EBUSY);
  auto *job=endpoint.next(200);assert(job&&bench::le32(job->frame.bytes+8)==901);
  endpoint.started(*job,0,200,1,0);endpoint.complete(*job,201,0);
  mt::Endpoint::Result result;
  assert(endpoint.takeLocalUiResult(result)&&result.id==901&&result.kind==mt::Endpoint::Kind::Local);
  assert(!endpoint.takeLocalUiResult(result)&&!endpoint.take(0,result)&&!endpoint.take(1,result));
  // A rejected overlapping request did not reserve its ID in Endpoint's local nonce filter.
  assert(!endpoint.submit(mt::Endpoint::LocalUiOwner,other.bytes,other.len,false,300,false,true,false,nullptr,true));
  endpoint.cancel(7);assert(endpoint.takeLocalUiResult(result)&&result.id==902&&result.kind==mt::Endpoint::Kind::Failed);
  assert(!endpoint.takeLocalUiResult(result)&&!endpoint.take(0,result)&&!endpoint.take(1,result));
  auto authenticated=frame(903);
  assert(!endpoint.submit(mt::Endpoint::LocalUiOwner,authenticated.bytes,authenticated.len,true,400,false,true,false,nullptr,true));
  job=endpoint.next(500);assert(job);endpoint.started(*job,0,500,1,0);endpoint.complete(*job,501,0);
  endpoint.authenticated(903,PEER,0);
  assert(endpoint.invalidatePrivatePeer(PEER));
  assert(endpoint.takeLocalUiResult(result)&&result.id==903&&result.kind==mt::Endpoint::Kind::Failed&&result.code==4&&!result.peer);
  auto revoked=frame(904);
  assert(!endpoint.submit(mt::Endpoint::LocalUiOwner,revoked.bytes,revoked.len,true,600,false,true,false,nullptr,true));
  job=endpoint.next(700);assert(job);endpoint.started(*job,0,700,1,0);endpoint.complete(*job,701,0);
  endpoint.authenticated(904,PEER,0);endpoint.cancelPrivate(8);
  assert(endpoint.takeLocalUiResult(result)&&result.id==904&&result.kind==mt::Endpoint::Kind::Failed&&result.code==8&&!result.peer);
  auto old=frame(905),current=frame(906);
  assert(!endpoint.submit(mt::Endpoint::LocalUiOwner,old.bytes,old.len,false,800,false,true,false,nullptr,true));
  job=endpoint.next(900);assert(job);endpoint.started(*job,0,900,1,0);endpoint.complete(*job,901,0);
  assert(endpoint.takeLocalUiResult(result)&&result.id==905);
  assert(!endpoint.submit(mt::Endpoint::LocalUiOwner,current.bytes,current.len,false,1000,false,true,false,nullptr,true));
  assert(endpoint.submit(mt::Endpoint::LocalUiOwner,old.bytes,old.len,false,1001,false,true,false,nullptr,true)==-EBUSY);
  job=endpoint.next(1100);assert(job&&bench::le32(job->frame.bytes+8)==906);
  endpoint.started(*job,0,1100,1,0);endpoint.complete(*job,1101,0);
  assert(endpoint.takeLocalUiResult(result)&&result.id==906&&result.kind==mt::Endpoint::Kind::Local);
  assert(!endpoint.submit(mt::Endpoint::LocalUiOwner,old.bytes,old.len,false,1200,false,true,false,nullptr,true));
  assert(endpoint.takeLocalUiResult(result)&&result.id==905&&result.kind==mt::Endpoint::Kind::Local);
  group("local UI owner stays out of client bitmasks, forbids NoClientOwner private traffic, and preserves one unread terminal result");
}

int main() {
  basicText();
  serialBoundary();
  admissionAndChannels();
  serialReceiveDispatch();
  localPhoneCopy();
  localUiOwnerLane();
}
