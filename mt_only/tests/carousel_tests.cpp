// SPDX-License-Identifier: GPL-3.0-only
// M20 DisplayConfig carousel persistence/admission tests. Memory only: no
// physical display, transport, radio or credential API is opened.
#include "device_config_fixture.h"

static meshtastic_AdminMessage display(uint32_t timeout=15,bool flip=false,uint32_t carousel=0) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
  a.set_config.which_payload_variant=meshtastic_Config_display_tag;
  auto &d=a.set_config.payload_variant.display;d.screen_on_secs=timeout;d.flip_screen=flip;
  d.auto_screen_carousel_secs=carousel;return a;
}
static meshtastic_Config_DisplayConfig displayGetter(Fixture &f,bool ble=false) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;
  a.get_config_request=meshtastic_AdminMessage_ConfigType_DISPLAY_CONFIG;f.admin(a,ble);
  const auto reply=(ble?f.ble:f.usb).reply();assert(reply.which_payload_variant==meshtastic_AdminMessage_get_config_response_tag);
  assert(reply.get_config_response.which_payload_variant==meshtastic_Config_display_tag);
  return reply.get_config_response.payload_variant.display;
}
static std::vector<uint8_t> adminBytes(const meshtastic_AdminMessage &a) {
  uint8_t bytes[512]{};auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
  assert(pb_encode(&stream,meshtastic_AdminMessage_fields,&a));return {bytes,bytes+stream.bytes_written};
}
static void rawAdmin(Fixture &f,const std::vector<uint8_t> &wire,bool ble=false,uint32_t now=1) {
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=request.packet;
  p.to=Node;p.id=0x20200001;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
  assert(wire.size()<=sizeof(p.decoded.payload.bytes));memcpy(p.decoded.payload.bytes,wire.data(),wire.size());p.decoded.payload.size=wire.size();
  f.input(request,ble,now);
}
static uint32_t crc32(const uint8_t *p,size_t n) {
  uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;
}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
static void varint(std::vector<uint8_t> &out,uint64_t value) {
  do {uint8_t byte=uint8_t(value&0x7f);value>>=7;out.push_back(byte|(value?0x80:0));} while(value);
}
static std::vector<uint8_t> rawDisplayField(unsigned field,uint64_t value) {
  std::vector<uint8_t> displayWire{0x08,0x0f};varint(displayWire,uint64_t(field)<<3);varint(displayWire,value);
  std::vector<uint8_t> config{0x2a};varint(config,displayWire.size());config.insert(config.end(),displayWire.begin(),displayWire.end());
  std::vector<uint8_t> admin{0x92,0x02};varint(admin,config.size());admin.insert(admin.end(),config.begin(),config.end());return admin;
}
struct State {
  mt::Config config,journalConfig;std::map<std::string,std::vector<uint8_t>> durable;
  unsigned writes,usbSaves,bleSaves,starts,stops,sends,free,nodes,keys;
  bool armed,receiving,faulted,nodeDirty;mt::SettingsJournal::State journalState;
  uint32_t generation,knownPeer,knownAge;int activeSlot;bool knownDirect;
  size_t routePending,retransmitting,routeKnown,routeCount,routeHistory,routeOpaque;
};
static State state(const Fixture &f) {
  uint32_t peer=0,age=0;bool direct=false;const bool observed=f.service.route.knownObservation(0,101,peer,age,direct);
  assert(observed==(f.service.route.knownCount()!=0));
  return {f.service.config,f.journal.config(),f.backend.entries,
    f.backend.writes,f.usb.saves,f.ble.saves,f.radio.starts,f.radio.stops,f.radio.sends,
    f.service.endpointFree(),f.service.nodes.count(),f.service.signature.keyCount(101),
    f.service.endpointArmed(),f.service.receiving(),f.service.faulted(),f.service.nodes.dirty(),f.journal.state(),
    f.journal.generation(),peer,age,f.journal.activeSlot(),direct,
    f.service.route.pending(),f.service.route.retransmitting(),f.service.route.knownCount(),f.service.route.routeCount(),
    f.service.route.historyCount(),f.service.route.opaqueCount()};
}
static void same(const Fixture &f,const State &s) {
  uint32_t peer=0,age=0;bool direct=false;const bool observed=f.service.route.knownObservation(0,101,peer,age,direct);
  assert(f.service.config.same(s.config)&&f.journal.config().same(s.journalConfig)&&f.backend.entries==s.durable&&
    f.backend.writes==s.writes&&f.usb.saves==s.usbSaves&&f.ble.saves==s.bleSaves&&
    f.radio.starts==s.starts&&f.radio.stops==s.stops&&f.radio.sends==s.sends&&f.service.endpointFree()==s.free&&
    f.service.nodes.count()==s.nodes&&f.service.endpointArmed()==s.armed&&f.service.receiving()==s.receiving&&
    f.service.faulted()==s.faulted&&f.service.nodes.dirty()==s.nodeDirty&&f.service.signature.keyCount(101)==s.keys&&
    f.journal.state()==s.journalState&&f.journal.generation()==s.generation&&f.journal.activeSlot()==s.activeSlot&&
    observed==(s.routeKnown!=0)&&peer==s.knownPeer&&age==s.knownAge&&direct==s.knownDirect&&
    f.service.route.pending()==s.routePending&&
    f.service.route.retransmitting()==s.retransmitting&&f.service.route.knownCount()==s.routeKnown&&
    f.service.route.routeCount()==s.routeCount&&f.service.route.historyCount()==s.routeHistory&&
    f.service.route.opaqueCount()==s.routeOpaque);
}
static void correlatedBadRequest(const Host &host) {
  bool notice=false,ack=false;
  for(const auto &record:host.out) {
    if(record.which_payload_variant==meshtastic_FromRadio_clientNotification_tag) {
      const auto &n=record.clientNotification;notice|=n.has_reply_id&&n.reply_id==0x20200001&&n.level==meshtastic_LogRecord_Level_ERROR;
    } else if(record.which_payload_variant==meshtastic_FromRadio_packet_tag) {
      const auto &p=record.packet;if(p.decoded.portnum!=meshtastic_PortNum_ROUTING_APP||p.decoded.request_id!=0x20200001)continue;
      meshtastic_Routing r{};auto stream=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);
      ack|=pb_decode(&stream,meshtastic_Routing_fields,&r)&&r.error_reason==meshtastic_Routing_Error_BAD_REQUEST;
    }
  }
  assert(notice&&ack);
}
static void sealFormer(std::vector<uint8_t> &config) {
  assert(config.size()==mt::Config::FormerSize);const auto sum=mt::Config::checksum(config.data(),config.size()-2);
  config[config.size()-2]=uint8_t(sum);config.back()=uint8_t(sum>>8);
}
static std::vector<uint8_t> formerRecord(const std::vector<uint8_t> &config,uint32_t generation) {
  assert(config.size()==mt::Config::FormerSize);
  std::vector<uint8_t> record(mt::SettingsJournal::FormerRecordSize,0);record[0]='Z';record[1]='T';record[2]='J';record[3]=2;
  put32(record.data()+4,generation);record[8]=config.size();memcpy(record.data()+12,config.data(),config.size());
  put32(record.data()+record.size()-4,crc32(record.data(),record.size()-4));return record;
}
static std::vector<uint8_t> populatedV4(bool publicChannel) {
  mt::Config old;old.bytes[4]=1;auto node=old.node();assert(node.setName(publicChannel?"M20 preserved public":"M20 preserved disabled",false));
  assert(node.setName(publicChannel?"PUB":"OFF",true));if(publicChannel)node.setPublic();else node.setKey(0,nullptr,0);old.node(node);
  old.setTxEnabled(true);assert(old.setDisplay(42,true,0));assert(old.setFixedPosition(123456789,-987654321,true,-321));
  std::vector<uint8_t> bytes(old.bytes,old.bytes+mt::Config::FormerSize);bytes[2]=4;memset(bytes.data()+97,0,13);sealFormer(bytes);
  mt::Config image;memcpy(image.bytes,bytes.data(),bytes.size());assert(image.validBody(4));return bytes;
}
static std::vector<uint8_t> outboundText(Fixture &f,uint32_t id) {
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;data.has_bitfield=true;
  data.payload.size=2;data.payload.bytes[0]='q';data.payload.bytes[1]='!';
  uint8_t encoded[64]{};auto stream=pb_ostream_from_buffer(encoded,sizeof(encoded));assert(pb_encode(&stream,meshtastic_Data_fields,&data));
  std::vector<uint8_t> wire(255);size_t length=0;
  assert(f.service.codec.encodeZeroHopData(Node,0x87654321,id,encoded,stream.bytes_written,wire.data(),wire.size(),length));
  wire.resize(length);wire[12]=0;return wire;
}
static void seedRoutingHistory(Fixture &f,uint32_t salt) {
  const uint32_t decodedPeer=0x87650000u|(salt&0xffff),opaquePeer=0x76540000u|(salt&0xffff);
  const uint8_t publicKey[32]={9};assert(f.service.signature.trustPublicKey(decodedPeer,publicKey,100));
  uint8_t decoded[17]{};put32(decoded,0xffffffffu);put32(decoded+4,decodedPeer);put32(decoded+8,0x51000000u|salt);
  decoded[12]=uint8_t((3u<<5)|3u);decoded[13]=8;decoded[15]=uint8_t(decodedPeer);decoded[16]=0x42;
  bench::RouteMetadata metadata{};metadata.channelDecoded=true;metadata.portnum=4;metadata.nodeInfoValid=true;metadata.encodedDataSize=1;
  assert(f.service.route.ingestWithMetadata(bench::Protocol::Meshtastic,decoded,sizeof(decoded),metadata,100,7));f.service.route.clearPending();
  uint8_t opaque[17]{};put32(opaque,0xffffffffu);put32(opaque+4,opaquePeer);put32(opaque+8,0x52000000u|salt);
  opaque[12]=uint8_t((3u<<5)|3u);opaque[13]=8;opaque[15]=uint8_t(opaquePeer);opaque[16]=0x24;
  metadata={};metadata.decodeRejected=true;
  assert(f.service.route.ingestWithMetadata(bench::Protocol::Meshtastic,opaque,sizeof(opaque),metadata,100,11));f.service.route.clearPending();
  assert(f.service.route.historyCount()==1&&f.service.route.opaqueCount()==1&&f.service.route.knownCount()==1&&f.service.signature.keyCount(101)==1);
}
static std::vector<uint8_t> preload(Fixture &f,uint32_t salt) {
  auto config=f.service.config;config.bytes[4]=1;config.setTxEnabled(true);assert(!f.edit.save(config,1));
  assert(f.journal.generation()==1&&f.journal.activeSlot()==0&&f.backend.entries.count("mtbase/state0")==1);
  assert(f.service.start(false,2)&&f.service.armEndpoint(true));auto queued=outboundText(f,0x53000000u|salt);
  assert(!f.service.submitMessage(0,queued.data(),queued.size(),false,3)&&f.service.endpointFree()==3&&f.service.endpointArmed());
  seedRoutingHistory(f,salt);return queued;
}
static void sameQueuedMessage(Fixture &f,const std::vector<uint8_t> &queued) {
  auto changed=queued;changed.back()^=1;
  // -EINVAL distinguishes the still-live same-ID job with different bytes from
  // a cleared queue whose reserved nonce would instead return -EALREADY.
  assert(f.service.submitMessage(0,changed.data(),changed.size(),false,4)==-EINVAL&&f.service.endpointFree()==3);
}

int main() {
  for(bool ble:{false,true})for(uint8_t model:{uint8_t(81),uint8_t(127)}) {
    auto f=std::make_unique<Fixture>(model);auto initial=displayGetter(*f,ble);
    assert(initial.screen_on_secs==15&&!initial.flip_screen&&!initial.auto_screen_carousel_secs);
    auto &host=ble?f->ble:f->usb;host.out.clear();f->admin(display(15,false,0),ble);
    assert(host.error()==meshtastic_Routing_Error_NONE&&!f->backend.writes&&!host.saves);
    host.out.clear();f->admin(display(60,true,30),ble);
    if(model==81) {
      assert(host.error()==meshtastic_Routing_Error_BAD_REQUEST&&!f->backend.writes&&!host.saves);
      assert(f->service.config.displaySeconds()==15&&!f->service.config.displayCarouselSeconds());
      // The pinned router projection stores a one-second duration even on a bare XIAO.
      assert(f->service.config.setDisplay(1,false,0));
      const auto projected=displayGetter(*f,ble);
      assert(projected.screen_on_secs==1);
      host.out.clear();f->admin(display(15,false,0),ble);
      assert(host.error()==meshtastic_Routing_Error_NONE&&f->backend.writes==1&&host.saves==1);
      assert(displayGetter(*f,!ble).screen_on_secs==15);
      mt::SettingsJournal reboot;assert(!reboot.load(f->backend)&&reboot.config().displaySeconds()==15);
      for(unsigned option=0;option<3;++option){
        auto setting=display(15,option==0,option==1?30:0);
        setting.set_config.payload_variant.display.use_long_node_name=option==2;
        host.out.clear();f->admin(setting,ble);
        assert(host.error()==meshtastic_Routing_Error_BAD_REQUEST&&f->backend.writes==1&&host.saves==1);
      }
    } else {
      assert(host.error()==meshtastic_Routing_Error_NONE&&f->backend.writes==1&&host.saves==1);
      assert(f->service.config.displaySeconds()==60&&f->service.config.displayFlip()&&f->service.config.displayCarouselSeconds()==30);
      const auto cross=displayGetter(*f,!ble);assert(cross.screen_on_secs==60&&cross.flip_screen&&cross.auto_screen_carousel_secs==30);
      mt::SettingsJournal reboot;assert(!reboot.load(f->backend)&&reboot.config().displayCarouselSeconds()==30);
    }
    assert(!f->radio.sends);
  }
  std::cout<<"PASS both transports and boards expose zero default; T096 persists carousel while headless XIAO rejects activation\n";

  for(bool ble:{false,true})for(uint8_t model:{uint8_t(81),uint8_t(127)}) {
    auto f=std::make_unique<Fixture>(model);auto &host=ble?f->ble:f->usb;
    const auto queued=preload(*f,0x100u*model+unsigned(ble));host.out.clear();const auto before=state(*f);
    for(unsigned variant=0;variant<13;++variant) {
      if(variant==10)continue; // M46 implements this setting; positive/negative coverage lives in display_names_tests.cpp.
      auto a=display(15,false,variant==0?86401:30);auto &d=a.set_config.payload_variant.display;
      if(variant==1)d.gps_format=static_cast<meshtastic_Config_DisplayConfig_DeprecatedGpsCoordinateFormat>(1);
      if(variant==2)d.compass_north_top=true;
      if(variant==3)d.units=meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL;
      if(variant==4)d.oled=meshtastic_Config_DisplayConfig_OledType_OLED_SSD1306;
      if(variant==5)d.displaymode=meshtastic_Config_DisplayConfig_DisplayMode_COLOR;
      if(variant==6)d.heading_bold=true;
      if(variant==7)d.wake_on_tap_or_motion=true;
      if(variant==8)d.compass_orientation=meshtastic_Config_DisplayConfig_CompassOrientation_DEGREES_90;
      if(variant==9)d.use_12h_clock=true;
      if(variant==10)d.use_long_node_name=true; // changed setting still refuses while queued/busy
      if(variant==11)d.enable_message_bubbles=true;
      if(variant==12)d.screen_on_secs=86401;
      host.out.clear();f->admin(a,ble);assert(host.error()==meshtastic_Routing_Error_BAD_REQUEST);same(*f,before);sameQueuedMessage(*f,queued);
    }
    auto canonical=adminBytes(display(15,false,30));
    auto unknown=canonical;unknown.insert(unknown.end(),{0xc0,0x07,0x00});
    auto duplicate=canonical;duplicate.insert(duplicate.end(),canonical.begin(),canonical.end());
    auto malformed=canonical;malformed.pop_back();
    std::vector<std::vector<uint8_t>> rawInvalid={
      unknown,duplicate,malformed,
      {0x92,0x02,0x07,0x2a,0x05,0x08,0x0f,0x18,0x9e,0x00}, // overlong 30
      {0x92,0x02,0x04,0x2a,0x02,0x18,0x00},                // explicit zero
      {0x92,0x02,0x08,0x2a,0x06,0x08,0x0f,0x18,0x1e,0x18,0x1e}, // duplicate field
      {0x92,0x02,0x08,0x2a,0x06,0x08,0x0f,0x18,0x1e,0x78,0x00}, // unknown field
      {0x92,0x02,0x06,0x2a,0x04,0x08,0x0f,0x18,0x1e,0xd0,0x02,0x01}, // alternate admin oneof
      {0x92,0x02,0x0a,0x2a,0x08,0x08,0x0f,0x18,0xff,0xff,0xff,0xff,0x0f}, // UINT32_MAX
      {0x92,0x02,0x0a,0x2a,0x08,0x08,0x0f,0x18,0x80,0x80,0x80,0x80,0x10}  // uint32 overflow
    };
    // Nanopb's generated C fields are enum-typed and may be short under target
    // flags. Inject hostile integer wire values directly so no C++ cast can
    // truncate them before the production decoder/policy sees them.
    const uint64_t hostileValues[]={uint64_t(1),uint64_t(255),uint64_t(256),uint64_t(65535),uint64_t(65536),
      uint64_t(0xffffffff),uint64_t(0x100000000)};
    for(unsigned field:{2u,6u,7u,8u,11u})for(uint64_t value:hostileValues)rawInvalid.push_back(rawDisplayField(field,value));
    for(const auto &wire:rawInvalid) {
      host.out.clear();rawAdmin(*f,wire,ble);assert(host.error()==meshtastic_Routing_Error_BAD_REQUEST);
      correlatedBadRequest(host);same(*f,before);sameQueuedMessage(*f,queued);
    }
  }
  std::cout<<"PASS both boards/transports reject bounded-field, hostile-enum, overflow, alternate, explicit-zero, noncanonical, unknown, duplicate and malformed wires with correlated errors while preserving exact durable journal, armed endpoint queue, routing/duplicate/opaque histories and NodeDB state\n";

  for(bool publicChannel:{false,true}) {
    const auto v4=populatedV4(publicChannel);mt::Config migrated;
    assert(mt::Config::decode(v4.data(),v4.size(),migrated)&&migrated.valid()&&migrated.bytes[2]==mt::Config::Version);
    assert(!memcmp(migrated.bytes+3,v4.data()+3,94)&&!migrated.displayCarouselSeconds()&&migrated.role()==1&&migrated.rebroadcastMode()==0&&!migrated.primaryName()[0]);
    assert(migrated.node().isPublic()==publicChannel&&migrated.provisioned()==publicChannel&&migrated.txEnabled()&&
      migrated.displaySeconds()==42&&migrated.displayFlip()&&migrated.fixedPosition()&&migrated.fixedAltitude()&&
      migrated.latitudeI()==123456789&&migrated.longitudeI()==-987654321&&migrated.altitude()==-321);
    for(unsigned slot:{0u,1u}) {
      Backend backend;backend.entries[slot?"mtbase/state1":"mtbase/state0"]=formerRecord(v4,7);mt::SettingsJournal journal;
      assert(!journal.load(backend)&&journal.generation()==7&&journal.activeSlot()==int(slot)&&journal.config().same(migrated));
    }
    auto invalid=v4;invalid[97]=1;sealFormer(invalid);auto unchanged=migrated;
    assert(!mt::Config::decode(invalid.data(),invalid.size(),migrated)&&migrated.same(unchanged));
    auto future=migrated;future.bytes[2]=mt::Config::Version+1;future.seal();assert(!mt::Config::decode(future.bytes,future.Size,migrated)&&migrated.same(unchanged));
    // A newer corrupt/future slot must recover the complete earlier v4 record.
    for(bool futureVersion:{false,true}) {
      Backend backend;backend.entries["mtbase/state0"]=formerRecord(v4,7);
      auto bad=formerRecord(v4,8);if(futureVersion){bad[14]=12;const size_t configSize=bad[8];const auto sum=mt::Config::checksum(bad.data()+12,configSize-2);bad[12+configSize-2]=uint8_t(sum);bad[12+configSize-1]=uint8_t(sum>>8);put32(bad.data()+bad.size()-4,crc32(bad.data(),bad.size()-4));}
      else bad[31]^=1;
      backend.entries["mtbase/state1"]=bad;mt::SettingsJournal recovered;
      assert(!recovered.load(backend)&&recovered.state()==mt::SettingsJournal::State::Recovered&&recovered.generation()==7&&recovered.activeSlot()==0&&recovered.config().same(unchanged));
    }
  }
  {
    mt::Config invalid;mt::Config::put32(invalid.bytes+97,86401);invalid.seal();assert(!invalid.valid());
  }
  std::cout<<"PASS populated Public/disabled v4 direct and both-slot migrations preserve owner/channel/US/TX/display/fixed-position fields; bounds, reserved, future and corrupt records fail or recover without output mutation\n";

  {
    auto f=std::make_unique<Fixture>(127);auto cfg=f->service.config;cfg.bytes[4]=1;cfg.setTxEnabled(true);
    assert(!f->edit.save(cfg,1)&&f->service.start(false,2)&&f->service.armEndpoint(true));
    const auto writes=f->backend.writes,starts=f->radio.starts,stops=f->radio.stops;
    text(*f,0x20200002,3);assert(f->service.endpointFree()==3);
    f->usb.out.clear();f->admin(display(),false,4);assert(f->usb.error()==0&&f->service.endpointFree()==3&&f->service.endpointArmed());
    assert(f->backend.writes==writes&&f->radio.starts==starts&&f->radio.stops==stops);
    // A carousel period of 10 seconds is a supported setting on this model.
    // Use an actually out-of-range value to exercise BAD_REQUEST while queued;
    // this Host intentionally has no remote config commit adapter.
    f->usb.out.clear();f->admin(display(15,false,86401),false,5);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    assert(f->service.endpointFree()==3&&f->service.endpointArmed()&&f->backend.writes==writes&&!f->radio.sends);
  }
  std::cout<<"PASS unchanged display save preserves queued TX; invalid display update is rejected before queue, arm, radio or storage mutation\n";

  for(bool uncertain:{false,true}) {
    auto f=std::make_unique<Fixture>(127);f->begin(100);f->usb.out.clear();f->admin(display(20,true,7),false,101);
    assert(f->usb.error()==0&&f->edit.dirty()&&!f->backend.writes&&f->service.config.displayCarouselSeconds()==7);
    const auto live=displayGetter(*f,true);assert(live.screen_on_secs==20&&live.flip_screen&&live.auto_screen_carousel_secs==7);
    f->backend.fail=true;f->backend.uncertain=uncertain;f->usb.out.clear();f->commit();
    assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&f->edit.open()&&f->edit.dirty()&&!f->journal.ready());
    const auto writes=f->backend.writes;f->commit();assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&f->backend.writes==writes);
    mt::SettingsJournal reboot;assert(!reboot.load(f->backend));
    assert(reboot.config().displayCarouselSeconds()==(uncertain?7u:0u));
  }
  std::cout<<"PASS batch live readback, failed/uncertain durability and no-blind-retry semantics cover carousel\n";
}
