// SPDX-License-Identifier: GPL-3.0-only
// M18 local fixed-position persistence and companion behavior. Memory only:
// no device, GNSS, serial port, credential, radio send or owner location.
#include "device_config_fixture.h"

namespace {
constexpr uint32_t HistoryPeer=0x87654321;
struct PrivatePositionRuntime final:mt::pki::PrivateRuntime {
  uint32_t peer()const override{return HistoryPeer;}
  bool ready(uint32_t local)const override{return local==Node;}
  bool publicIdentity(uint8_t out[32])const override{memset(out,0x42,32);return true;}
  bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,uint32_t,uint32_t,bool,uint32_t,bench::Frame &)override{return false;}
  void freeze()override{assert(false);}
  bool matchesPeer(uint32_t value)const override{return value==HistoryPeer;}
  bool revoke(uint32_t)override{return false;}
  void clearEvent()override{}
  void sweep(mt::Endpoint &)override{}
  int submit(mt::Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,bool,const uint8_t *,size_t,bool,uint32_t)override{return -ENOTSUP;}
  void receive(mt::Endpoint &,bench::MtCodec &,const uint8_t *,size_t,uint32_t,uint32_t,bool)override{assert(false);}
  void proof(mt::Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t)override{assert(false);}
  bool event(uint8_t,const uint8_t *,size_t,mt::pki::PrivateEvent &)override{return false;}
};
struct PreservedState {unsigned keys=0;size_t history=0,known=0;};
static PreservedState seedUnrelatedState(mt::Service &service,uint32_t id=0x18180001) {
  const uint8_t publicKey[32]={9};
  assert(service.signature.trustPublicKey(HistoryPeer,publicKey,100));
  uint8_t frame[17]{};
  for(unsigned i=0;i<4;++i){frame[i]=0xff;frame[4+i]=uint8_t(HistoryPeer>>(8*i));frame[8+i]=uint8_t(id>>(8*i));}
  frame[12]=uint8_t((3u<<5)|3u);frame[13]=8;frame[15]=uint8_t(HistoryPeer);frame[16]=0x42;
  bench::RouteMetadata metadata{};metadata.channelDecoded=true;metadata.portnum=4;
  metadata.nodeInfoValid=true;metadata.encodedDataSize=1;
  assert(service.route.ingestWithMetadata(bench::Protocol::Meshtastic,frame,sizeof(frame),metadata,100,7));
  service.route.clearPending();
  PreservedState state{service.signature.keyCount(101),service.route.historyCount(),service.route.knownCount()};
  assert(state.keys==1&&state.history==1&&state.known==1);return state;
}
static void assertUnrelatedState(const mt::Service &service,const PreservedState &state) {
  assert(service.signature.keyCount(1000)==state.keys);
  assert(service.route.historyCount()==state.history&&service.route.knownCount()==state.known);
}
}

static meshtastic_AdminMessage fixed(int32_t latitude,int32_t longitude,bool altitude=false,int32_t metres=0) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_fixed_position_tag;
  auto &p=a.set_fixed_position;p.has_latitude_i=true;p.latitude_i=latitude;
  p.has_longitude_i=true;p.longitude_i=longitude;
  p.has_altitude=altitude;p.altitude=altitude?metres:0;return a;
}
static meshtastic_AdminMessage emptyFixed() {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_fixed_position_tag;return a;
}
static meshtastic_AdminMessage removeFixed(bool value=true) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_remove_fixed_position_tag;
  a.remove_fixed_position=value;return a;
}
static meshtastic_AdminMessage positionConfig(bool fixedValue) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
  a.set_config.which_payload_variant=meshtastic_Config_position_tag;
  a.set_config.payload_variant.position.fixed_position=fixedValue;
  a.set_config.payload_variant.position.gps_mode=meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT;return a;
}
static meshtastic_AdminMessage display(uint32_t seconds,bool flip=false) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
  a.set_config.which_payload_variant=meshtastic_Config_display_tag;
  a.set_config.payload_variant.display.screen_on_secs=seconds;
  a.set_config.payload_variant.display.flip_screen=flip;return a;
}
static meshtastic_Config_PositionConfig getPosition(Fixture &f,bool ble=false) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;
  a.get_config_request=meshtastic_AdminMessage_ConfigType_POSITION_CONFIG;f.admin(a,ble);
  const auto reply=(ble?f.ble:f.usb).reply();assert(reply.which_payload_variant==meshtastic_AdminMessage_get_config_response_tag);
  assert(reply.get_config_response.which_payload_variant==meshtastic_Config_position_tag);
  return reply.get_config_response.payload_variant.position;
}
static const meshtastic_NodeInfo *lastLocalNode(const Host &host) {
  for(auto i=host.out.rbegin();i!=host.out.rend();++i)
    if(i->which_payload_variant==meshtastic_FromRadio_node_info_tag&&i->node_info.num==Node)return &i->node_info;
  return nullptr;
}
static void assertOnlyPosition(const meshtastic_Position &p,int32_t latitude,int32_t longitude,bool altitude=false,int32_t metres=0) {
  assert(p.has_latitude_i&&p.latitude_i==latitude&&p.has_longitude_i&&p.longitude_i==longitude);
  assert(p.has_altitude==altitude&&p.altitude==(altitude?metres:0));
  assert(!p.time&&p.location_source==meshtastic_Position_LocSource_LOC_MANUAL&&!mt::detail::enumNumber(p.altitude_source)&&
    !p.timestamp&&!p.timestamp_millis_adjust&&!p.has_altitude_hae&&!p.altitude_hae&&
    !p.has_altitude_geoidal_separation&&!p.altitude_geoidal_separation&&!p.PDOP&&!p.HDOP&&!p.VDOP&&
    !p.gps_accuracy&&!p.has_ground_speed&&!p.ground_speed&&!p.has_ground_track&&!p.ground_track&&
    !p.fix_quality&&!p.fix_type&&!p.sats_in_view&&!p.sensor_id&&!p.next_update&&!p.seq_number&&!p.precision_bits);
}
static std::vector<uint8_t> adminBytes(const meshtastic_AdminMessage &a) {
  uint8_t bytes[512]{};auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
  assert(pb_encode(&stream,meshtastic_AdminMessage_fields,&a));return {bytes,bytes+stream.bytes_written};
}
static void rawAdmin(Fixture &f,const std::vector<uint8_t> &wire,bool ble=false,uint32_t now=1) {
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=request.packet;
  p.to=Node;p.id=71;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
  assert(wire.size()<=sizeof(p.decoded.payload.bytes));memcpy(p.decoded.payload.bytes,wire.data(),wire.size());p.decoded.payload.size=wire.size();
  f.input(request,ble,now);
}
static void handshake(Fixture &f,bool ble=false) {
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;request.want_config_id=0x1818;
  f.input(request,ble,100);
}
static void sendAdmin(mt::Companion &companion,Host &host,const meshtastic_AdminMessage &a,uint32_t now=1) {
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=request.packet;
  p.to=Node;p.id=71;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
  auto payload=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
  assert(pb_encode(&payload,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=payload.bytes_written;
  uint8_t bytes[512]{};auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
  assert(pb_encode(&stream,meshtastic_ToRadio_fields,&request));host.now=now;
  companion.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});companion.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});companion.byte(stream.bytes_written>>8,now,0,mt::Companion::WorkspaceProof{true});companion.byte(stream.bytes_written,now,0,mt::Companion::WorkspaceProof{true});
  for(size_t i=0;i<stream.bytes_written;++i)companion.byte(bytes[i],now,0,mt::Companion::WorkspaceProof{true});
}

int main() {
  for(uint8_t model:{uint8_t(81),uint8_t(127)})for(bool ble:{false,true}) {
    auto f=std::make_unique<Fixture>(model);auto &host=ble?f->ble:f->usb;
    const auto initial=getPosition(*f,ble);
    assert(!initial.fixed_position&&mt::detail::enumNumber(initial.gps_mode)==meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT);
    assert(!initial.position_broadcast_secs&&!initial.position_broadcast_smart_enabled&&!initial.gps_enabled&&!initial.position_flags);
    host.out.clear();handshake(*f,ble);const auto *empty=lastLocalNode(host);assert(empty&&!empty->has_position&&!f->radio.sends);

    const auto state=seedUnrelatedState(f->service,0x18180000u+model+ble);
    host.out.clear();f->admin(fixed(449876543,-931234567,true,287),ble,200);assert(host.error()==meshtastic_Routing_Error_NONE);
    assert(f->service.config.fixedPosition()&&f->service.config.fixedAltitude()&&f->backend.writes==1&&!f->radio.sends);
    assertUnrelatedState(f->service,state);
    const auto *node=lastLocalNode(host);assert(node&&node->has_position);assertOnlyPosition(node->position,449876543,-931234567,true,287);
    const auto cross=getPosition(*f,!ble);assert(cross.fixed_position&&mt::detail::enumNumber(cross.gps_mode)==meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT);
    mt::SettingsJournal reboot;assert(!reboot.load(f->backend)&&reboot.config().fixedPosition());
    assert(reboot.config().latitudeI()==449876543&&reboot.config().longitudeI()==-931234567&&reboot.config().altitude()==287);

    host.out.clear();f->admin(removeFixed(),ble,300);assert(host.error()==meshtastic_Routing_Error_NONE);
    node=lastLocalNode(host);assert(node&&node->has_position&&!node->position.has_latitude_i&&!node->position.has_longitude_i&&
      !node->position.has_altitude&&!f->service.config.fixedPosition()&&f->backend.writes==2&&!f->radio.sends);
    assertUnrelatedState(f->service,state);
    mt::SettingsJournal removed;assert(!removed.load(f->backend)&&!removed.config().fixedPosition());
  }
  std::cout<<"PASS both boards/transports persist set/remove, preserve signature/routing history, cross-client getter, handshake and restart without RF\n";

  {
    auto f=std::make_unique<Fixture>(81);f->admin(emptyFixed());assert(f->usb.error()==0);
    const auto *node=lastLocalNode(f->usb);assert(node&&node->has_position);assertOnlyPosition(node->position,0,0);
    f->admin(fixed(0,1800000000,true,0));assert(f->usb.error()==0);node=lastLocalNode(f->usb);
    assert(node&&node->has_position);assertOnlyPosition(node->position,0,1800000000,true,0);
    f->admin(fixed(-900000000,-1800000000));assert(f->usb.error()==0);
    f->admin(fixed(900000000,1800000000));assert(f->usb.error()==0&&!f->radio.sends);
  }
  std::cout<<"PASS omitted-zero, equator, meridian, explicit zero altitude and coordinate boundaries\n";

  {
    auto f=std::make_unique<Fixture>(127);const auto original=f->service.config;const auto writes=f->backend.writes;
    for(unsigned variant=0;variant<22;++variant) {
      auto a=fixed(123456789,-987654321,true,50);auto &p=a.set_fixed_position;
      if(variant==0)p.latitude_i=900000001;
      if(variant==1)p.longitude_i=-1800000001;
      if(variant==2)p.time=1;
      if(variant==3)p.location_source=meshtastic_Position_LocSource_LOC_MANUAL;
      if(variant==4)p.altitude_source=meshtastic_Position_AltSource_ALT_MANUAL;
      if(variant==5)p.timestamp=1;
      if(variant==6)p.timestamp_millis_adjust=1;
      if(variant==7){p.has_altitude_hae=true;p.altitude_hae=1;}
      if(variant==8){p.has_altitude_geoidal_separation=true;p.altitude_geoidal_separation=1;}
      if(variant==9)p.PDOP=1;
      if(variant==10)p.HDOP=1;
      if(variant==11)p.VDOP=1;
      if(variant==12)p.gps_accuracy=1;
      if(variant==13){p.has_ground_speed=true;p.ground_speed=1;}
      if(variant==14){p.has_ground_track=true;p.ground_track=1;}
      if(variant==15)p.fix_quality=1;
      if(variant==16)p.fix_type=1;
      if(variant==17)p.sats_in_view=1;
      if(variant==18)p.sensor_id=1;
      if(variant==19)p.next_update=1;
      if(variant==20)p.seq_number=1;
      if(variant==21)p.precision_bits=32;
      f->usb.out.clear();f->admin(a);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
      assert(f->service.config.same(original)&&f->backend.writes==writes&&!f->radio.sends);
    }
    f->usb.out.clear();f->admin(removeFixed(false));assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    auto canonical=adminBytes(fixed(1,2));auto unknown=canonical;unknown.insert(unknown.end(),{0xc0,0x07,0x00});
    f->usb.out.clear();rawAdmin(*f,unknown);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    auto duplicate=canonical;duplicate.insert(duplicate.end(),canonical.begin(),canonical.end());
    f->usb.out.clear();rawAdmin(*f,duplicate);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    auto noncanonical=canonical;noncanonical[1]=uint8_t(noncanonical[1]|0x80);noncanonical.insert(noncanonical.begin()+2,0x00);
    f->usb.out.clear();rawAdmin(*f,noncanonical);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    const std::vector<uint8_t> alternateWire={0xc8,0x02,0x01};
    f->usb.out.clear();rawAdmin(*f,alternateWire);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    auto malformed=canonical;malformed.pop_back();
    f->usb.out.clear();rawAdmin(*f,malformed);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    assert(f->service.config.same(original)&&!f->radio.sends);
  }
  std::cout<<"PASS invalid metadata, ranges, false remove, unknown, duplicate and noncanonical wires refuse atomically\n";

  {
    auto f=std::make_unique<Fixture>(81);f->admin(positionConfig(false));assert(f->usb.error()==0&&!f->backend.writes);
    const auto state=seedUnrelatedState(f->service,0x18180100);
    f->admin(positionConfig(true));assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&!f->backend.writes);
    f->admin(fixed(111111111,222222222));assert(f->usb.error()==0&&f->backend.writes==1);
    assertUnrelatedState(f->service,state);
    f->admin(positionConfig(true));assert(f->usb.error()==0&&f->backend.writes==1);
    auto smart=positionConfig(true);smart.set_config.payload_variant.position.position_broadcast_smart_enabled=true;
    f->admin(smart);assert(f->usb.error()==meshtastic_Routing_Error_NONE&&f->backend.writes==2);
    auto smartReadback=getPosition(*f);const auto smartStored=f->service.config.gnssSettings();
    assert(smartReadback.fixed_position&&smartReadback.position_broadcast_smart_enabled&&smartStored.smart);
    assert(f->service.config.fixedPosition()&&f->service.config.latitudeI()==111111111&&f->service.config.longitudeI()==222222222);
    f->admin(smart);assert(f->usb.error()==meshtastic_Routing_Error_NONE&&f->backend.writes==2);
    auto regular=smart;regular.set_config.payload_variant.position.position_broadcast_secs=900;
    f->admin(regular);assert(f->usb.error()==0&&f->backend.writes==3&&getPosition(*f).position_broadcast_secs==900);
    f->admin(regular);assert(f->usb.error()==0&&f->backend.writes==3);
    f->admin(positionConfig(false));assert(f->usb.error()==0&&!f->service.config.fixedPosition()&&f->backend.writes==4);
    assertUnrelatedState(f->service,state);
  }
  std::cout<<"PASS bounded PositionConfig read/write preserves signature/routing history and cannot enable GNSS or fixed mode without coordinates\n";

  {
    auto f=std::make_unique<Fixture>(81);auto cfg=f->service.config;cfg.bytes[4]=1;cfg.setTxEnabled(true);
    assert(!f->edit.save(cfg,1)&&f->service.start(false,2)&&f->service.armEndpoint(true));
    const auto writes=f->backend.writes,starts=f->radio.starts,stops=f->radio.stops;
    text(*f,0x18180001,3);assert(f->service.endpointFree()==3);
    f->usb.out.clear();f->admin(fixed(100,200),false,4);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    assert(!f->service.config.fixedPosition()&&f->service.endpointFree()==3&&f->service.endpointArmed());
    assert(f->backend.writes==writes&&f->radio.starts==starts&&f->radio.stops==stops&&!f->radio.sends);
  }
  std::cout<<"PASS queued TX blocks storage transition without discarding queue, arm or retry state\n";

  {
    auto f=std::make_unique<Fixture>(127);auto cfg=f->service.config;cfg.bytes[4]=1;cfg.setTxEnabled(true);
    assert(!f->edit.save(cfg,1)&&f->service.start(false,2)&&f->service.armEndpoint(true));const auto writes=f->backend.writes;
    const auto state=seedUnrelatedState(f->service,0x18180200);
    f->begin(10);assert(!f->service.endpointArmed());f->usb.out.clear();f->admin(fixed(333,444,true,-12),false,11);
    assert(f->usb.error()==0&&f->edit.dirty()&&f->service.config.fixedPosition()&&f->backend.writes==writes);
    assertUnrelatedState(f->service,state);
    const auto *live=lastLocalNode(f->usb);assert(live&&live->has_position);assertOnlyPosition(live->position,333,444,true,-12);
    meshtastic_ToRadio disconnect{};disconnect.which_payload_variant=meshtastic_ToRadio_disconnect_tag;disconnect.disconnect=true;
    f->input(disconnect,false,12);assert(f->edit.open()&&f->edit.dirty()&&f->backend.writes==writes);
    f->commit();assert(f->usb.error()==0&&!f->edit.open()&&f->backend.writes==writes+1&&f->service.endpointArmed());
    assertUnrelatedState(f->service,state);
    mt::SettingsJournal reboot;assert(!reboot.load(f->backend)&&reboot.config().fixedPosition()&&reboot.config().altitude()==-12);
  }
  std::cout<<"PASS open batch exposes pending position, preserves signature/routing history through commit, verifies durability and restores TX intent\n";

  {
    auto f=std::make_unique<Fixture>(81);const auto state=seedUnrelatedState(f->service,0x18180210);
    f->begin(100);f->usb.out.clear();f->admin(fixed(12,34),false,101);assert(f->usb.error()==0);
    f->usb.out.clear();f->admin(removeFixed(),false,102);assert(f->usb.error()==0&&!f->service.config.fixedPosition());
    assert(f->edit.dirty()&&!f->backend.writes);assertUnrelatedState(f->service,state);
    f->commit();assert(f->usb.error()==0&&!f->edit.open()&&f->backend.writes==1);assertUnrelatedState(f->service,state);
  }
  {
    auto f=std::make_unique<Fixture>(81);const auto state=seedUnrelatedState(f->service,0x18180211);
    f->begin(200);f->usb.out.clear();f->admin(fixed(56,78),false,201);assert(f->usb.error()==0);
    f->usb.out.clear();f->admin(positionConfig(false),false,202);assert(f->usb.error()==0&&!f->service.config.fixedPosition());
    assert(f->edit.dirty()&&!f->backend.writes);assertUnrelatedState(f->service,state);
    assert(!f->edit.poll(202+mt::SettingsEdit::IdleMs,true)&&!f->edit.open()&&f->backend.writes==1);
    assertUnrelatedState(f->service,state);
  }
  std::cout<<"PASS position set/remove round trips to durable state preserve histories through explicit and idle commits\n";

  {
    auto f=std::make_unique<Fixture>(81);f->begin(300);f->usb.out.clear();f->owner("Changed owner",false,301);assert(f->usb.error()==0);
    f->usb.out.clear();f->admin(fixed(90,123),false,302);assert(f->usb.error()==0);
    f->usb.out.clear();f->owner("ZephTastic",false,303);assert(f->usb.error()==0);
    const auto restored=f->service.config.node();
    assert(!strcmp(reinterpret_cast<const char *>(restored.bytes+bench::MtNodeRecord::Name),"ZephTastic")&&
      f->service.config.fixedPosition());
    f->commit();assert(f->usb.error()==0&&!f->edit.open());
  }
  {
    auto f=std::make_unique<Fixture>(127);f->begin(400);f->usb.out.clear();f->admin(display(30,true),false,401);assert(f->usb.error()==0);
    f->usb.out.clear();f->admin(fixed(90,123),false,402);assert(f->usb.error()==0);
    f->usb.out.clear();f->admin(display(15,false),false,403);assert(f->usb.error()==0);
    assert(f->service.config.displaySeconds()==15&&!f->service.config.displayFlip()&&f->service.config.fixedPosition());
    f->commit();assert(f->usb.error()==0&&!f->edit.open());
  }
  std::cout<<"PASS mixed owner/display and position batches restore non-position fields through the full apply path\n";

  {
    auto f=std::make_unique<Fixture>(81);f->begin(20);f->usb.out.clear();f->admin(fixed(555,666),false,21);
    assert(f->usb.error()==0&&f->edit.dirty()&&!f->backend.writes);f->service.stop();
    assert(!f->edit.poll(21+mt::SettingsEdit::IdleMs,true)&&!f->edit.open()&&f->backend.writes==1);
    mt::SettingsJournal reboot;assert(!reboot.load(f->backend)&&reboot.config().latitudeI()==555);
  }
  std::cout<<"PASS qualified idle commit persists a pending fixed position after verified RF-off quiescence\n";

  {
    auto f=std::make_unique<Fixture>(81);f->admin(fixed(777,888));assert(f->usb.error()==0);
    const auto durable=f->service.config;f->backend.fail=true;f->backend.uncertain=false;
    f->usb.out.clear();f->admin(removeFixed());assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE);
    assert(f->service.config.same(durable)&&f->service.config.fixedPosition()&&!f->journal.ready()&&!f->radio.sends);
  }
  {
    auto f=std::make_unique<Fixture>(81);f->begin(30);f->usb.out.clear();f->admin(fixed(999,111),false,31);assert(f->usb.error()==0);
    f->backend.fail=true;f->backend.uncertain=true;f->usb.out.clear();f->commit();assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE);
    assert(f->edit.open()&&f->service.config.fixedPosition()&&!f->journal.ready()&&!f->radio.sends);
    f->backend.fail=false;mt::SettingsJournal reboot;assert(!reboot.load(f->backend)&&reboot.config().fixedPosition());
  }
  std::cout<<"PASS failed remove retains old live value; uncertain batch commit is not acknowledged durable and resolves on reload\n";

  {
    for(unsigned profile=0;profile<4;++profile) {
      Radio radio;Backend backend;mt::SettingsJournal journal;assert(!journal.load(backend));
      PrivatePositionRuntime runtime;const bool normal=profile==0||profile==3,allow=profile!=1;
      mt::Service service(radio,allow,Node,81,normal,false,profile==3?&runtime:nullptr);
      mt::SettingsEdit edit(service,journal,backend);Host host(edit,journal);mt::companion::CompanionWorkspace workspace;mt::Companion companion(service,host,81,workspace);
      const auto state=seedUnrelatedState(service,0x18180300u+profile);
      meshtastic_AdminMessage a=fixed(123,456);sendAdmin(companion,host,a);
      assert(host.error()==0&&service.config.fixedPosition()&&backend.writes==1&&!radio.sends);
      assertUnrelatedState(service,state);assert(service.privateConfigured()==(profile==3));
    }
    auto f=std::make_unique<Fixture>(81);f->provision();assert(f->service.start(false,0));f->radio.state=0;f->service.rx(nullptr,0,0,0,0);
    const auto old=f->service.config;f->admin(fixed(123,456));assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    assert(f->service.faulted()&&f->service.config.same(old)&&!f->radio.sends);
  }
  std::cout<<"PASS normal, RX-only, one-packet and private-load boundaries preserve unrelated history without TX; faulted service refuses without mutation\n";

  {
    auto f=std::make_unique<Fixture>(81);seedUnrelatedState(f->service,0x18180400);
    f->owner("Changed owner");assert(f->usb.error()==0);
    assert(!f->service.signature.keyCount(1000)&&!f->service.route.historyCount()&&!f->service.route.knownCount());
  }
  std::cout<<"PASS a mixed/non-position settings change still takes full reconfiguration and clears stale histories\n";
}
