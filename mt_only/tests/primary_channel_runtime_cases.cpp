// SPDX-License-Identifier: GPL-3.0-only
// Actual Service/Endpoint/PkiRuntime plus real PSA; deterministic public keys,
// in-memory ledger and mock radio only. No target or RF result is inferred.
#include "pki_runtime_fixture.h"
#include "gnss/PositionController.h"
#include "modules/GpioModuleWire.h"
#include "meshtastic/remote_hardware.pb.h"
static unsigned groups=0;
static void group(const char *name){++groups;std::cout<<"PASS "<<name<<'\n';}
static void custom(Fixture &f,size_t width=16) {
 f.s.stop();auto cfg=f.s.config;auto record=cfg.node();uint8_t bytes[32]{};bytes[0]=0x51;
 mt::PrimaryChannel::Selection selection;assert(mt::PrimaryChannel::normalize(bytes,width,selection));
 record.setKey(selection.hash,selection.bytes,selection.length);mt::PrimaryChannel::clear(selection);
 cfg.node(record);assert(f.s.apply(cfg)&&f.s.start(false,0)&&f.s.armEndpoint(true));
}
static Bytes publicFrame(Fixture &f,unsigned kind,uint32_t packet=901) {
 uint8_t frame[255]{};size_t n=0;const auto from=f.peer.binding.node,to=f.local.binding.node;
 if(kind==0){auto plain=data("public control ACK");assert(f.s.codec.encodeZeroHopData(from,to,packet,plain.data(),plain.size(),frame,sizeof(frame),n));frame[12]=8;}
 if(kind==1)assert(f.s.codec.encodeTelemetryQuery(from,to,packet,frame,sizeof(frame),n));
 if(kind==2)assert(f.s.codec.encodePositionQuery(from,to,packet,frame,sizeof(frame),n));
 if(kind==3){uint8_t raw[239]{};size_t size=0;assert(mt::TraceRoute::requestData(nullptr,0,raw,sizeof(raw),size));
  assert(f.s.codec.encodeTraceRouteRequest(from,to,packet,raw,size,frame,sizeof(frame),n));}
 return {frame,frame+n};
}
static void assertSentControl(Fixture &f,unsigned port,uint32_t request,bool reserved=true) {
 f.poll(2000);assert(f.radio.frames.size()==1);const auto &wire=f.radio.frames.front();bench::RouteMetadata m;
 assert(f.s.codec.decode(wire.data(),wire.size(),m)&&m.channelDecoded&&m.portnum==port&&m.request_id==request);
 assert(f.ledger.sent(bench::le32(wire.data()+8))==reserved&&wire[13]==f.s.codec.channelHash());
}
int main(){
 for(size_t width:{size_t(16),size_t(32)})for(unsigned kind=0;kind<4;++kind) {
  Fixture f;custom(f,width);if(kind==1)f.s.measuredUptime(1234);
  if(kind==2){f.s.stop();auto c=f.s.config;assert(c.setFixedPosition(0,0,false,0));c.setPositionSharing(true);
   assert(f.s.apply(c)&&f.s.start(false,0)&&f.s.armEndpoint(true));}
  const auto before=f.entropy.calls,stops=f.radio.stops,restores=f.radio.restores;f.rx(publicFrame(f,kind));
  assert(f.entropy.calls==before+1&&f.ledger.sent(81)&&f.s.receiving()&&f.s.endpointArmed()&&!f.radio.sends);
  assert(f.radio.stops==stops+1&&f.radio.restores==restores+1);
  assertSentControl(f,kind==0?5:kind==1?67:kind==2?3:70,901);
  group("custom AES128/AES256 automatic ACK, metrics, position and traceroute controls reserve once before publication");
 }
 for(size_t width:{size_t(16),size_t(32)})for(unsigned kind=0;kind<3;++kind){Fixture f;custom(f,width);uint8_t bytes[255]{};size_t n=0;bool ok=false;
  const auto id=1000+kind,peer=f.peer.binding.node;const auto before=f.entropy.calls,stops=f.radio.stops,restores=f.radio.restores;
  if(kind==0)ok=f.s.encodePositionQuery(peer,id,bytes,sizeof(bytes),n);
  if(kind==1)ok=f.s.encodeTelemetryQuery(peer,id,bytes,sizeof(bytes),n);
  if(kind==2){uint8_t raw[239]{};size_t size=0;assert(mt::TraceRoute::requestData(nullptr,0,raw,sizeof(raw),size));ok=f.s.encodeTraceRouteQuery(peer,id,raw,size,bytes,sizeof(bytes),n);}
  bench::RouteMetadata m;assert(ok&&f.s.codec.decode(bytes,n,m)&&m.wantResponse&&f.ledger.sent(id)&&f.entropy.calls==before+1);
  assert(f.s.receiving()&&!f.radio.sends&&f.radio.stops==stops+1&&f.radio.restores==restores+1);group("canonical custom position, metrics and traceroute queries use dedicated codec helpers and durable ID admission");
 }
 for(size_t width:{size_t(16),size_t(32)}){Fixture f;custom(f,width);uint8_t bytes[26]{};size_t n=0;
  const auto peer=f.peer.binding.node;const auto entropy=f.entropy.calls,stops=f.radio.stops,restores=f.radio.restores;
  assert(f.s.encodeTelemetryQuery(peer,1201,bytes,sizeof(bytes),n,true));bench::RouteMetadata m;
  assert(n==26&&f.s.codec.decode(bytes,n,m)&&m.portnum==67&&m.wantResponse&&m.hasBitfield&&m.bitfield==2&&
    m.payloadLength==2&&m.payload[0]==0x32&&m.payload[1]==0&&f.ledger.sent(1201));
  assert(f.entropy.calls==entropy+1&&f.radio.stops==stops+1&&f.radio.restores==restores+1&&f.s.receiving()&&!f.radio.sends);
  group("custom AES128/AES256 LocalStats request reserves durable ID before exact canonical channel encoding");
 }
 for(size_t width:{size_t(16),size_t(32)})for(uint8_t tag:{2,3,4,5,6,7,8,9,11}){
  Fixture f;custom(f,width);uint8_t bytes[26]{};size_t n=0;
  const auto before=f.entropy.calls;const uint32_t id=2000+tag;
  assert(f.s.encodeTelemetryQuery(f.peer.binding.node,id,bytes,sizeof(bytes),n,tag==6,tag));
  bench::RouteMetadata m{};assert(n==26&&f.s.codec.decode(bytes,n,m)&&m.wantResponse&&m.portnum==67&&m.hasBitfield&&m.bitfield==2);
  assert(m.payloadLength==2&&m.payload[0]==uint8_t((tag<<3)|2)&&!m.payload[1]);
  assert(f.ledger.sent(id)&&f.entropy.calls==before+1&&f.s.receiving()&&!f.radio.sends);
 }
 group("all nine telemetry selectors on AES128/AES256 channels use exact canonical encoding and one durable ID reservation");

 for(unsigned mode=0;mode<5;++mode){Fixture f;custom(f);bench::Frame out;memset(out.bytes,0xa5,sizeof(out.bytes));out.len=123;out.dueAt=17;out.expires=99;out.used=true;out.attempts=3;const auto before=out;
  constexpr uint8_t query[]={8,3,24,1,72,2};const auto stops=f.radio.stops,restores=f.radio.restores,entropy=f.entropy.calls;
  if(mode==0){f.radio.busyValue=true;}
  if(mode==1){f.entropy.fail=true;}
  if(mode==2){f.radio.restoreError=-EIO;}
  if(mode==3){f.lm.fault=2;}
  if(mode==4){f.radio.stopError=-EAGAIN;}
  assert(!f.s.encodeChannelControl(f.peer.binding.node,1100,query,sizeof(query),out));
  assert(!memcmp(out.bytes,before.bytes,sizeof(out.bytes))&&out.len==before.len&&out.dueAt==before.dueAt&&out.expires==before.expires&&out.used==before.used&&out.attempts==before.attempts&&!f.radio.sends);
  if(mode==0)assert(f.radio.stops==stops&&f.radio.restores==restores&&f.entropy.calls==entropy&&f.s.receiving());
  if(mode==1)assert(f.entropy.calls==entropy+1&&f.ledger.sent(1100)&&f.s.receiving()&&!f.s.faulted()&&f.radio.stops==stops+1&&f.radio.restores==restores+1);
  if(mode==2)assert(f.s.faulted()&&!f.s.receiving()&&!f.s.endpointArmed()&&f.radio.stops==stops+2&&f.radio.restores==restores+1);
  if(mode==3)assert(f.s.faulted()&&!f.s.receiving()&&!f.s.endpointArmed()&&f.radio.restores==restores);
  if(mode==4)assert(!f.s.faulted()&&f.s.receiving()&&f.radio.stops==stops+1&&f.radio.restores==restores&&f.entropy.calls==entropy);
  group("busy reservation restore and uncertain-write refusals preserve caller output and prevent RF publication");
 }
 {Fixture f;custom(f);f.entropy.fail=true;f.rx(publicFrame(f,0));assert(f.entropy.calls==1&&f.ledger.sent(81));
  f.poll(2000);assert(!f.radio.sends&&f.s.receiving()&&!f.s.faulted());
  group("automatic ACK reservation failure restores RX without queuing an unreserved fallback");}
 {Fixture f;custom(f);auto wire=f.incoming(902,"private same-domain",true);f.rx(wire);
  assert(f.entropy.calls==1&&f.ledger.sent(81)&&f.delivered(f.usb)==1&&f.delivered(f.ble)==1);
  f.poll(2000);assert(f.radio.sends==1);bench::RouteMetadata m;const auto &ack=f.radio.frames.front();
  assert(f.s.codec.decode(ack.data(),ack.size(),m)&&m.ackProofLength==8);
  group("private ACK on custom channel retains its existing single pause/reservation without entering the public hook");}
 {Fixture f;const auto before=f.entropy.calls;uint8_t frame[22]{};size_t n=0;
  assert(f.s.encodePositionQuery(f.peer.binding.node,1200,frame,sizeof(frame),n)&&n==22&&f.entropy.calls==before&&!f.ledger.sent(1200));
  f.rx(publicFrame(f,0));assert(f.entropy.calls==before);assertSentControl(f,5,901,false);
  group("exact default Public query and automatic ACK retain direct encoding without new ledger reservations");}
 {Fixture f;custom(f);uint8_t frame[21]{};memset(frame,0x42,sizeof(frame));const auto before=f.entropy.calls;size_t n=99;
  assert(!f.s.encodePositionQuery(f.peer.binding.node,1300,frame,sizeof(frame),n)&&!n&&f.entropy.calls==before&&!f.ledger.sent(1300));
  for(auto byte:frame)assert(byte==0x42);
  group("insufficient output capacity is refused before durable reservation");}
 {Fixture f;custom(f);constexpr uint8_t position[]={8,3,24,1,72,2},metrics[]={8,67,18,2,18,0,24,1,72,2};
  bench::Frame out;memset(out.bytes,0x42,sizeof(out.bytes));out.len=123;f.entropy.fail=true;
  assert(!f.s.encodeChannelControl(f.peer.binding.node,1400,position,sizeof(position),out)&&f.ledger.sent(1400));
  const auto entropy=f.entropy.calls;f.entropy.fail=false;
  assert(!f.s.encodeChannelControl(f.peer.binding.node,1400,metrics,sizeof(metrics),out)&&f.entropy.calls==entropy&&out.len==123);
  for(const auto byte:out.bytes)assert(byte==0x42);
  assert(f.s.receiving()&&!f.radio.sends);
  group("burned outgoing ID rejects changed canonical plaintext without new entropy or output mutation");}
 {Fixture f;custom(f);const auto entropy=f.entropy.calls;bench::Frame out;memset(out.bytes,0x42,sizeof(out.bytes));out.len=123;
  std::vector<Bytes> invalid={data("TEXT is not a control"),{8,3,24,1,72,2,53,1,0,0,0},{8,5,24,1,72,0},{8,3,24,1},
    {8,3,24,1,72,2,1},{8,67,18,2,18,0,24,1,72,2,1}};
  for(const auto &bytes:invalid){assert(!f.s.encodeChannelControl(f.peer.binding.node,1500,bytes.data(),bytes.size(),out));
    assert(f.entropy.calls==entropy&&!f.ledger.sent(1500)&&out.len==123);for(const auto byte:out.bytes)assert(byte==0x42);}
  constexpr uint8_t query[]={8,3,24,1,72,2};
  for(uint32_t to:{0u,1u,f.local.binding.node,0xfffffffeu,0xffffffffu})
    assert(!f.s.encodeChannelControl(to,1500,query,sizeof(query),out)&&f.entropy.calls==entropy);
  assert(f.s.receiving()&&!f.radio.sends);
  group("unsupported control port malformed query metadata and nonunicast destinations fail before any reservation");}
 for(size_t width:{size_t(16),size_t(32)}) {
  Fixture f(true);custom(f,width);f.s.stop();auto cfg=f.s.config;
  assert(cfg.setFixedPosition(123456789,-456789012,false,0));cfg.setPositionSharing(true);
  assert(f.s.apply(cfg)&&f.s.start(false,0)&&f.s.armEndpoint(true));
  const auto precision=f.s.config.positionPrecision(0);assert(precision>0);
  const uint32_t id=0x15700123u;const auto entropy=f.entropy.calls;
  assert(!f.ledger.sent(id)&&f.s.announcePosition(1000,id)==0);
  assert(f.ledger.sent(id)&&f.entropy.calls==entropy+1&&f.s.moduleAnnouncementPending()&&!f.radio.sends);
  f.poll(2000);assert(f.radio.sends==1&&f.radio.frames.size()==1);
  const auto &wire=f.radio.frames.front();bench::RouteMetadata m{};
  assert(f.s.codec.decode(wire.data(),wire.size(),m)&&m.channelDecoded&&
    bench::le32(wire.data())==0xffffffffu&&bench::le32(wire.data()+8)==id&&
    wire[12]==uint8_t(f.s.config.hopLimit()|(f.s.config.hopLimit()<<5))&&
    wire[13]==f.s.codec.channelHash()&&m.portnum==meshtastic_PortNum_POSITION_APP&&
    m.request_id==0&&!m.wantResponse&&m.hasBitfield&&!m.bitfield&&m.payloadLength>0&&
    m.signatureLength==64&&m.publicKeyLength==0);
  bench::MtSignature stockGate;assert(stockGate.trustPublicKey(f.local.binding.node,f.local.binding.publicKey,2000));
  assert(stockGate.apply(wire.data(),wire.size(),m,2000)==bench::MtSignature::Result::Verified&&
    bench::MtSignature::nodeId(f.local.binding.publicKey)==f.local.binding.node);
  meshtastic_Position position{};auto pin=pb_istream_from_buffer(m.payload,m.payloadLength);
  assert(pb_decode(&pin,meshtastic_Position_fields,&position)&&!pin.bytes_left&&
    position.has_latitude_i&&position.has_longitude_i&&position.precision_bits==precision);
  meshtastic_Position expected{};assert(mt::PositionQuery::localPosition(f.s.config,nullptr,expected));
  mt::PositionPrecision::apply(expected,precision);
  assert(position.latitude_i==expected.latitude_i&&position.longitude_i==expected.longitude_i);
  f.radio.done=true;f.s.poll(2300,0x27182818u);
  assert(f.s.receiving()&&!f.s.faulted()&&f.radio.sends==1&&f.radio.frames.size()==1);

  uint8_t positionData[64]{};size_t positionSize=0;
  assert(mt::PositionQuery::fixedResponse(f.s.config,0,positionData,sizeof(positionData),positionSize,
    0,nullptr,true)&&positionSize);
  bench::Frame duplicate{};memset(duplicate.bytes,0xa5,sizeof(duplicate.bytes));duplicate.len=117;
  const auto beforeDuplicate=f.entropy.calls;
  assert(!f.s.encodeChannelControl(0xffffffffu,id,positionData,positionSize,duplicate));
  uint8_t sentinel[sizeof(duplicate.bytes)];memset(sentinel,0xa5,sizeof(sentinel));
  assert(f.entropy.calls==beforeDuplicate&&f.ledger.sent(id)&&duplicate.len==117&&
    !memcmp(duplicate.bytes,sentinel,sizeof(sentinel))&&f.radio.sends==1&&f.radio.frames.size()==1);

  // The same request_id=0 Position Data remains valid for a directed custom
  // channel control; this path is encoded but not submitted to the radio.
  bench::Frame directed{};const auto beforeDirected=f.entropy.calls;const uint32_t directedId=id+20;
  assert(f.s.encodeChannelControl(f.peer.binding.node,directedId,positionData,positionSize,directed));
  bench::RouteMetadata directedMeta{};
  assert(f.s.codec.decode(directed.bytes,directed.len,directedMeta)&&
    bench::le32(directed.bytes)==f.peer.binding.node&&bench::le32(directed.bytes+8)==directedId&&
    directedMeta.portnum==meshtastic_PortNum_POSITION_APP&&directedMeta.request_id==0&&
    !directedMeta.wantResponse&&f.ledger.sent(directedId)&&f.entropy.calls==beforeDirected+1&&
    f.radio.sends==1&&f.radio.frames.size()==1);

  auto encodeRawPosition=[](int32_t latitude,uint32_t precision,bool wantResponse,uint8_t out[64],size_t &size){
    size=0;meshtastic_Position p{};p.has_latitude_i=p.has_longitude_i=true;
    p.latitude_i=latitude;p.longitude_i=-456789012;p.precision_bits=precision;
    uint8_t payload[233]{};auto ps=pb_ostream_from_buffer(payload,sizeof(payload));
    if(!pb_encode(&ps,meshtastic_Position_fields,&p))return false;
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_POSITION_APP;d.has_bitfield=true;
    d.want_response=wantResponse;d.payload.size=ps.bytes_written;memcpy(d.payload.bytes,payload,ps.bytes_written);
    auto ds=pb_ostream_from_buffer(out,64);if(!pb_encode(&ds,meshtastic_Data_fields,&d))return false;
    size=ds.bytes_written;return true;
  };
  uint8_t badCoordinates[64]{},badPrecision[64]{},badWantResponse[64]{};
  size_t badCoordinatesSize=0,badPrecisionSize=0,badWantResponseSize=0;
  assert(encodeRawPosition(900000001,precision,false,badCoordinates,badCoordinatesSize));
  assert(encodeRawPosition(123456789,33,false,badPrecision,badPrecisionSize));
  assert(encodeRawPosition(123456789,precision,true,badWantResponse,badWantResponseSize));
  auto refusedPosition=[&](uint32_t badId,const uint8_t *raw,size_t rawSize){
    bench::Frame out{};memset(out.bytes,0x6b,sizeof(out.bytes));out.len=83;
    const auto before=f.entropy.calls;assert(!f.s.encodeChannelControl(0xffffffffu,badId,raw,rawSize,out));
    uint8_t unchanged[sizeof(out.bytes)];memset(unchanged,0x6b,sizeof(unchanged));
    assert(f.entropy.calls==before&&!f.ledger.sent(badId)&&out.len==83&&
      !memcmp(out.bytes,unchanged,sizeof(unchanged)));
  };
  refusedPosition(id+21,badCoordinates,badCoordinatesSize);
  refusedPosition(id+22,badPrecision,badPrecisionSize);
  refusedPosition(id+23,badWantResponse,badWantResponseSize);
  bench::Frame zeroId{};memset(zeroId.bytes,0x3c,sizeof(zeroId.bytes));zeroId.len=71;
  const auto beforeZero=f.entropy.calls;
  assert(!f.s.encodeChannelControl(0xffffffffu,0,positionData,positionSize,zeroId));
  uint8_t zeroUnchanged[sizeof(zeroId.bytes)];memset(zeroUnchanged,0x3c,sizeof(zeroUnchanged));
  assert(f.entropy.calls==beforeZero&&zeroId.len==71&&
    !memcmp(zeroId.bytes,zeroUnchanged,sizeof(zeroUnchanged))&&f.radio.frames.size()==1);

  constexpr uint8_t positionQuery[]={8,3,24,1,72,2};
  constexpr uint8_t telemetryQuery[]={8,67,18,2,18,0,24,1,72,2};
  uint8_t hardware[meshtastic_HardwareMessage_size]{};meshtastic_HardwareMessage gpio{};
  gpio.type=meshtastic_HardwareMessage_Type_READ_GPIOS;
  auto hw=pb_ostream_from_buffer(hardware,sizeof(hardware));assert(pb_encode(&hw,meshtastic_HardwareMessage_fields,&gpio));
  uint8_t gpioData[239]{};size_t gpioSize=0;
  assert(mt::modules::GpioModuleWire::encodeRequest(hardware,hw.bytes_written,false,0,
    gpioData,sizeof(gpioData),gpioSize)&&gpioSize);
  uint8_t malformed[70]{};assert(positionSize+3<=sizeof(malformed));memcpy(malformed,positionData,positionSize);
  malformed[positionSize]=0xa0;malformed[positionSize+1]=0x06;malformed[positionSize+2]=0x01;
  auto refused=[&](uint32_t to,uint32_t badId,const uint8_t *dataBytes,size_t dataSize){
    bench::Frame out{};memset(out.bytes,0x5a,sizeof(out.bytes));out.len=91;
    const auto before=f.entropy.calls;assert(!f.s.encodeChannelControl(to,badId,dataBytes,dataSize,out));
    uint8_t unchanged[sizeof(out.bytes)];memset(unchanged,0x5a,sizeof(unchanged));
    assert(f.entropy.calls==before&&!f.ledger.sent(badId)&&out.len==91&&
      !memcmp(out.bytes,unchanged,sizeof(unchanged)));
  };
  refused(0xffffffffu,id+30,positionQuery,sizeof(positionQuery));
  refused(0xffffffffu,id+31,telemetryQuery,sizeof(telemetryQuery));
  refused(0xffffffffu,id+32,gpioData,gpioSize);
  refused(0xfffffffeu,id+33,positionData,positionSize);
  refused(f.local.binding.node,id+34,positionData,positionSize);
  refused(0xffffffffu,id+35,malformed,positionSize+3);
  assert(f.s.receiving()&&!f.s.faulted()&&f.radio.sends==1&&f.radio.frames.size()==1);
  group("custom AES128/AES256 primary position announcement broadcasts once through Service and rejects duplicate, generic, wrong-destination and malformed controls before reservation");
 }
 {
  Fixture f(true);f.s.stop();
  auto cfg=f.s.config;
  assert(cfg.setFixedPosition(123456789,-456789012,false,0));
  auto gps=cfg.gnssSettings();
  gps.broadcastSecs=60;
  assert(cfg.setGnssSettings(gps));
  const uint8_t key[16]={0x5a};
  mt::ChannelRecord secondary;
  assert(secondary.set(2,"Nav",key,sizeof(key),19)&&cfg.setSecondaryChannel(1,secondary));
  cfg.setPositionSharing(true);
  assert(cfg.setPrimaryPositionPrecision(0)&&!cfg.positionPrecision(0)&&cfg.positionPrecision(1)==19);
  assert(f.s.apply(cfg)&&f.s.start(false,0)&&f.s.armEndpoint(true));
  mt::ModuleController modules;
  mt::PositionController positions;
  constexpr uint32_t id=0x1234aabb;
  assert(f.knownChannelClock.set(1000));
  positions.poll(f.s,modules,true,1000,id);
  assert(f.s.selectedChannel()==0&&f.s.moduleAnnouncementPending()&&f.ledger.sent(id));
  f.poll(2000);
  assert(f.radio.frames.size()==1);
  const auto &frame=f.radio.frames.front();
  mt::PrimaryChannel::Selection selected;
  assert(cfg.channelSelection(1,selected)&&frame[13]==selected.hash);
  bench::MtCodec receiver;
  assert(receiver.setKey(selected.hash,selected.bytes,selected.length,selected.aead));
  mt::PrimaryChannel::clear(selected);
  bench::RouteMetadata meta{};
  assert(receiver.decode(frame.data(),frame.size(),meta)&&meta.channelDecoded&&meta.portnum==meshtastic_PortNum_POSITION_APP);
  meshtastic_Position position{};
  auto stream=pb_istream_from_buffer(meta.payload,meta.payloadLength);
  assert(pb_decode(&stream,meshtastic_Position_fields,&position)&&!stream.bytes_left);
  assert(position.precision_bits==19&&position.has_latitude_i&&position.has_longitude_i);
  assert(position.latitude_i==mt::PositionPrecision::truncateCoordinate(123456789,19u));
  assert(position.longitude_i==mt::PositionPrecision::truncateCoordinate(-456789012,19u));
  group("periodic Position selects the first sharing-enabled secondary, restores primary and transmits secondary precision");
 }
 {
  Fixture sender(true,true);
  Radio radio;
  mt::Service receiver(radio,true,sender.peer.binding.node,81,true,true);
  mt::test::KnownChannelFixtureClock clock;
  clock.bind(receiver);
  auto cfg=sender.s.config;
  assert(receiver.apply(cfg)&&receiver.start(false,0)&&receiver.armEndpoint(true));
  // Separate deterministic identities and real signing/verifying. Force reply
  // refusal so the observed window can only have come from the verified RX path.
  receiver.channelMeter().observeRx(24000,true);
  constexpr uint32_t window=mt::stock::nodeinfo::RecentSenderMs;
  for(const auto stamp:{100u,window-100u}) {
    const uint32_t id=stamp+500;
    assert(!sender.s.submitNodeInfo(0,id,UINT32_MAX,3,false,true,stamp));
    sender.poll(stamp+500);assert(!sender.radio.frames.empty());
    const auto &frame=sender.radio.frames.back();
    assert(bench::le32(frame.data()+8)==id);
    receiver.rx(frame.data(),frame.size(),3,stamp,id+2);
    sender.complete(stamp+900);
  }
  assert(receiver.nodeDiscovery.queued==0);
  assert(!receiver.nodeDiscovery.replyAllowed(sender.local.binding.node,window+101));
  assert(receiver.nodeDiscovery.replyAllowed(sender.local.binding.node,2*window-100));
  group("verified RF NodeInfo renews its sender suppression window despite denied reply allocation");
 }
 std::cout<<"SUMMARY primary-channel-runtime suite: "<<groups<<" groups\n";
}
