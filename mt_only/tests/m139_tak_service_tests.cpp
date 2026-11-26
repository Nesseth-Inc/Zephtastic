// SPDX-License-Identifier: GPL-3.0-only
// Actual Service/Endpoint/MtCodec TAK PLI queue path; synthetic position and radio only.
#include "endpoint_fixture.h"
#include "known_channel_fixture.h"
#include "pki/PrivateRuntime.h"
#include "gnss/PositionController.h"
#include "meshtastic/atak.pb.h"
#include "pb_decode.h"
#include <cassert>

namespace {
static unsigned groups=0;
void pass(const char *s){++groups;printf("PASS %s\n",s);}

void configureTak(Fixture &f,unsigned precision=8) {
  f.s.stop();
  auto cfg=f.s.config;
  assert(cfg.setFixedPosition(123456789,-456789012,true,77));
  cfg.setPositionSharing(true);
  assert(cfg.setPrimaryPositionPrecision(precision));
  assert(cfg.setDeviceRole(10,0));
  // Model the role-transition cadence installed by the real Admin path.
  auto gnss=cfg.gnssSettings();gnss.broadcastSecs=180;gnss.smart=true;
  gnss.smartMinInterval=15;gnss.smartMinDistance=20;gnss.positionFlags=777;
  assert(cfg.setGnssSettings(gnss));
  cfg.setOwnerUnmessagable(true);
  assert(cfg.valid()&&f.s.apply(cfg));
  assert(f.s.start(false,1000)&&f.s.armEndpoint(true));
  assert(f.s.takConfiguration(uint32_t(meshtastic_Team_Blue),
                              uint32_t(meshtastic_MemberRole_Medic)));
}

meshtastic_TAKPacketV2 decodePli(Fixture &f) {
  assert(!f.radio.frames.empty());
  bench::RouteMetadata m;
  assert(f.s.codec.decode(f.radio.frames.back().data(),f.radio.frames.back().size(),m));
  assert(m.channelDecoded&&!m.decodeRejected&&m.portnum==meshtastic_PortNum_ATAK_PLUGIN_V2);
  assert(m.payloadLength>1&&m.payload[0]==0xff);
  meshtastic_TAKPacketV2 out{};
  auto in=pb_istream_from_buffer(m.payload+1,m.payloadLength-1);
  assert(pb_decode(&in,meshtastic_TAKPacketV2_fields,&out)&&in.bytes_left==0);
  return out;
}

struct PrivateProbe final:mt::pki::PrivateRuntime {
  mt::Service *context=nullptr;
  uint32_t local=OURS;unsigned appCalls=0,freezes=0;bool canReady=true,allow=false;
  uint32_t observedPort=0;meshtastic_TAKPacketV2 observedPli{};
  uint32_t peer()const override{return 0;}
  bool ready(uint32_t n)const override{return canReady&&n==local;}
  bool publicIdentity(uint8_t *)const override{return false;}
  bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,uint32_t,uint32_t,bool,uint32_t,bench::Frame &)override{return false;}
  void freeze()override{++freezes;}
  bool matchesPeer(uint32_t)const override{return false;}
  bool revoke(uint32_t)override{return false;}
  void clearEvent()override{}
  void sweep(mt::Endpoint &)override{}
  int submit(mt::Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,bool,const uint8_t *,size_t,bool,uint32_t)override{return -ENOTSUP;}
  void receive(mt::Endpoint &,bench::MtCodec &,const uint8_t *,size_t,uint32_t,uint32_t,bool)override{}
  void proof(mt::Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t)override{}
  bool event(uint8_t,const uint8_t *,size_t,mt::pki::PrivateEvent &)override{return false;}
  bool encodePublicApplication(bench::MtCodec &codec,uint32_t to,uint32_t id,const uint8_t *data,size_t n,bench::Frame &frame)override{
    assert(context&&context->privateStoragePaused()&&!context->receiving());
    ++appCalls;meshtastic_Data d{};auto in=pb_istream_from_buffer(data,n);
    assert(pb_decode(&in,meshtastic_Data_fields,&d)&&!in.bytes_left);observedPort=uint32_t(d.portnum);
    assert(observedPort==meshtastic_PortNum_ATAK_PLUGIN_V2&&d.payload.size>1&&d.payload.bytes[0]==0xff);
    auto payload=pb_istream_from_buffer(d.payload.bytes+1,d.payload.size-1);
    assert(pb_decode(&payload,meshtastic_TAKPacketV2_fields,&observedPli)&&!payload.bytes_left);
    if(!allow)return false;
    size_t size=0;
    const bool ok=codec.encodeZeroHopData(local,to,id,data,n,frame.bytes,sizeof(frame.bytes),size);
    if(ok)frame.len=size;return ok;
  }
};

struct QuiescentRadio final:mt::Radio {
  int state=0;unsigned sends=0,stops=0,restores=0;uint8_t *pending=nullptr;bool done=false;
  std::vector<std::vector<uint8_t>> frames;
  bool ready()override{return true;}int start()override{state=1;return 0;}
  int restore()override{++restores;state=1;return 0;}int stop()override{++stops;state=0;pending=nullptr;return 0;}
  int health()override{return state;}bool busy()override{return false;}uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *p,size_t n)override{++sends;frames.emplace_back(p,p+n);pending=p;done=false;return 0;}
  bool completion(int &result)override{result=0;return done;}
};

}

int main() {
  {
    Fixture f;configureTak(f);assert(f.s.announcePosition(1000,0x13579bdf)==0);
    assert(f.s.endpointControls()==1);
    // The first queued module packet excludes a second announcement before ID leasing.
    assert(f.s.announcePosition(1001,0x2468ace0)==-EAGAIN&&f.s.endpointControls()==1);
    f.s.poll(1250,7);assert(f.radio.sends==1&&f.radio.frames.size()==1);
    const auto pli=decodePli(f);
    assert(pli.cot_type_id==meshtastic_CotType_CotType_a_f_G_U_C);
    assert(pli.team==meshtastic_Team_Blue&&pli.role==meshtastic_MemberRole_Medic);
    meshtastic_Position expected{};expected.latitude_i=123456789;expected.longitude_i=-456789012;
    mt::PositionPrecision::apply(expected,8);
    assert(pli.latitude_i==expected.latitude_i&&pli.longitude_i==expected.longitude_i&&pli.altitude==0);
    assert(!strcmp(pli.callsign,"ZephTastic")&&!strcmp(pli.device_callsign,"ZephTastic"));
    assert(!strcmp(pli.uid,"!12345678")&&pli.battery==0);
    assert(pli.how==meshtastic_CotHow_CotHow_h_e&&pli.geo_src==meshtastic_GeoPointSource_GeoPointSource_USER);
    assert(f.radio.frames.back()[12]==uint8_t((3u<<5)|3u));
    f.radio.done=true;f.s.poll(1800,8);assert(f.s.sent==1&&f.s.receiving());
    mt::HardwareSnapshot hardware;hardware.cellPercent=zt::Observed<uint8_t>(73);f.s.measuredHardware(hardware);
    assert(f.s.announcePosition(1900,0x13579be0)==0);f.s.poll(2150,9);
    assert(f.radio.sends==2&&decodePli(f).battery==73);
    pass("role10 emits one queued raw-ff port78 PLI with configured team/role, fixed-position provenance, owner UID and unknown battery zero");
  }
  {
    Fixture f;configureTak(f);f.s.stop();auto cfg=f.s.config;assert(cfg.setPrimaryPositionPrecision(0));
    assert(f.s.apply(cfg)&&f.s.start(false,1000)&&f.s.armEndpoint(true));
    assert(f.s.announcePosition(1000,51)==-EACCES&&f.s.endpointControls()==0&&f.radio.sends==0);
    assert(!f.s.takConfiguration(0,0,false));
    assert(f.s.announcePosition(1001,52)==-EACCES&&f.s.endpointControls()==0);
    assert(!f.s.takConfiguration(UINT32_MAX,0));
    assert(f.s.announcePosition(1002,53)==-EACCES&&f.s.endpointControls()==0);
    f.s.stop();cfg=f.s.config;cfg.clearFixedPosition();
    auto gnss=cfg.gnssSettings();gnss.enabled=true;assert(cfg.setGnssSettings(gnss));
    assert(cfg.setPrimaryPositionPrecision(8)&&f.s.apply(cfg)&&f.s.start(false,1000)&&f.s.armEndpoint(true));
    assert(f.s.takConfiguration(0,0));f.s.measuredPosition({});
    assert(f.s.announcePosition(1003,54)==-EACCES&&f.s.endpointControls()==0);
    pass("TAK unavailable or invalid config, zero privacy precision and missing valid position refuse before queueing");
  }
  {
    Fixture f;configureTak(f);assert(f.s.takConfiguration(0,0));
    assert(f.s.announcePosition(1000,57)==0);f.s.poll(1250,7);
    const auto pli=decodePli(f);
    assert(pli.team==meshtastic_Team_Cyan&&pli.role==meshtastic_MemberRole_TeamMember);
    pass("unspecified TAK team and member role use pinned stock Cyan and TeamMember defaults");
  }
  {
    Fixture f;f.s.stop();auto cfg=f.s.config;assert(cfg.setFixedPosition(123456789,-456789012,false,0));
    cfg.setPositionSharing(true);assert(cfg.setDeviceRole(1,0));assert(f.s.apply(cfg));
    assert(f.s.start(false,1000)&&f.s.armEndpoint(true));meshtastic_Position p{};
    p.has_latitude_i=p.has_longitude_i=true;p.latitude_i=333000000;p.longitude_i=-444000000;f.s.measuredPosition(p);
    assert(f.s.announcePosition(1000,61)==0);f.s.poll(1250,7);assert(f.radio.sends==1);
    bench::RouteMetadata m;assert(f.s.codec.decode(f.radio.frames.back().data(),f.radio.frames.back().size(),m));
    assert(m.channelDecoded&&m.portnum==meshtastic_PortNum_POSITION_APP);
    f.radio.done=true;f.s.poll(1800,8);pass("ordinary client role keeps existing port3 Position announcement path");
  }
  {
    Fixture f;configureTak(f);mt::ModuleController modules;mt::ModuleSettings settings;
    mt::PositionController positions;
    // The real interval resolver used to refuse all roles above ROUTER.
    modules.poll(f.s,settings,true,1000,80,[](const uint8_t *,size_t,uint32_t){});
    positions.poll(f.s,modules,true,1000,81);
    assert(positions.schedule().lastSendMs()==1000&&f.s.endpointControls()==1);
    f.s.poll(1250,82);assert(f.radio.sends==1&&decodePli(f).team==meshtastic_Team_Blue);
    pass("TAK tracker passes the existing region/online interval resolver and position scheduler into the common queue");
  }
  {
    PrivateProbe privateRuntime;QuiescentRadio radio;mt::Service service(radio,true,OURS,81,true,false,&privateRuntime);
    privateRuntime.context=&service;
    mt::test::KnownChannelFixtureClock clock;clock.bind(service);
    auto cfg=service.config;cfg.bytes[4]=1;auto node=cfg.node();node.setPublic();cfg.node(node);
    assert(cfg.setFixedPosition(123456789,-456789012,true,77));cfg.setPositionSharing(true);
    assert(cfg.setPrimaryPositionPrecision(32)&&cfg.setDeviceRole(10,0));
    assert(cfg.setSecondaryChannel(1,[] {mt::ChannelRecord c;uint8_t k[16]{};k[0]=0x31;assert(c.set(2,"Ops",k,sizeof(k),8,false,false,false));return c;}()));
    assert(service.apply(cfg)&&service.start(false,1000)&&service.armEndpoint(true)&&service.selectChannel(1));
    assert(service.takConfiguration(uint32_t(meshtastic_Team_Blue),uint32_t(meshtastic_MemberRole_Medic)));
    assert(service.receiving());
    assert(service.announcePosition(1000,71)==-EPERM);
    meshtastic_Position expected{};expected.latitude_i=123456789;expected.longitude_i=-456789012;mt::PositionPrecision::apply(expected,8);
    assert(privateRuntime.appCalls==1&&privateRuntime.observedPort==meshtastic_PortNum_ATAK_PLUGIN_V2&&
      privateRuntime.observedPli.latitude_i==expected.latitude_i&&privateRuntime.observedPli.longitude_i==expected.longitude_i);
    assert(radio.stops==1&&radio.restores==1&&radio.sends==0&&service.receiving()&&!service.faulted());
    privateRuntime.allow=true;assert(service.announcePosition(1001,72)==0&&privateRuntime.appCalls==2);
    assert(service.announcePosition(1002,73)==-EAGAIN&&privateRuntime.appCalls==2&&service.endpointControls()==1);
    assert(radio.stops==2&&radio.restores==2&&radio.sends==0&&service.receiving()&&!service.faulted());
    pass("custom-channel provider sees selected-channel precision only under pause; refusal restores RX without plaintext fallback, queue pressure avoids another provider lease");
  }
  printf("PASS %u M139 TAK Service groups; host fixture only\n",groups);
}
