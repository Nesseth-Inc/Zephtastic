// SPDX-License-Identifier: GPL-3.0-only
// Independent assertions against the selected source's RAM-only fixture.
#include "device_config_fixture.h"
int main(){
 for(uint8_t model:{81,127})for(bool viaBle:{false,true}){
  Fixture f(model);auto original=f.service.config;original.setDisplay(77,true);auto record=original.node();record.setName("Keep owner",false);original.node(record);assert(!f.edit.save(original,0));
  uint32_t rng=0x1a2b3c4d;
  for(unsigned i=0;i<100;++i){rng=rng*1664525u+1013904223u;const uint32_t interval=rng%90001u;const auto prior=f.service.config;const auto writes=f.backend.writes;f.admin(device(interval),viaBle,10+i);const auto &host=viaBle?f.ble:f.usb;
   if(interval>2147483){assert(host.error()==meshtastic_Routing_Error_BAD_REQUEST&&f.service.config.same(prior)&&f.backend.writes==writes);continue;}
   assert(host.error()==0);auto after=f.service.config;const uint32_t effective=interval<3600?3600:interval;assert(getter(f,!viaBle)==effective&&after.valid());
   // Restore only the interval and nested/outer checksum fields, then demand all
   // other stored bytes match; protects names/channel/region/display/identity.
   auto node=after.node();node.write32(bench::MtNodeRecord::Interval,prior.node().interval());node.seal();after.node(node);assert(after.same(prior));
   mt::SettingsJournal reboot;assert(!reboot.load(f.backend)&&reboot.config().same(f.service.config));assert(!f.radio.sends&&!f.radio.starts&&!f.service.endpointArmed());
  }
 }
 std::cout<<"PASS 400-randomized-device-saves-preserve-unrelated-record-bytes-and-journal-restart\n";
 for(bool viaBle:{false,true})for(unsigned bad=0;bad<4;++bad){Fixture f;f.provision();assert(f.service.start(false,0)&&f.service.armEndpoint(true));text(f,0x1234,1);const auto prior=f.service.config;const auto writes=f.backend.writes,stops=f.radio.stops,free=f.service.endpointFree();auto request=device(7200);auto &d=request.set_config.payload_variant.device;if(bad==0)d.role=static_cast<meshtastic_Config_DeviceConfig_Role>(99);if(bad==1)d.tzdef[0]='x';if(bad==2)d.buzzer_gpio=UINT32_MAX;if(bad==3)d.node_info_broadcast_secs=UINT32_MAX;f.admin(request,viaBle,5);assert((viaBle?f.ble:f.usb).error()==meshtastic_Routing_Error_BAD_REQUEST);assert(f.service.config.same(prior)&&f.backend.writes==writes&&f.radio.stops==stops&&f.service.endpointFree()==free&&f.service.endpointArmed());}
 std::cout<<"PASS mixed-valid-interval-unsupported-fields-cannot-partially-apply-or-cancel-traffic\n";
 for(uint32_t base:{0u,1u,0xfffffff0u,0x80000000u})for(uint32_t secs:{3600u,3601u,86400u,86401u,604800u,2147483u}){mt::Discovery d;d.observeArm(true,base);assert(d.periodic(base+30000));d.admitted(base+30000,10800);d.replied(9,base+30000);d.reschedule(secs);const uint32_t due=base+30000+secs*1000;assert(!d.periodic(due-1)&&d.periodic(due));assert(!d.replyAllowed(9,base+60000));const auto queued=d.queued,replies=d.replies;d.cancel();d.observeArm(true,due+100);assert(!d.periodic(due+30099)&&d.periodic(due+30100));assert(d.queued==queued&&d.replies==replies);}
 std::cout<<"PASS independent-discovery-deadline-wrap-and-overdue-rearm-guard\n";
 // Construct malformed numeric enums directly on the wire, so a short C++ enum
 // cannot truncate the test input before the actual nanopb decoder sees it.
 for(bool viaBle:{false,true})for(uint32_t field:{meshtastic_Config_DeviceConfig_role_tag,meshtastic_Config_DeviceConfig_rebroadcast_mode_tag,meshtastic_Config_DeviceConfig_buzzer_mode_tag})for(uint64_t value:{uint64_t(99),uint64_t(255),uint64_t(256),uint64_t(257),uint64_t(65535),uint64_t(65536),uint64_t(65537),uint64_t(UINT32_MAX),uint64_t(UINT32_MAX)+1,uint64_t(UINT32_MAX)+2,UINT64_MAX}) {
  Fixture f;f.provision();assert(f.service.start(false,0)&&f.service.armEndpoint(true));text(f,0x55aa,1);
  const auto config=f.service.config;const auto writes=f.backend.writes,stops=f.radio.stops,free=f.service.endpointFree();
  uint8_t rawDevice[256],rawConfig[300],rawAdmin[350];auto valid=device(7200);
  auto d=pb_ostream_from_buffer(rawDevice,sizeof(rawDevice));assert(pb_encode(&d,meshtastic_Config_DeviceConfig_fields,&valid.set_config.payload_variant.device));
  assert(pb_encode_tag(&d,PB_WT_VARINT,field)&&pb_encode_varint(&d,value));
  auto c=pb_ostream_from_buffer(rawConfig,sizeof(rawConfig));assert(pb_encode_tag(&c,PB_WT_STRING,meshtastic_Config_device_tag)&&pb_encode_string(&c,rawDevice,d.bytes_written));
  auto a=pb_ostream_from_buffer(rawAdmin,sizeof(rawAdmin));assert(pb_encode_tag(&a,PB_WT_STRING,meshtastic_AdminMessage_set_config_tag)&&pb_encode_string(&a,rawConfig,c.bytes_written));
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;p.to=Node;p.id=0x8877;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
  assert(a.bytes_written<=sizeof(p.decoded.payload.bytes));memcpy(p.decoded.payload.bytes,rawAdmin,a.bytes_written);p.decoded.payload.size=a.bytes_written;f.input(r,viaBle,5);
  assert((viaBle?f.ble:f.usb).error()==meshtastic_Routing_Error_BAD_REQUEST&&f.service.config.same(config)&&f.backend.writes==writes&&f.radio.stops==stops&&f.service.endpointFree()==free&&f.service.endpointArmed());
 }
 std::cout<<"PASS raw-wire-overwidth-enums-cannot-alias-supported-values-or-cancel-queued-traffic\n";

}
