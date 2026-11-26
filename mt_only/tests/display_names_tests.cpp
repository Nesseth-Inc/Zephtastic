// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
#include "ui/Render.h"
static meshtastic_AdminMessage setting(bool enabled){
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
 a.set_config.which_payload_variant=meshtastic_Config_display_tag;
 a.set_config.payload_variant.display.screen_on_secs=15;
 a.set_config.payload_variant.display.use_long_node_name=enabled;return a;
}
static bool readback(Fixture &f,bool ble){
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;
 a.get_config_request=meshtastic_AdminMessage_ConfigType_DISPLAY_CONFIG;
 auto &h=ble?f.ble:f.usb;h.out.clear();f.admin(a,ble);
 return h.reply().get_config_response.payload_variant.display.use_long_node_name;
}
int main(int argc,char **argv){
 if(argc==4){ // stock-protobuf bridge: model, transport, complete ToRadio hex
  Fixture f(uint8_t(std::stoi(argv[1])));std::vector<uint8_t> wire;std::string hex=argv[3];
  assert(hex.size()%2==0);for(size_t i=0;i<hex.size();i+=2)wire.push_back(uint8_t(std::stoul(hex.substr(i,2),nullptr,16)));
  const bool ble=std::string(argv[2])=="ble";f.raw(wire.data(),wire.size(),ble,1);
  auto &h=ble?f.ble:f.usb;for(auto &r:h.out){uint8_t b[1024];auto o=pb_ostream_from_buffer(b,sizeof b);assert(pb_encode(&o,meshtastic_FromRadio_fields,&r));for(size_t i=0;i<o.bytes_written;++i)printf("%02x",b[i]);puts("");}
  printf("STATE %u %u\n",unsigned(f.service.config.displayLongNames()),f.backend.writes);return 0;
 }
 for(uint8_t model:{81,127})for(bool ble:{false,true}){
  Fixture f(model);auto &h=ble?f.ble:f.usb;assert(!readback(f,ble));
  const auto initial=f.service.config;h.out.clear();f.admin(setting(true),ble);
  bool hasDisplay=model==127;
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED) && defined(CONFIG_MT_DISPLAY)
  hasDisplay|=model==81;
#endif
  assert(h.error()==(hasDisplay?meshtastic_Routing_Error_NONE:meshtastic_Routing_Error_BAD_REQUEST));
  if(!hasDisplay){assert(f.service.config.same(initial)&&f.backend.writes==0);continue;}
  assert(readback(f,!ble)&&f.service.config.displayLongNames()&&f.backend.writes==1);
  mt::SettingsJournal reload;assert(!reload.load(f.backend)&&reload.config().displayLongNames());
  if(model==81){
    auto prefs=setting(true);auto &display=prefs.set_config.payload_variant.display;
    display.screen_on_secs=60;display.flip_screen=true;display.auto_screen_carousel_secs=20;
    h.out.clear();f.admin(prefs,ble);assert(!h.error());
    mt::SettingsJournal persisted;assert(!persisted.load(f.backend));
    assert(persisted.config().displayFlip() && persisted.config().displaySeconds()==60 &&
           persisted.config().displayCarouselSeconds()==20 && persisted.config().displayLongNames());
    auto &other=ble?f.usb:f.ble;meshtastic_AdminMessage query{};query.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;
    query.get_config_request=meshtastic_AdminMessage_ConfigType_DISPLAY_CONFIG;other.out.clear();f.admin(query,!ble);
    const auto actual=other.reply().get_config_response.payload_variant.display;
    assert(actual.flip_screen && actual.screen_on_secs==60 && actual.auto_screen_carousel_secs==20 && actual.use_long_node_name);
    display.units=meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL;
    const auto before=f.backend.writes;h.out.clear();f.admin(prefs,ble);
    assert(h.error()==meshtastic_Routing_Error_BAD_REQUEST && f.backend.writes==before);
    h.out.clear();f.admin(setting(true),ble);assert(!h.error());
  }
  const auto writes=f.backend.writes;h.out.clear();f.admin(setting(true),ble);assert(h.error()==0&&f.backend.writes==writes);
  h.out.clear();f.admin(setting(false),ble);assert(h.error()==0&&!readback(f,!ble)&&f.backend.writes==writes+1);
  assert(!reload.load(f.backend)&&!reload.config().displayLongNames()&&!f.radio.sends);
 }
 puts("PASS both boards/transports; real save/readback/reload/toggle/no-op and headless refusal");
 for(bool uncertain:{false,true}){
  Fixture f;f.begin();f.usb.out.clear();f.admin(setting(true));assert(f.usb.error()==0&&f.backend.writes==0&&readback(f,true));
  f.backend.fail=true;f.backend.uncertain=uncertain;f.usb.out.clear();f.commit();assert(f.usb.error()!=0&&f.journal.state()==mt::SettingsJournal::State::IoError);
  auto writes=f.backend.writes;f.usb.out.clear();f.admin(setting(false));assert(f.usb.error()!=0&&f.backend.writes==writes);
 }
 puts("PASS shared batch visibility; failed/uncertain storage never reports committed success or retries");
 {
  mt::Config base;base.bytes[4]=1;base.setTxEnabled(false);base.setDisplay(42,true,3);base.setNodeInfoSeconds(172800);base.setFixedPosition(123,-456,true,-12);base.setPositionSharing(true);base.setChannelMuted(true);
  std::vector<uint8_t> old(base.bytes,base.bytes+mt::Config::FormerSize);old[2]=8;memset(old.data()+104,0,6);
  const auto sealOld=[](std::vector<uint8_t> &b){const auto sum=mt::Config::checksum(b.data(),b.size()-2);b[b.size()-2]=uint8_t(sum);b.back()=uint8_t(sum>>8);};sealOld(old);
  mt::Config image;memcpy(image.bytes,old.data(),old.size());assert(image.validBody(8));
  mt::Config migrated;assert(mt::Config::decode(old.data(),old.size(),migrated)&&migrated.valid()&&migrated.bytes[2]==mt::Config::Version&&!migrated.displayLongNames());
  assert(!memcmp(old.data()+3,migrated.bytes+3,101)&&migrated.role()==1&&migrated.rebroadcastMode()==0&&!migrated.primaryName()[0]&&migrated.modemPreset()==0&&migrated.channelNumber()==20);
  auto oldBad=old;oldBad[103]=1;sealOld(oldBad);const auto before=migrated;assert(!mt::Config::decode(oldBad.data(),oldBad.size(),migrated)&&migrated.same(before));
  for(unsigned value:{2u,255u}){auto bad=migrated;bad.bytes[103]=value;bad.seal();assert(!bad.valid());}
  for(unsigned precision:{1u,32u}){auto supported=migrated;assert(supported.setPrimaryPositionPrecision(precision)&&supported.valid());mt::Config out;
    assert(mt::Config::decode(supported.bytes,supported.Size,out)&&out.same(supported)&&out.primaryPositionPrecision()==precision&&out.positionPrecision(0)==(precision>15?15:precision));}
  {auto bad=migrated;bad.bytes[122]=33;bad.seal();assert(!bad.valid());}
  migrated.setDisplayLongNames(true);assert(migrated.valid());mt::Config reloaded;assert(mt::Config::decode(migrated.bytes,migrated.Size,reloaded)&&reloaded.same(migrated));
 }
 puts("PASS original112 populated v8 migration preserves other fields; current precision roundtrip and reserved/boolean validation");
 {
  mt::ui::Model m;m.page=mt::ui::Page::Nodes;m.node(42,"Long Example","LE",127,-70,4,0);
  assert(!strcmp(m.nodeLabel(m.nodes[0],"fallback"),"LE"));mt::ui::Canvas a,b;a.draw(m);m.useLongNodeName=true;b.draw(m);assert(memcmp(a.bytes,b.bytes,sizeof a.bytes));
  assert(!strcmp(m.nodeLabel(m.nodes[0],"fallback"),"Long Example"));m.nodes[0].name[0]=0;assert(!strcmp(m.nodeLabel(m.nodes[0],"fallback"),"LE"));
  m.nodes[0].shortName[0]=0;assert(!strcmp(m.nodeLabel(m.nodes[0],"fallback"),"fallback"));
 }
 puts("PASS actual canvas changes and bounded missing-name fallbacks");
}
