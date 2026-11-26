// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include "NodeJournal.h"
static std::vector<uint8_t> nodeWire(Fixture &f,uint32_t from,uint32_t to,uint32_t id,
    uint8_t flags=0,bool wants=true,uint32_t request=0,bool empty=false) {
  bench::Frame frame;
  if(empty) {
    const uint8_t data[]={8,4,72,2};size_t len=0;
    assert(f.s.codec.encodeZeroHopData(from,to,id,data,sizeof(data),frame.bytes,255,len));frame.len=len;
  } else assert(mt::Discovery::encode(f.s.codec,f.s.config.node(),from,81,to,id,wants,request,frame));
  frame.bytes[12]=flags;return {frame.bytes,frame.bytes+frame.len};
}
static bench::RouteMetadata metadata(Fixture &f,const std::vector<uint8_t> &wire) {
  bench::RouteMetadata m;assert(f.s.codec.decode(wire.data(),wire.size(),m));return m;
}
static meshtastic_User decodeUser(const bench::RouteMetadata &m) {
  meshtastic_User user{};auto stream=pb_istream_from_buffer(m.payload,m.payloadLength);assert(pb_decode(&stream,meshtastic_User_fields,&user));return user;
}
static void request(Fixture &f,uint32_t id,uint32_t to=PEER,bool ack=true,bool response=true,uint32_t now=100,uint8_t owner=0) {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
  auto &p=r.packet;p.from=0xDEADCAFE;p.id=id;p.to=to;p.want_ack=ack;p.hop_limit=3;
  p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_NODEINFO_APP;p.decoded.want_response=response;
  input(owner?f.bc:f.c,r,now);
}
static std::vector<uint8_t> peerResponse(Fixture &f,uint32_t id,uint32_t request,unsigned keyLength=0,uint8_t keyByte=0,int32_t model=81,const char *name="Peer") {
  meshtastic_User user{};strcpy(user.long_name,name);strcpy(user.short_name,"P");
  user.hw_model=static_cast<meshtastic_HardwareModel>(model<0?0:model);user.public_key.size=keyLength;
  memset(user.public_key.bytes,keyByte,keyLength);
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_NODEINFO_APP;data.request_id=request;data.has_bitfield=true;
  auto payload=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));assert(pb_encode(&payload,meshtastic_User_fields,&user));data.payload.size=payload.bytes_written;
  // Inject an invalid wide wire value, not an enum cast that becomes the valid
  // future model65535 under -fshort-enums before nanopb can encode it.
  if(model<0){const uint8_t invalid[]={0x28,0xff,0xff,0xff,0xff,0x0f};assert(data.payload.size+sizeof(invalid)<=sizeof(data.payload.bytes));memcpy(data.payload.bytes+data.payload.size,invalid,sizeof(invalid));data.payload.size+=sizeof(invalid);}

  uint8_t encoded[239],wire[255];auto stream=pb_ostream_from_buffer(encoded,sizeof(encoded));assert(pb_encode(&stream,meshtastic_Data_fields,&data));size_t length=0;
  assert(f.s.codec.encodeZeroHopData(PEER,OURS,id,encoded,stream.bytes_written,wire,sizeof(wire),length));
  return {wire,wire+length};
}
int main() {
  {
    for(uint8_t model:{81,127,132}) {
      Fixture f(true,model,true);assert(f.s.armEndpoint(false));f.s.poll(0,700);f.s.poll(100000,701);assert(!f.radio.sends);
      assert(f.s.armEndpoint(true));f.s.poll(100001,702);f.s.poll(130000,703);assert(!f.radio.sends);
      f.s.poll(130001,704);assert(!f.radio.sends);f.s.poll(130251,705);assert(f.radio.sends==1);
      auto m=metadata(f,f.radio.frames[0]);auto u=decodeUser(m);
      assert(m.portnum==4&&!mt::Discovery::wantsResponse(m)&&!m.request_id&&m.hasBitfield);
      assert(!strcmp(u.id,"!12345678")&&!strcmp(u.long_name,"ZephTastic")&&!strcmp(u.short_name,"ZT"));
      assert(u.hw_model==model&&u.role==meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE&&u.has_is_unmessagable&&!u.is_unmessagable&&!u.public_key.size);
      assert(bench::le32(f.radio.frames[0].data())==0xffffffffu&&f.radio.frames[0][12]==(3|3<<5));
    }
    group("NodeInfo boot/disarm silence; 30-second armed delay and exact local owner/model/role");
  }
  {
    const char name[]="Node owner name 24chars!";
    static_assert(sizeof(name)-1==24);
    Fixture f;
    auto record=f.s.config.node();
    assert(record.setName(name,false)&&record.setName("FULL",true));
    bench::MtNodeInfo legacy;
    legacy.identity(OURS);
    assert(legacy.configure(record));
    struct Vector {uint8_t model;std::vector<uint8_t> field;};
    const Vector vectors[]={{81,{0x28,0x51}}, {127,{0x28,0x7f}}, {128,{0x28,0x80,0x01}},
                            {132,{0x28,0x84,0x01}}, {255,{0x28,0xff,0x01}}};
    for(const auto &v:vectors) {
      legacy.hardwareModel(v.model);
      for(bool useLegacy:{false,true}) {
        bench::Frame frame;
        if(useLegacy)assert(legacy.encode(70,frame));
        else assert(mt::Discovery::encode(f.s.codec,record,OURS,v.model,PEER,70,false,123,frame));
        bench::RouteMetadata m;
        assert(f.s.codec.decode(frame.bytes,frame.len,m)&&m.portnum==4&&m.nodeInfoValid);
        std::vector<uint8_t> expected={0x0a,9,'!','1','2','3','4','5','6','7','8',0x12,24};
        expected.insert(expected.end(),name,name+24);
        expected.insert(expected.end(),{0x1a,4,'F','U','L','L'});
        expected.insert(expected.end(),v.field.begin(),v.field.end());
        expected.insert(expected.end(),{0x38,uint8_t(useLegacy?0:1),0x48,uint8_t(useLegacy?1:0)});
        assert(m.payloadLength==expected.size()&&!memcmp(m.payload,expected.data(),expected.size()));
        const auto user=decodeUser(m);
        assert(user.hw_model==v.model&&!strcmp(user.long_name,name)&&!strcmp(user.short_name,"FULL"));
        assert(user.has_is_unmessagable&&user.is_unmessagable==useLegacy&&m.request_id==(useLegacy?0u:123u));
      }
    }
    // Every assigned 2.8.2 wire value, including UNSET and the reserved identities.
    for(unsigned model=0;model<=255;++model) {
      if(model>151 && model<255) continue;
      legacy.hardwareModel(uint8_t(model));
      for(bool useLegacy:{false,true}) {
        bench::Frame frame;
        if(useLegacy)assert(legacy.encode(70,frame));
        else assert(mt::Discovery::encode(f.s.codec,record,OURS,uint8_t(model),PEER,70,false,123,frame));
        bench::RouteMetadata decoded;
        assert(f.s.codec.decode(frame.bytes,frame.len,decoded) && decodeUser(decoded).hw_model==model);
      }
    }
    group("Both discovery encoders retain exact User bytes through model 127 and canonical multibyte IDs through 255 with maximum names");
  }
  {
    Fixture f(true,81,true);f.s.poll(0,700);f.s.poll(30000,701);assert(f.s.armEndpoint(false));f.s.poll(31000,702);assert(!f.radio.sends);
    assert(f.s.armEndpoint(true));f.s.stop();assert(f.s.apply(f.s.config)&&f.s.start(false,32000));f.s.poll(100000,703);assert(!f.radio.sends&&!f.s.endpointArmed());
    Fixture noTx(false,81,true);noTx.s.poll(0,700);noTx.rx(nodeWire(noTx,PEER,OURS,1),1000);noTx.s.poll(100000,702);assert(!noTx.radio.sends);
    group("disarm/configuration clears pending discovery and compiled TX gate blocks origination/replies");
  }
  {
    Fixture manual(true,81,true);
    manual.s.channelMeter().observeRx(24000,true); // Exactly 40% of the logged minute.
    assert(manual.s.submitNodeInfo(0,94,PEER,3,true,true,100)==-EAGAIN&&!manual.s.nodeDiscovery.queued);
    manual.channelNow=60000;
    assert(!manual.s.submitNodeInfo(0,94,PEER,3,true,true,101)&&manual.s.nodeDiscovery.queued==1);
    manual.s.channelMeter().observeRx(25000,true);
    assert(!manual.s.submitNodeInfo(1,94,PEER,3,true,true,102)&&manual.s.nodeDiscovery.queued==1);
    Fixture automatic(true,81,true);
    automatic.s.nodeInfoIntervalProvider([](mt::Service &,uint32_t &ms){ms=3600000;return true;});
    automatic.s.channelMeter().observeRx(24000,true);
    automatic.rx(nodeWire(automatic,PEER,OURS,100),1000,700);
    assert(!automatic.s.nodeDiscovery.queued);
    automatic.channelNow=60000;
    automatic.rx(nodeWire(automatic,PEER,OURS,101),1100,701);
    assert(automatic.s.nodeDiscovery.queued==1);
    automatic.s.poll(1400,702);automatic.done(1500);
    automatic.rx(nodeWire(automatic,PEER+1,OURS,102),1801100,703);
    assert(automatic.s.nodeDiscovery.queued==1);
    automatic.rx(nodeWire(automatic,PEER+1,OURS,103),3601100,704);
    assert(automatic.s.nodeDiscovery.queued==2);
    group("40% RF occupancy refuses fresh requests and replies without consuming admission; duplicate requests survive and live scaled floor applies");
  }
  {
    // TRACKER and SENSOR can suppress solicitation at the stock policy boundary.
    // Test original identity against the cached effective Endpoint job even though
    // the current board role menu does not enable those two roles.
    Fixture f(true,81,true);
    const auto wire=nodeWire(f,OURS,UINT32_MAX,52,1,false);
    for(auto role:{5u,6u}) {
      mt::Endpoint endpoint;
      const unsigned requestedHop=5;
      const bool requestedResponse=true;
      const uint8_t options=requestedHop|(requestedResponse?8u:0u);
      const bool effectiveResponse=mt::stock::nodeinfo::sendOurNodeInfo(requestedResponse,role);
      assert(!effectiveResponse);
      assert(!endpoint.submit(0,wire.data(),wire.size(),false,100,true,true,effectiveResponse,nullptr,
                              false,false,false,false,false,false,0,false,false,options));
      unsigned hop=1;
      bool response=false;
      assert(!endpoint.resolveOriginPolicy(52,4u|8u,hop,response));
      assert(!endpoint.resolveOriginPolicy(52,requestedHop,hop,response));
      hop=7;response=mt::stock::nodeinfo::sendOurNodeInfo(requestedResponse,0);
      assert(response&&endpoint.resolveOriginPolicy(52,options,hop,response)&&hop==1&&!response);
      assert(!endpoint.submit(1,wire.data(),wire.size(),false,101,true,false,response,nullptr,
                              false,false,false,false,false,false,0,false,false,options));
    }
    group("NodeInfo keeps requested hop and solicitation distinct under clamping, and identical retries reuse original effective options");
  }
  {
    mt::Discovery discovery;
    constexpr uint32_t window=mt::stock::nodeinfo::RecentSenderMs;
    assert(discovery.observeRequest(PEER,100,true));
    assert(!discovery.observeRequest(PEER,window-100,true));
    assert(!discovery.observeRequest(PEER,window+101,true)); // Refreshed by the suppressed request.
    assert(!discovery.observeRequest(PEER,2*window,false)); // An unsigned packet cannot extend it again.
    assert(discovery.observeRequest(PEER,2*window+101,true));
    assert(discovery.observeRequest(PEER+1,100,false)&&discovery.observeRequest(PEER+1,101,false));
    discovery.replied(PEER+1,101);
    assert(!discovery.observeRequest(PEER+1,102,false));
    group("verified NodeInfo request timestamps slide across suppressed requests; unsigned requests never extend the window");
  }
  {
    for(uint8_t flags:{uint8_t(8),uint8_t((3<<5)|3|8),uint8_t((3<<5)|1|8)}) {
      Fixture f(true,127,true);f.rx(nodeWire(f,PEER,OURS,100,flags),1000,700);f.s.poll(1300,701);assert(f.radio.sends==1);
      auto m=metadata(f,f.radio.frames[0]);unsigned hops=flags==8?0:((flags&7)==3?2:3);
      assert(m.portnum==4&&m.request_id==100&&!mt::Discovery::wantsResponse(m)&&m.nodeInfoValid);
      assert(bench::le32(f.radio.frames[0].data())==PEER&&bench::le32(f.radio.frames[0].data()+4)==OURS);
      assert(f.radio.frames[0][12]==uint8_t(hops|(hops<<5)|8));
    }
    group("stock-shaped NodeInfo reply preserves request ID, destination and capped return-hop/ACK policy");
  }
  {
    Fixture f(true,81,true);f.rx(nodeWire(f,PEER,0xffffffffu,100,0,true,0,true),1000,700);f.s.poll(1300,701);assert(f.radio.sends==1);f.done(1400);
    auto m=metadata(f,f.radio.frames[0]);assert(m.request_id==100&&!mt::Discovery::wantsResponse(m));
    f.rx(nodeWire(f,PEER,OURS,101),70000,701);f.s.poll(70300,702);assert(f.radio.sends==1);
    f.rx(nodeWire(f,PEER+1,OURS,102),71000,702);f.s.poll(71300,703);assert(f.radio.sends==1);
    f.rx(nodeWire(f,PEER+1,OURS,104),1801000,707);f.s.poll(1801300,708);assert(f.radio.sends==2);f.done(1801400);
    f.rx(nodeWire(f,PEER+2,OURS,103),1802000,704);f.s.poll(1802300,705);assert(f.radio.sends==2);
    group("empty User queries work; reply suppression is per-peer 12 hours plus automatic 30-minute floor");
  }
  {
    Fixture f(true,81,true);
    for(unsigned i=0;i<16;++i){const auto now=1000+i*1801000;f.rx(nodeWire(f,PEER+i,OURS,100+i),now,700+i);f.s.poll(now+300,800+i);f.done(now+400);}
    assert(f.radio.sends==16&&f.s.nodeDiscovery.replies==16);
    f.rx(nodeWire(f,PEER+100,OURS,500),16*1801000+1000,1000);f.s.poll(16*1801000+1300,1001);assert(f.radio.sends==16);
    group("16 live peer suppression records fail closed instead of evicting under a sender flood");
  }
  {
    Fixture f(true,81,true);f.rx(nodeWire(f,PEER,OURS,100),1000,700);
    for(unsigned i=1;i<=4;++i)f.text(i,PEER,false,1100);
    for(unsigned i=0;i<4;++i)f.rx(f.wire(PEER+i,OURS,200+i,0,0,8,true),1100,800+i);
    f.s.poll(1400,900);assert(f.radio.sends==1&&metadata(f,f.radio.frames[0]).portnum==5);f.done(1500);
    // Three ACK slots stay available; NodeInfo consumes at most one of four.
    for(auto now:{1800u,2200u}){f.s.poll(now,901);assert(metadata(f,f.radio.frames.back()).portnum==5);f.done(now+1);}
    for(auto now:{2600u,3000u,3400u,3800u}){f.s.poll(now,902);assert(metadata(f,f.radio.frames.back()).portnum==1);f.done(now+1);}
    f.s.poll(4200,903);assert(f.radio.sends==8&&metadata(f,f.radio.frames.back()).portnum==4);
    group("one low-priority NodeInfo leaves three ACK slots and yields to every ready user message");
  }
  {
    Fixture f(true,81,true);f.radio.occupied=true;f.s.poll(0,700);f.s.poll(30000,701);f.s.poll(41000,702);
    f.s.poll(59999,703);assert(!f.radio.sends&&!f.s.faulted());
    f.radio.occupied=false;f.s.poll(60000,704);assert(!f.radio.sends&&!f.s.faulted());
    Fixture busy(true,81,true);busy.radio.sendStatus=-EBUSY;busy.s.poll(0,700);busy.s.poll(30000,701);
    for(auto now:{30250u,31250u,32250u,33250u})busy.s.poll(now,702);
    assert(busy.radio.sends==3&&!busy.s.faulted());
    group("discovery contention expires; native EBUSY consumes at most three attempts");
  }
  {
    Fixture f(true,81,true);f.radio.air=2200;
    for(unsigned i=0;i<2;++i){auto now=1000+i*3000;f.text(10+i,PEER,false,now);f.s.poll(now,700);f.done(now+2300);}
    auto utilization=f.s.channel_.percent();assert(f.s.channel_.diagnosticSupported()&&utilization&&*utilization<40.0f);
    f.rx(nodeWire(f,PEER,OURS,100),7000,701);f.s.poll(7300,702);assert(f.radio.sends==3&&!f.s.throttled&&!f.s.faulted());
    group("normal-mode NodeInfo follows channel and region admission policy");
  }
  {
    Fixture f(true,81,true);f.s.poll(0,700);f.s.poll(30000,701);f.radio.sendStatus=-EIO;f.s.poll(30250,702);
    assert(f.s.faulted()&&!f.s.endpointArmed());f.s.poll(1000000,703);assert(f.radio.sends==1);
    group("native discovery fault cancels all further background RF");
  }
  {
    Fixture f(true,81,true);request(f,77);request(f,77,PEER,true,true,101,1);f.s.poll(300,700);f.done(400);assert(f.radio.sends==1);
    auto m=metadata(f,f.radio.frames[0]);assert(m.nodeInfoValid&&mt::Discovery::wantsResponse(m)&&bench::le32(f.radio.frames[0].data()+4)==OURS);
    assert(!results(f.h,77));f.rx(nodeWire(f,PEER+1,OURS,101,0,false,77),500,701);f.clients();assert(!results(f.h,77));
    f.rx(nodeWire(f,PEER,0xffffffffu,103,0,false,77),510,703);f.clients();assert(!results(f.h,77));
    for(auto &r:f.h.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.id==103)assert(!r.packet.decoded.request_id);
    auto requestShaped=nodeWire(f,PEER,OURS,104,0,false,77);
    // Change modern bitfield inside the encrypted payload from 0 to 2.
    requestShaped.back()^=2;f.rx(requestShaped,520,704);f.clients();assert(!results(f.h,77));
    for(auto &r:f.h.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.id==104)assert(!r.packet.decoded.request_id);
    f.rx(nodeWire(f,PEER,OURS,102,8,false,77),600,702);f.clients();assert(results(f.h,77,0,PEER)==1&&results(f.bh,77,0,PEER)==1);
    bool correlated=false;for(auto &r:f.h.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.id==102)correlated=r.packet.decoded.request_id==77;
    assert(correlated);f.s.poll(900,703);assert(f.radio.sends==2&&metadata(f,f.radio.frames.back()).portnum==5&&f.radio.frames.back()[12]==0);
    group("USB/BLE duplicate NodeInfo requests share TX; only matched peer application response completes and correlates");
  }
  {
    Fixture f(true,81,true);request(f,77,0xffffffffu,true,true);f.s.poll(300,700);f.done(400);
    f.rx(nodeWire(f,PEER,OURS,101,0,false,77),500,701);f.clients();assert(results(f.h,77,0,PEER)==1);
    request(f,78,PEER,true,true,1000);assert(results(f.h,78,38)==1);
    const auto admissions=f.s.nodeDiscovery.queued;request(f,77,0xffffffffu,true,true,70000);f.clients();assert(f.s.nodeDiscovery.queued==admissions&&f.radio.sends==1);
    group("broadcast query names actual responding peer; repeat completion does not consume another admission");
  }
  {
    Fixture f(true,81,true);request(f,77);f.s.poll(300,700);f.done(400);f.s.poll(90100,701);f.clients();assert(results(f.h,77,3)==1);
    f.rx(nodeWire(f,PEER,OURS,101,0,false,77),90200,702);f.clients();assert(!results(f.h,77,0));
    for(auto &r:f.h.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.id==101)assert(!r.packet.decoded.request_id);
    group("late NodeInfo cannot revive expired request through endpoint or raw companion correlation");
  }
  {
    Fixture f(true,81,true);request(f,77);f.s.poll(300,700);assert(f.s.active());
    f.rx(f.wire(PEER,OURS,100,77,2),310,701); // NAK while native completion is pending.
    f.rx(nodeWire(f,PEER,OURS,101,0,false,77),320,702);f.clients();
    assert(!results(f.h,77,0));
    for(auto &r:f.h.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.id==101)assert(!r.packet.decoded.request_id);
    f.done(400);f.clients();assert(results(f.h,77,2)==1&&!results(f.h,77,0));
    group("active terminal NAK cannot be bypassed by later NodeInfo before native completion");
  }
  {
    Fixture f(true,81,true);meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=r.packet;p.id=77;p.to=PEER;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_NODEINFO_APP;
    p.decoded.payload.size=1;p.decoded.payload.bytes[0]=1;input(f.c,r,100);assert(results(f.h,77,32)==1);
    p.id=78;p.decoded.payload.size=0;p.pki_encrypted=true;input(f.c,r,101);assert(results(f.h,78,34)==1);
    assert(f.s.armEndpoint(false));request(f,79);assert(results(f.h,79,4)==1);f.s.poll(100000,702);assert(!f.radio.sends);
    group("caller owner payload/explicit PKI requests are rejected and disarmed NodeInfo reports NO_INTERFACE");
  }
  {
    for(unsigned keyLength:{1u,32u}) {
      Fixture baseline(true,81,true),hinted(true,81,true);meshtastic_ToRadio r{};
      r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
      p.id=80;p.to=PEER;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
      p.decoded.portnum=meshtastic_PortNum_NODEINFO_APP;
      input(baseline.c,r,102);p.public_key.size=keyLength;memset(p.public_key.bytes,0xa5,keyLength);input(hinted.c,r,102);
      assert(!results(hinted.h,80)&&hinted.s.nodeDiscovery.queued==baseline.s.nodeDiscovery.queued);
      baseline.s.poll(400,700);hinted.s.poll(400,700);
      assert(baseline.radio.frames.size()==1&&hinted.radio.frames==baseline.radio.frames);
      const auto m=metadata(hinted,hinted.radio.frames[0]);const auto user=decodeUser(m);
      assert(m.portnum==meshtastic_PortNum_NODEINFO_APP&&m.nodeInfoValid&&!user.public_key.size&&
        bench::le32(hinted.radio.frames[0].data()+4)==OURS&&bench::le32(hinted.radio.frames[0].data())==PEER);
      baseline.done(500);hinted.done(500);assert(results(hinted.h,80)==results(baseline.h,80));
    }
    group("NodeInfo ignores bounded key hints without explicit PKI and preserves the ordinary channel wire");
  }
  {
    Fixture f(true,81,true);meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=r.packet;p.id=88;p.to=PEER;p.want_ack=true;p.hop_limit=3;
    p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_NODEINFO_APP;
    p.decoded.has_bitfield=true;p.decoded.bitfield=2;input(f.c,r,100);
    f.s.poll(300,700);assert(f.radio.sends==1&&mt::Discovery::wantsResponse(metadata(f,f.radio.frames[0])));
    f.done(400);f.rx(nodeWire(f,PEER,OURS,101,0,false,88),500,701);f.clients();assert(results(f.h,88,0,PEER)==1);
    group("modern local Data bit 1 preserves requested NodeInfo application response and correlation");
  }
  {
    for(unsigned mode=0;mode<4;++mode) {
      Fixture f(true,81,true);request(f,77);request(f,77,PEER,true,true,101,1);f.s.poll(300,700);f.done(400);
      if(mode==0) {
        struct FailedStorage:mt::SettingsBackend {
          int read(const char *,uint8_t *,size_t)override{return -EIO;}
          int write(const char *,const uint8_t *,size_t)override{return -EIO;}
        } storage;
        mt::NodeJournal journal;assert(journal.load(storage,f.s.nodes,OURS)==-EIO&&!f.s.nodes.ready());
      } else if(mode<3) {
        auto initial=peerResponse(f,90,0,32,0x42);auto m=metadata(f,initial);
        assert(mt::NodeUser::accept(f.s.nodes,OURS,PEER,m.payload,m.payloadLength,0,0)>=0);
      }
      auto reply=peerResponse(f,101,77,mode==2?32:0,0x43,mode==3?-1:81);
      f.rx(reply,500,701);f.clients();assert(!results(f.h,77,0)&&!results(f.bh,77,0));
      for(auto &r:f.h.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag)assert(r.packet.id!=101);
      if(mode==1||mode==2) {
        assert(f.s.nodes.get(0)->bytes[mt::NodeCache::Entry::Key]==0x42);
        f.rx(peerResponse(f,102,77,32,0x42),600,702);f.clients();
        assert(results(f.h,77,0,PEER)==1&&results(f.bh,77,0,PEER)==1);
      }
    }
    group("frozen, key-removing, key-changing and invalid-model User cannot complete a query; admitted key succeeds on both clients");
  }
  {
    Fixture f(true,81,true);
    const auto older=peerResponse(f,100,0,32,0x42,81,"Older name");
    f.rx(older,500,700);f.rx(peerResponse(f,101,0,32,0x42,81,"Current name"),600,701);
    auto current=[&](){return reinterpret_cast<const char *>(f.s.nodes.get(0)->bytes+mt::NodeCache::Entry::Name);};
    assert(!strcmp(current(),"Current name"));f.h.out.clear();f.bh.out.clear();
    f.rx(older,700,702);assert(!strcmp(current(),"Current name")&&f.h.out.empty()&&f.bh.out.empty());
    group("non-mutating endpoint admission preserves companion duplicate guard against old identity replay");
  }
  {
    Fixture f(true,81,true);const uint32_t start=0xfffff000u;
    f.s.stop();assert(f.s.start(false,start)&&f.s.armEndpoint(true));f.s.poll(start,700);
    f.s.poll(start+30000,701);f.s.poll(start+30250,702);assert(f.radio.sends==1);f.done(start+30300);
    const auto interval=f.s.config.node().interval()*1000u;
    f.s.poll(start+30000+interval-1,703);assert(f.radio.sends==1);
    f.s.poll(start+30000+interval,704);f.s.poll(start+30250+interval,705);assert(f.radio.sends==2);
    group("monotonic clock wrap preserves 30-second startup and configured periodic interval");
  }
  {
    Fixture f(true,81,true);assert(!f.s.storageQuiescent());
    assert(f.s.armEndpoint(false));assert(!f.s.storageQuiescent()); // RX still on.
    f.s.stop();assert(f.s.storageQuiescent());
    assert(f.s.start(false,0)&&f.s.armEndpoint(true));
    f.rx(nodeWire(f,PEER,OURS,100),1000,700);assert(!f.s.storageQuiescent());
    f.s.poll(1300,701);assert(f.s.active()&&!f.s.storageQuiescent());
    f.done(1400);assert(!f.s.storageQuiescent());f.s.stop();assert(f.s.storageQuiescent());
    assert(f.s.start(false,2000)&&f.s.armEndpoint(true));f.radio.sendStatus=-EIO;
    f.text(500,PEER,false,2000);f.s.poll(2300,702);assert(f.s.faulted()&&!f.s.storageQuiescent());
    Fixture stopFailure;stopFailure.radio.stopStatus=-EIO;stopFailure.s.stop();assert(stopFailure.s.faulted()&&!stopFailure.s.storageQuiescent());
    group("storage quiescence requires fully stopped service and never authorizes RX, armed, active or faulted state");
  }
  {
    Fixture f;mt::Endpoint e;assert(e.idle());
    auto text=f.wire(OURS,PEER,10,0,0,3|(3<<5),true);assert(!e.submit(0,text.data(),text.size(),true,100));assert(!e.idle());
    auto *job=e.next(100);assert(job);e.started(*job,0,100,100,700);assert(!e.idle());e.complete(*job,200,700);assert(!e.idle());
    e.cancel();assert(e.idle());bench::Frame frame;auto node=nodeWire(f,OURS,PEER,20,8,false,1);
    frame.len=node.size();memcpy(frame.bytes,node.data(),node.size());assert(!e.submitNodeInfoControl(frame,true,300)&&!e.idle());
    e.cancel();assert(e.idle());auto rx=f.wire(PEER,OURS,30,0,0,8,true);auto m=metadata(f,rx);
    e.receive(f.s.codec,f.s.route,OURS,rx.data(),rx.size(),m,3,400,701,true);assert(!e.idle());e.cancel();assert(e.idle());
    group("endpoint idle includes queued, native-active, waiting, ACK and NodeInfo jobs; cancellation drains all");
  }
  {
    Radio radio;radio.air=2200;mt::Service service(radio,true,OURS,81,false,true);
    mt::Config config;config.bytes[4]=1;auto record=config.node();record.setPublic();config.node(record);
    assert(config.valid()&&service.apply(config)&&service.start(true,0));
    auto sendNodeInfo=[&](uint32_t at,uint32_t id,int expectedSends) {
      assert(service.announce(at,id));service.poll(at+250,id);assert(radio.sends==expectedSends);
      radio.done=true;service.poll(at+251,id);
    };
    sendNodeInfo(1,55,1);sendNodeInfo(60252,56,2);sendNodeInfo(120503,57,2);
    assert(service.throttled==1&&!service.faulted());
    group("native NodeInfo shares the fixed 6000ms airtime budget");
  }
  {
    for(bool explicitAnnounce:{false,true}) {
      Radio radio;mt::Service service(radio,true,OURS,81,false,true);
      mt::Config config;config.bytes[4]=1;config.seal();assert(config.setPrimaryAead(true)&&config.valid());
      const auto record=config.node();const uint8_t effectiveHash=uint8_t(record.bytes[4]^0xAEu);
      assert(service.apply(config));
      const auto &stored=service.discovery.record();
      assert(stored.bytes[4]==record.bytes[4]&&
        !memcmp(stored.bytes+bench::MtNodeRecord::Key,record.bytes+bench::MtNodeRecord::Key,record.bytes[3]));
      assert(service.start(true,0));
      if(explicitAnnounce) {
        assert(service.announce(1,55));service.poll(251,55);
      } else {
        service.poll(0,700);service.poll(30000,701);service.poll(30250,702);
      }
      assert(radio.sends==1&&!radio.frames.empty());
      const auto &wire=radio.frames[0];bench::RouteMetadata m{};
      assert(wire.size()>28&&wire[13]==effectiveHash&&service.codec.decode(wire.data(),wire.size(),m));
      assert(m.channelDecoded&&m.portnum==4&&m.nodeInfoValid&&!m.request_id&&
        bench::le32(wire.data())==UINT32_MAX&&bench::le32(wire.data()+4)==OURS);
    }
    group("legacy periodic and explicit NodeInfo announcements use effective primary AEAD hash and authenticated CCM wire");
  }
  std::cout<<"SUMMARY NodeInfo suite: 27 scenario groups; no hardware or RF\n";
}
