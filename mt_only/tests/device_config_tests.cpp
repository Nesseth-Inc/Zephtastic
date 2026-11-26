// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
static void group(const char *s){std::cout<<"PASS "<<s<<'\n';}
int main() {
 {
  auto hasNotice=[](const Host &host,uint32_t id) {
   for(const auto &record:host.out)if(record.which_payload_variant==meshtastic_FromRadio_clientNotification_tag) {
    const auto &n=record.clientNotification;
    if(n.has_reply_id&&n.reply_id==id&&n.level==meshtastic_LogRecord_Level_INFO&&
       strstr(n.message,"power-saving")&&strstr(n.message,"optional sensor defaults"))return true;
   }
   return false;
  };
  auto hasAck=[](const Host &host,uint32_t id) {
   for(const auto &record:host.out)if(record.which_payload_variant==meshtastic_FromRadio_packet_tag&&
       record.packet.decoded.request_id==id&&record.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP) {
    meshtastic_Routing routing{};auto stream=pb_istream_from_buffer(record.packet.decoded.payload.bytes,record.packet.decoded.payload.size);
    return pb_decode(&stream,meshtastic_Routing_fields,&routing)&&routing.error_reason==meshtastic_Routing_Error_NONE;
   }
   return false;
  };
  auto f=std::make_unique<Fixture>();auto request=device(10800);
  request.set_config.payload_variant.device.role=meshtastic_Config_DeviceConfig_Role_ROUTER;
  f->usb.overrideDeviceSave=true;f->usb.deviceSaveResult=mt::Companion::Host::DeviceDefaultsPartial;f->admin(request);
  assert(hasAck(f->usb,71)&&hasNotice(f->usb,71));
  f->usb.out.clear();request.set_config.payload_variant.device.role=meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE;
  request.set_config.payload_variant.device.node_info_broadcast_secs=7200;
  f->usb.deviceSaveResult=0;f->admin(request);
  assert(hasAck(f->usb,71)&&!hasNotice(f->usb,71));
  f->usb.out.clear();request.set_config.payload_variant.device.role=meshtastic_Config_DeviceConfig_Role_ROUTER;
  f->usb.deviceSaveResult=-EIO;f->admin(request);
  assert(!hasAck(f->usb,71)&&!hasNotice(f->usb,71));
  group("partial ROUTER default notice is request-correlated, keeps success ACK, and excludes ordinary success/failure");
 }
 {
  enum class E8:uint8_t {Zero=0,One=1};enum class E16:uint16_t {Zero=0,One=1};enum class E32:uint32_t {Zero=0,One=1};
  for(uint32_t value:{0u,1u,99u,255u,256u,257u,65535u,65536u,UINT32_MAX}) {
   const uint8_t u8=uint8_t(value);const uint16_t u16=uint16_t(value);E8 e8;E16 e16;E32 e32;
   memcpy(&e8,&u8,sizeof(e8));memcpy(&e16,&u16,sizeof(e16));memcpy(&e32,&value,sizeof(e32));
   assert(mt::detail::enumNumber(e8)==u8&&mt::detail::enumNumber(e16)==u16&&mt::detail::enumNumber(e32)==value);
  }
  group("representation extraction supports native one-, two- and four-byte enum ABIs");
 }
 for(uint8_t model:{81,127}) {
  auto f=std::make_unique<Fixture>(model);assert(getter(*f)==10800);
  for(uint32_t input:{0u,1u,3599u,3600u,3601u,86400u,86401u,604800u,2147483u}) {
   auto before=f->service.config;f->admin(device(input));assert(f->usb.error()==0);
   auto want=input<3600?3600:input;assert(getter(*f,true)==want&&f->service.config.valid());
   auto expected=before;assert(expected.setNodeInfoSeconds(input)&&expected.same(f->service.config));
   mt::SettingsJournal reload;assert(reload.load(f->backend)==0&&reload.config().same(expected));
  }
  assert(!f->radio.sends&&!f->radio.starts&&f->service.route.identity()==Node&&!f->service.endpointArmed());
  group("actual interval getter, stock minimum clamp, durable boundary writes on both boards");
 }
 {
  auto f=std::make_unique<Fixture>();const auto before=f->service.config;
  for(unsigned field=0;field<13;++field) {
   auto a=device();auto &d=a.set_config.payload_variant.device;
   switch(field) {
   case 0:d.role=meshtastic_Config_DeviceConfig_Role_TRACKER;break;
   case 1:d.serial_enabled=false;break;case 2:d.led_heartbeat_disabled=false;break;
   case 3:d.button_gpio=1;break;case 4:d.buzzer_gpio=1;break;
   case 5:d.rebroadcast_mode=meshtastic_Config_DeviceConfig_RebroadcastMode_ALL_SKIP_DECODING;break;
   case 6:d.double_tap_as_button_press=true;break;case 7:d.is_managed=true;break;
   case 8:d.disable_triple_click=true;break;case 9:strcpy(d.tzdef,"UTC0");break;
   case 10:d.buzzer_mode=meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED;break;
   case 11:d.node_info_broadcast_secs=2147484;break;case 12:d.node_info_broadcast_secs=UINT32_MAX;break;
   }
   f->admin(a,field&1);assert((field&1?f->ble:f->usb).error()==meshtastic_Routing_Error_BAD_REQUEST);
   assert(f->service.config.same(before)&&!f->backend.writes&&!f->usb.saves&&!f->ble.saves);
  }
  group("every unsupported Device field and oversized interval rejects before state/storage changes");
 }
 {
  auto f=std::make_unique<Fixture>();const auto before=f->service.config;
  // Append raw numeric wire values, never first narrow through a C++ enum or
  // the encoder. In particular 257->1 and 256->0 must not become valid settings
  // under -fshort-enums. The UENUM decoder rejects overflow before dispatch.
  auto varint=[](std::vector<uint8_t> &v,uint64_t x){while(x>127){v.push_back(uint8_t(x)|128);x>>=7;}v.push_back(uint8_t(x));};
  for(uint64_t value:{uint64_t(99),uint64_t(255),uint64_t(256),uint64_t(257),uint64_t(65535),uint64_t(65536),uint64_t(65537),uint64_t(INT32_MAX),uint64_t(UINT32_MAX),uint64_t(0x100000000),uint64_t(0x100000001),UINT64_MAX})for(unsigned field=0;field<3;++field) {
   auto a=device();uint8_t bytes[256];auto encoded=pb_ostream_from_buffer(bytes,sizeof(bytes));
   assert(pb_encode(&encoded,meshtastic_Config_DeviceConfig_fields,&a.set_config.payload_variant.device));
   std::vector<uint8_t> d(bytes,bytes+encoded.bytes_written);varint(d,uint64_t(field==0?1:field==1?6:13)*8);varint(d,value);
   std::vector<uint8_t> config;varint(config,10);varint(config,d.size());config.insert(config.end(),d.begin(),d.end());
   std::vector<uint8_t> admin;varint(admin,34*8+2);varint(admin,config.size());admin.insert(admin.end(),config.begin(),config.end());
   meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;p.to=Node;p.id=999;p.want_ack=true;
   p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
   p.decoded.payload.size=admin.size();memcpy(p.decoded.payload.bytes,admin.data(),admin.size());f->input(r,field&1);
   assert((field&1?f->ble:f->usb).error()==meshtastic_Routing_Error_BAD_REQUEST);
   assert(f->service.config.same(before)&&!f->backend.writes&&!f->usb.saves&&!f->ble.saves);
  }
  group("raw unknown, negative and over-width Device enums reject without invalid enum loads or supported-value truncation");
 }
 for(bool active:{false,true}) {
  auto f=std::make_unique<Fixture>();f->provision();assert(f->service.start(false,0)&&f->service.armEndpoint(true));text(*f,991,1);
  if(active){f->service.poll(300,700);assert(f->service.active());}
  const auto free=f->service.endpointFree(),writes=f->backend.writes,stops=f->radio.stops,saves=f->usb.saves;
  f->admin(device());assert(f->usb.error()==0&&f->service.endpointArmed()&&f->service.receiving());
  assert(f->service.endpointFree()==free&&f->service.active()==active&&f->backend.writes==writes&&f->radio.stops==stops&&f->usb.saves==saves);
  group("exact no-op preserves queued or active frame, volatile TX arm and storage");
 }
 {
  auto f=std::make_unique<Fixture>();f->begin();f->admin(device(7200),true,100);assert(f->ble.error()==0&&f->edit.dirty()&&!f->backend.writes);
  assert(getter(*f)==7200);f->admin(device(7200),false,20000);assert(f->usb.error()==0);
  assert(!f->edit.poll(60100,true)&&f->edit.open()&&!f->backend.writes);
  assert(!f->edit.poll(80000,true)&&!f->edit.open()&&f->backend.writes==1);
  mt::SettingsJournal reload;assert(reload.load(f->backend)==0&&reload.config().node().interval()==7200);
  group("shared batch immediate getter and accepted no-op refresh activity without extra write");
 }
 for(bool uncertain:{false,true}) {
  auto f=std::make_unique<Fixture>();f->admin(device(7200));assert(f->usb.error()==0);f->begin();f->admin(device(3600));
  f->backend.fail=true;f->backend.uncertain=uncertain;f->commit();
  assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&f->edit.open()&&f->edit.dirty()&&!f->journal.ready());
  assert(f->service.config.node().interval()==3600&&!f->service.endpointArmed());
  auto writes=f->backend.writes;f->commit();assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&f->backend.writes==writes);
  mt::SettingsJournal reload;assert(reload.load(f->backend)==0&&reload.config().node().interval()==(uncertain?3600u:7200u));
  group("uncertain Device commit retains pending state and freezes blind writes");
 }
 {
  mt::Config prior;auto record=prior.node();record.write32(bench::MtNodeRecord::Interval,60);record.setName("Preserved",false);record.seal();prior.node(record);
  prior.setDisplay(42,true);mt::Config decoded;assert(prior.valid()&&mt::Config::decode(prior.bytes,prior.Size,decoded)&&decoded.same(prior));
  uint8_t legacy[80];memcpy(legacy,prior.bytes,78);legacy[2]=1;auto crc=mt::Config::checksum(legacy,78);legacy[78]=crc;legacy[79]=crc>>8;
  assert(mt::Config::decode(legacy,80,decoded)&&decoded.node().interval()==60&&decoded.displaySeconds()==15);
  assert(decoded.setNodeInfoSeconds(60)&&decoded.node().interval()==3600&&!strcmp(reinterpret_cast<const char *>(decoded.node().bytes+bench::MtNodeRecord::Name),"Preserved"));
  group("v1/v2 old short interval and owner remain readable; controlled save clamps without schema migration");
 }
 for(uint32_t start:{100u,0xffff0000u}) {
  mt::Discovery d;d.observeArm(true,start);d.reschedule(3600);assert(!d.periodic(start+29999)&&d.periodic(start+30000));
  d.admitted(start+30000,10800);d.replied(0x1234,start+30000);d.reschedule(7200);
  assert(!d.periodic(start+30000+7200000-1)&&d.periodic(start+30000+7200000));
  d.reschedule(3600);assert(d.periodic(start+30000+3600000));d.reschedule(86400);
  assert(!d.periodic(start+30000+3600000)&&d.periodic(start+30000+86400000));
  d.reschedule(1);assert(!d.periodic(start+89999)&&d.periodic(start+90000));
  assert(!d.replyAllowed(0x1234,start+90000)&&d.queued==1&&d.replies==1);
  d.cancel();assert(!d.periodic(start+90000));d.observeArm(true,start+90000);
  assert(!d.periodic(start+119999)&&d.periodic(start+120000));
  group("shrink/grow uses last admission across wrap and retains startup, minute and peer limits");
 }
 {
  auto f=std::make_unique<Fixture>();f->provision();assert(f->service.start(false,0)&&f->service.armEndpoint(true));
  f->service.nodeDiscovery.observeArm(true,0);f->service.nodeDiscovery.admitted(100,10800);f->service.nodeDiscovery.replied(0x1234,100);
  f->admin(device(3600),false,200);assert(f->usb.error()==0&&f->service.receiving()&&!f->service.endpointArmed());
  assert(!f->service.nodeDiscovery.periodic(3600100)&&!f->service.nodeDiscovery.replyAllowed(0x1234,3600100));
  assert(f->service.armEndpoint(true));f->service.nodeDiscovery.observeArm(true,201);
  assert(!f->service.nodeDiscovery.periodic(3600099)&&f->service.nodeDiscovery.periodic(3600100));
  f->service.nodeDiscovery.defer(3600100);f->admin(device(3600));assert(!f->service.nodeDiscovery.periodic(3600100));
  group("real save disarms, restores prior RX, reschedules once; same config retains a deferred deadline");
 }
 {
  auto f=std::make_unique<Fixture>();f->provision();assert(f->service.start(false,0)&&f->service.armEndpoint(true));f->radio.air=2200;
  for(unsigned i=0;i<2;++i){text(*f,991+i,100+i*4000);f->service.poll(300+i*4000,700+i);assert(f->radio.sends==i+1);f->radio.done=true;f->service.poll(2600+i*4000,800+i);}
  f->admin(device(3600),false,8000);assert(f->usb.error()==0&&f->service.armEndpoint(true));
  f->service.poll(8100,901);auto utilization=f->service.channel_.percent();
  assert(f->service.channel_.diagnosticSupported()&&utilization&&*utilization<40.0f);
  f->service.poll(38100,902);f->service.poll(38400,903);
  assert(f->radio.sends==3&&f->service.throttled==0&&!f->service.faulted());
  group("normal-mode interval edit preserves channel and region admission policy");
 }
 {
  Radio radio;radio.air=2200;mt::Service service(radio,true,Node,127,false,true);
  mt::Config config;config.bytes[4]=1;auto record=config.node();record.setPublic();config.node(record);
  assert(config.valid()&&service.apply(config)&&service.start(true,0));
  auto sendNodeInfo=[&](uint32_t at,uint32_t id,unsigned expectedSends) {
   assert(service.announce(at,id));service.poll(at+250,id);assert(radio.sends==expectedSends);
   radio.done=true;service.poll(at+251,id);
  };
  sendNodeInfo(1,55,1);sendNodeInfo(60252,56,2);
  service.stop();auto updated=service.config;assert(updated.setNodeInfoSeconds(3600)&&service.apply(updated));
  assert(service.start(true,60600));service.poll(60600,57);service.poll(90600,57);
  service.poll(120503,57);service.poll(120753,57);
  assert(radio.sends==2&&service.throttled==1&&!service.faulted());
  group("native NodeInfo budget survives interval reconfiguration");
 }
}
