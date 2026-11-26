// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
#include <climits>
#define USERPREFS_EVENT_MODE 0
#define HOP_MAX 7
struct {struct {uint8_t hop_limit;} lora;} config;
struct Default {static uint8_t getConfiguredOrDefaultHopLimit(uint8_t);};
static int8_t getHopsAway(const meshtastic_MeshPacket &p){return int8_t(p.hop_start-p.hop_limit);}
struct RoutingModule {uint8_t getHopLimitForResponse(const meshtastic_MeshPacket&);};
#include "hop_policy/default.inc"
#include "hop_policy/response.inc"
static void group(const char *s){std::cout<<"PASS "<<s<<'\n';}
int main(){
 for(unsigned stored=0;stored<=7;++stored){config.lora.hop_limit=stored;
  for(unsigned input=0;input<=255;++input)assert(mt::HopPolicy::configured(input,stored)==Default::getConfiguredOrDefaultHopLimit(input));
  for(unsigned start=0;start<=7;++start)for(unsigned remaining=0;remaining<=start;++remaining){
   meshtastic_MeshPacket p{};p.hop_start=start;p.hop_limit=remaining;RoutingModule stock;
   assert(mt::HopPolicy::response(start,remaining,stored)==stock.getHopLimitForResponse(p));
  }
 }
 group("all256 configured inputs and all modern header/default combinations match literal pinned stock functions");
 for(unsigned hop=0;hop<=7;++hop){mt::Config c;assert(c.setHopLimit(hop)&&c.valid());mt::Config out;assert(mt::Config::decode(c.bytes,c.Size,out)&&out.same(c));}
 for(unsigned bad:{8u,255u,UINT_MAX}){mt::Config c;const auto before=c;assert(!c.setHopLimit(bad)&&c.same(before));c.bytes[121]=uint8_t(bad);c.seal();mt::Config out;const auto old=out;assert(!mt::Config::decode(c.bytes,c.Size,out)&&out.same(old));}
 group("v12 hop0through7 persists exactly; invalid setter and raw record refuse atomically");
 {mt::Config c;assert(c.setDeviceRole(2,5)&&c.setPrimaryName("OldNamed")&&c.setLoRa(4,1));const auto old=c;
  uint8_t prior[128]{};memcpy(prior,c.bytes,126);prior[2]=11;prior[121]=0;
  auto seal=[&](){const auto crc=mt::Config::checksum(prior,126);prior[126]=uint8_t(crc);prior[127]=uint8_t(crc>>8);};seal();mt::Config out;
  assert(mt::Config::decode(prior,sizeof(prior),out)&&out.valid()&&out.bytes[2]==mt::Config::Version&&out.hopLimit()==3);
  assert(!memcmp(out.bytes,c.bytes,2)&&!memcmp(out.bytes+3,c.bytes+3,118));
  prior[121]=1;seal();const auto before=out;assert(!mt::Config::decode(prior,sizeof(prior),out)&&out.same(before));
  assert(out.role()==2&&out.rebroadcastMode()==5&&!strcmp(out.primaryName(),"OldNamed")&&out.modemPreset()==4&&out.channelNumber()==1&&old.node().valid());
 }
 group("original v11 CRC and reserved bytes validate before migration; names role mode key modem and channel retained");
 for(bool ble:{false,true})for(unsigned hop=0;hop<=7;++hop){Fixture f;f.radio.configurable=true;f.provision();
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;a.set_config.which_payload_variant=meshtastic_Config_lora_tag;
  auto &l=a.set_config.payload_variant.lora;l.region=meshtastic_Config_LoRaConfig_RegionCode_US;l.use_preset=true;l.channel_num=20;l.hop_limit=hop;l.sx126x_rx_boosted_gain=true;
  f.admin(a,ble);assert((ble?f.ble:f.usb).error()==0&&f.service.config.hopLimit()==hop);
  mt::SettingsJournal reboot;assert(!reboot.load(f.backend)&&reboot.config().same(f.service.config));
  meshtastic_AdminMessage q{};q.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;q.get_config_request=meshtastic_AdminMessage_ConfigType_LORA_CONFIG;f.admin(q,!ble);
  assert((ble?f.usb:f.ble).reply().get_config_response.payload_variant.lora.hop_limit==hop);
  const auto writes=f.backend.writes,starts=f.radio.starts;f.admin(a,ble);assert((ble?f.ble:f.usb).error()==0&&f.backend.writes==writes&&f.radio.starts==starts&&!f.radio.sends);
 }
 group("all hop defaults use actual USB BLE configuration opposite readback journal reboot and unchanged-save with zero RF");
 for(unsigned hop=0;hop<=7;++hop){Radio radio;mt::Service s(radio,true,Node,81,true,false);auto c=s.config;c.bytes[4]=1;c.seal();assert(s.apply(c)&&s.start(false,0)&&s.armEndpoint(true));
  const uint8_t data[]={8,0x80,0x02,18,1,'x',72,0};assert(s.submitPublicApplication(0,0xffffffffu,500+hop,data,sizeof(data),hop,false,100)==0);
  assert(s.endpointFree()==3);
 }
 group("actual Service and Endpoint admit public origins at every encoded three-bit hop limit");
 return 0;
}
