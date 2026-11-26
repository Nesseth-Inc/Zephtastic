// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
#include "stock_precision_reference.h"
int main(){
 unsigned tested=0;
 for(int32_t lat:{123456789,-900000000,-899999999,-123456789,-1,0,1,899999999,900000000})
 for(int32_t lon:{987654321,-1800000000,-1799999999,-987654321,-1,0,1,1799999999,1800000000}){
  mt::Config c;assert(c.setFixedPosition(lat,lon,true,-35));c.setPositionSharing(true);auto before=c;
  uint8_t bytes[240];size_t n=99;bool ok=mt::PositionQuery::fixedResponse(c,123,bytes,sizeof(bytes),n);
  int32_t x=stock_reference::truncateCoordinate(lat,uint32_t(15)),y=stock_reference::truncateCoordinate(lon,uint32_t(15));
  bool valid=x>=-900000000&&x<=900000000&&y>=-1800000000&&y<=1800000000;
  assert(ok==valid&&c.same(before));if(!valid){assert(n==0);continue;}
  meshtastic_Data d{};auto in=pb_istream_from_buffer(bytes,n);assert(pb_decode(&in,meshtastic_Data_fields,&d)&&d.request_id==123);
  meshtastic_Position p{};in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&in,meshtastic_Position_fields,&p));
  assert(p.precision_bits==15&&p.latitude_i==x&&p.longitude_i==y&&p.has_latitude_i&&p.has_longitude_i);
  assert(p.has_altitude&&p.altitude==-35&&!p.time&&!p.timestamp&&!p.fix_quality&&!p.sats_in_view);++tested;
  c.setPositionSharing(false);assert(!mt::PositionQuery::fixedResponse(c,123,bytes,sizeof(bytes),n)&&n==0);
 }
 assert(tested>30);std::cout<<"PASS stock-derived 15-bit quantization for signs/zero/boundaries; invalid geographic centers fail closed; local coordinates unchanged\n";
 for(uint8_t model:{81,127})for(bool ble:{false,true}){
  Fixture f(model,false);auto c=f.service.config;c.bytes[4]=1;c.setFixedPosition(123456789,-987654321,false,0);c.seal();assert(!f.edit.save(c,0));auto &h=ble?f.ble:f.usb;
  auto setting=[](unsigned precision){meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;auto &ch=a.set_channel;ch.index=0;ch.role=meshtastic_Channel_Role_PRIMARY;ch.has_settings=true;ch.settings.psk.size=1;ch.settings.psk.bytes[0]=1;ch.settings.has_module_settings=true;ch.settings.module_settings.position_precision=precision;return a;};
  for(unsigned precision:{15,32}){
   f.admin(setting(precision),ble);assert(h.error()==0&&f.service.config.positionSharing());
   meshtastic_AdminMessage get{};get.which_payload_variant=meshtastic_AdminMessage_get_channel_request_tag;get.get_channel_request=1;f.admin(get,ble);
   assert(h.reply().get_channel_response.settings.module_settings.position_precision==15);
   auto writes=f.backend.writes;f.admin(setting(15),ble);assert(h.error()==0&&f.backend.writes==writes);
  }
  auto before=f.service.config;for(unsigned bad:{1u,14u,16u,31u,33u,UINT32_MAX}){f.admin(setting(bad),ble);assert(h.error()==meshtastic_Routing_Error_BAD_REQUEST&&f.service.config.same(before));}
  f.admin(setting(0),ble);assert(h.error()==0&&!f.service.config.positionSharing());assert(!f.radio.sends&&!f.radio.starts);
 }
 std::cout<<"PASS both models/transports: effective precision readback and inert round-trip, 32 clamped, unsupported values reject, consent revokes\n";
}

