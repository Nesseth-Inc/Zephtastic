// SPDX-License-Identifier: GPL-3.0-only
// Synthetic Admin transport/storage fixture. No credential or RF crypto proof.
#include "device_config_fixture.h"
#include "PositionQuery.h"

namespace {
struct Provider final:mt::pki::PrivateRuntime {
 bool available=true;
 uint32_t peer()const override{return 0x87654321;}
 bool ready(uint32_t local)const override{return available&&local==Node;}
 bool publicIdentity(uint8_t [32])const override{return false;}
 bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,uint32_t,uint32_t,bool,uint32_t,bench::Frame &)override{return false;}
 void freeze()override{available=false;}
 bool matchesPeer(uint32_t)const override{return false;}
 bool revoke(uint32_t)override{return false;}
 void clearEvent()override{}
 void sweep(mt::Endpoint &)override{}
 int submit(mt::Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,bool,const uint8_t *,size_t,bool,uint32_t)override{return -ENOTSUP;}
 void receive(mt::Endpoint &,bench::MtCodec &,const uint8_t *,size_t,uint32_t,uint32_t,bool)override{}
 void proof(mt::Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t)override{}
 bool event(uint8_t,const uint8_t *,size_t,mt::pki::PrivateEvent &)override{return false;}
};
unsigned groups=0;
void group(const char *name){++groups;std::cout<<"PASS "<<name<<'\n';}
Host &host(Fixture &f,bool ble){return ble?f.ble:f.usb;}
meshtastic_AdminMessage channel(unsigned index,unsigned role,const char *name,unsigned precision,
                                uint8_t keySeed=0,bool inherited=false,bool muted=false){
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;
 auto &c=a.set_channel;c.index=int32_t(index);c.role=static_cast<meshtastic_Channel_Role>(role);c.has_settings=true;
 strcpy(c.settings.name,name);c.settings.has_module_settings=true;
 c.settings.module_settings.position_precision=precision;c.settings.module_settings.is_muted=muted;
 if(!inherited){c.settings.psk.size=keySeed?16:1;c.settings.psk.bytes[0]=keySeed?keySeed:1;}
 return a;
}
meshtastic_AdminMessage fixed(int32_t latitude=123456789,int32_t longitude=-123456789){
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_fixed_position_tag;
 a.set_fixed_position.has_latitude_i=a.set_fixed_position.has_longitude_i=true;
 a.set_fixed_position.latitude_i=latitude;a.set_fixed_position.longitude_i=longitude;
 a.set_fixed_position.has_altitude=true;a.set_fixed_position.altitude=-35;return a;
}
void accept(Fixture &f,const meshtastic_AdminMessage &a,bool ble){f.admin(a,ble);assert(host(f,ble).error()==meshtastic_Routing_Error_NONE);}
meshtastic_Channel get(Fixture &f,unsigned index,bool ble){
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_channel_request_tag;a.get_channel_request=index+1;
 f.admin(a,ble);const auto reply=host(f,ble).reply();assert(reply.which_payload_variant==meshtastic_AdminMessage_get_channel_response_tag&&reply.get_channel_response.index==int32_t(index));return reply.get_channel_response;
}
void reload(Fixture &f){mt::SettingsJournal journal;assert(journal.load(f.backend)==0&&journal.config().same(f.service.config));}
bool sameKey(const mt::PrimaryChannel::Selection &a,const mt::PrimaryChannel::Selection &b){return a.length==b.length&&a.hash==b.hash&&!memcmp(a.bytes,b.bytes,a.length);}
meshtastic_Position position(const mt::Config &config,unsigned index,bool expected){
 uint8_t data[128]{};size_t size=99;const bool ready=mt::PositionQuery::fixedResponse(config,0x9801,data,sizeof(data),size,index);
 assert(ready==expected);if(!expected){assert(size==0);return {};}
 meshtastic_Data decoded{};auto in=pb_istream_from_buffer(data,size);
 assert(pb_decode(&in,meshtastic_Data_fields,&decoded)&&!in.bytes_left&&decoded.portnum==meshtastic_PortNum_POSITION_APP&&decoded.request_id==0x9801);
 meshtastic_Position p{};auto payload=pb_istream_from_buffer(decoded.payload.bytes,decoded.payload.size);
 assert(pb_decode(&payload,meshtastic_Position_fields,&p)&&!payload.bytes_left&&p.has_latitude_i&&p.has_longitude_i);return p;
}
void setup(Fixture &f,Provider &provider,bool ble){f.radio.configurable=true;assert(f.service.bindPrivateRuntime(provider));f.provision();accept(f,fixed(),ble);}
}

int main(){std::cout.setf(std::ios::unitbuf);
 for(uint8_t model:{81,127})for(bool ble:{false,true}){
  Provider provider;Fixture f(model,false);setup(f,provider,ble);
  accept(f,channel(0,1,"Original",12,0x51),ble);
  accept(f,channel(3,2,"Chosen",31,0x62,false,true),!ble);
  accept(f,channel(4,2,"Inherited",8,0,true),ble);
  accept(f,channel(5,2,"Other",0,0,true),!ble);
  mt::PrimaryChannel::Selection before[8]{},after[8]{};
  for(unsigned index:{0u,3u,4u,5u})assert(f.service.config.channelSelection(index,before[index]));
  const auto identity=f.service.route.identity();const auto writes=f.backend.writes;
  accept(f,channel(3,1,"Chosen",31,0x62,false,true),ble);
  assert(f.backend.writes==writes+1&&f.service.route.identity()==identity);
  for(unsigned index:{0u,3u,4u,5u})assert(f.service.config.channelSelection(index,after[index]));
  assert(sameKey(before[0],after[3])&&sameKey(before[3],after[0])&&sameKey(before[4],after[4])&&sameKey(before[5],after[5]));
  for(bool transport:{false,true}){
   const auto primary=get(f,0,transport),oldPrimary=get(f,3,transport),inherited=get(f,4,transport),other=get(f,5,transport);
   assert(primary.role==meshtastic_Channel_Role_PRIMARY&&!strcmp(primary.settings.name,"Chosen")&&primary.settings.module_settings.position_precision==31&&primary.settings.module_settings.is_muted);
   assert(primary.settings.psk.size==16&&primary.settings.psk.bytes[0]==0x62);
   assert(oldPrimary.role==meshtastic_Channel_Role_SECONDARY&&!strcmp(oldPrimary.settings.name,"Original")&&oldPrimary.settings.module_settings.position_precision==12&&!oldPrimary.settings.module_settings.is_muted);
   assert(oldPrimary.settings.psk.size==16&&oldPrimary.settings.psk.bytes[0]==0x51);
   assert(inherited.settings.psk.size==16&&inherited.settings.psk.bytes[0]==0x51&&inherited.settings.module_settings.position_precision==8);
   assert(other.settings.psk.size==16&&other.settings.psk.bytes[0]==0x51&&!other.settings.module_settings.position_precision);
  }
  assert(position(f.service.config,0,true).precision_bits==31&&position(f.service.config,3,true).precision_bits==12&&position(f.service.config,4,true).precision_bits==8);
  position(f.service.config,5,false);reload(f);assert(!f.radio.sends);
  group("both models and transports: PRIMARY promotion swaps metadata and preserves untouched inherited effective channels durably");
 }
 for(uint8_t model:{81,127})for(bool ble:{false,true}){
  Provider provider;Fixture f(model,false);setup(f,provider,ble);
  accept(f,channel(0,1,"LongFast",32),ble);accept(f,channel(2,2,"Private",27,0x62),!ble);
  assert(get(f,0,!ble).settings.module_settings.position_precision==32&&get(f,2,ble).settings.module_settings.position_precision==27);
  const auto pub=position(f.service.config,0,true);assert(pub.precision_bits==15&&pub.latitude_i==123404288&&pub.longitude_i==-123404288);
  assert(position(f.service.config,2,true).precision_bits==27);
  accept(f,channel(0,1,"LongFast",0),ble);assert(!get(f,0,!ble).settings.module_settings.position_precision);
  position(f.service.config,0,false);assert(position(f.service.config,2,true).precision_bits==27);
  accept(f,channel(2,2,"Private",0,0x62),!ble);position(f.service.config,2,false);assert(!f.service.config.positionSharing());reload(f);
  accept(f,channel(0,1,"LongFast",9),ble);accept(f,channel(2,2,"Private",23,0x62),!ble);
  accept(f,fixed(234567890,-234567890),ble);
  for(bool transport:{false,true})assert(!get(f,0,transport).settings.module_settings.position_precision&&!get(f,2,transport).settings.module_settings.position_precision);
  assert(!f.service.config.positionSharing()&&f.service.config.latitudeI()==234567890&&f.service.config.longitudeI()==-234567890);
  position(f.service.config,0,false);position(f.service.config,2,false);reload(f);assert(!f.radio.sends);
  group("per-channel precision getters, public cap, zero consent and new-coordinate revocation cross USB/BLE and reload");
 }
 for(uint8_t model:{81,127})for(bool ble:{false,true}){
  Provider provider;Fixture f(model,false);setup(f,provider,ble);
  accept(f,channel(0,1,"Original",12,0x51),ble);accept(f,channel(3,2,"Chosen",22,0x62),!ble);
  const auto before=f.service.config;const auto durable=f.journal.config();f.backend.fail=true;
  f.admin(channel(3,1,"Chosen",22,0x62),ble);
  if(host(f,ble).error()!=meshtastic_Routing_Error_NO_INTERFACE||!f.service.config.same(before)||!f.journal.config().same(durable))
   std::cerr<<"FAIL save diagnostic model="<<unsigned(model)<<" ble="<<ble<<" ack="<<host(f,ble).error()<<" active_same="<<f.service.config.same(before)<<" durable_same="<<f.journal.config().same(durable)<<" journal_state="<<unsigned(f.journal.state())<<'\n';
  assert(host(f,ble).error()==meshtastic_Routing_Error_NO_INTERFACE&&f.journal.state()==mt::SettingsJournal::State::IoError&&f.service.config.same(before)&&f.journal.config().same(durable));
  meshtastic_AdminMessage query{};query.which_payload_variant=meshtastic_AdminMessage_get_channel_request_tag;query.get_channel_request=1;
  f.admin(query,!ble);assert(host(f,!ble).error()==meshtastic_Routing_Error_NO_INTERFACE&&f.service.config.same(before));
  mt::SettingsJournal journalReload;assert(journalReload.load(f.backend)==0&&journalReload.config().same(durable));assert(!f.radio.sends);
  Provider rebootProvider;Fixture reboot(model,false);reboot.radio.configurable=true;reboot.backend.entries=f.backend.entries;
  assert(reboot.journal.load(reboot.backend)==0&&reboot.service.bindPrivateRuntime(rebootProvider)&&reboot.service.apply(reboot.journal.config()));
  for(bool transport:{false,true})assert(get(reboot,0,transport).settings.psk.bytes[0]==0x51&&get(reboot,3,transport).role==meshtastic_Channel_Role_SECONDARY);
  assert(reboot.service.config.same(before)&&!reboot.radio.sends);
  group("failed promotion journal save retains active and durable channel/privacy settings on both transports/models");
 }
 for(uint8_t model:{81,127})for(bool ble:{false,true}){
  Provider provider;Fixture f(model,false);setup(f,provider,ble);const auto before=f.service.config;const auto writes=f.backend.writes;
  for(unsigned mode:{0u,1u,2u,4u,5u,6u,7u,8u,9u}){auto a=channel(mode&1?3:0,mode&1?2:1,"Private",12,0x62);auto &c=a.set_channel;
   switch(mode){case 0:c.settings.module_settings.position_precision=33;break;case 1:c.settings.uplink_enabled=true;break;case 2:c.settings.downlink_enabled=true;break;case 4:c.settings.id=1;break;case 5:c.settings.channel_num=1;break;case 6:c.index=8;break;case 7:c.role=static_cast<meshtastic_Channel_Role>(3);break;case 8:strcpy(c.settings.name,"bad\x01");break;case 9:c.settings.module_settings.position_precision=33;break;}
   f.admin(a,ble);assert(host(f,ble).error()==meshtastic_Routing_Error_BAD_REQUEST&&f.service.config.same(before)&&f.backend.writes==writes);
  }
  for(unsigned request:{0u,9u}){meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_channel_request_tag;a.get_channel_request=request;f.admin(a,!ble);assert(host(f,!ble).error()==meshtastic_Routing_Error_BAD_REQUEST&&f.service.config.same(before)&&f.backend.writes==writes);}
  assert(!f.radio.sends);group("unsupported channel fields, invalid precision/index/role/name and getter boundaries refuse atomically");
 }
 for(uint8_t model:{81,127})for(bool ble:{false,true}){
  Provider provider;Fixture f(model,false);f.radio.configurable=true;assert(f.service.bindPrivateRuntime(provider));f.provision();const auto before=f.service.config;const auto writes=f.backend.writes;
  for(unsigned index:{0u,3u}){f.admin(channel(index,index?2:1,"Private",12,0x62),ble);assert(host(f,ble).error()==meshtastic_Routing_Error_BAD_REQUEST&&f.service.config.same(before)&&f.backend.writes==writes);}
  group("nonzero primary and secondary privacy require fixed coordinates before any save");
 }
 for(uint8_t model:{81,127})for(bool ble:{false,true}){
  Fixture f(model,false);f.radio.configurable=true;f.provision();accept(f,fixed(),ble);const auto before=f.service.config;const auto writes=f.backend.writes;
  f.admin(channel(3,1,"Private",20,0x62),ble);assert(host(f,ble).error()==meshtastic_Routing_Error_BAD_REQUEST&&f.service.config.same(before)&&f.backend.writes==writes&&!f.radio.sends);
  group("custom promotion without a ready synthetic provider refuses before storage");
 }
 for(uint8_t model:{81,127})for(bool ble:{false,true}){
  Provider provider;Fixture f(model,false);f.radio.configurable=true;
  assert(f.service.bindPrivateRuntime(provider));f.provision();
  const auto before=f.service.config;const auto identity=f.service.route.identity();
  auto a=channel(7,2,"Retained",0,0,false,true);accept(f,a,ble);
  a.set_channel.role=meshtastic_Channel_Role_DISABLED;accept(f,a,ble);
  const auto disabled=get(f,7,!ble);mt::PrimaryChannel::Selection selection;
  assert(disabled.role==meshtastic_Channel_Role_DISABLED&&!strcmp(disabled.settings.name,"Retained")&&
    disabled.settings.psk.size==1&&disabled.settings.psk.bytes[0]==1&&disabled.settings.module_settings.is_muted&&
    !disabled.settings.module_settings.position_precision&&!f.service.config.channelSelection(7,selection));
  mt::PrimaryChannel::clear(selection);assert(!memcmp(before.bytes,f.service.config.bytes,126)&&f.service.route.identity()==identity);
  position(f.service.config,7,false);reload(f);assert(!f.radio.sends);
  const auto saved=f.service.config;const auto writes=f.backend.writes;
  a.set_channel.settings.uplink_enabled=true;f.admin(a,ble);
  assert(host(f,ble).error()==meshtastic_Routing_Error_BAD_REQUEST&&f.service.config.same(saved)&&f.backend.writes==writes);
  a.set_channel.settings.uplink_enabled=false;a.set_channel.role=meshtastic_Channel_Role_SECONDARY;accept(f,a,!ble);
  const auto enabled=get(f,7,ble);assert(enabled.role==meshtastic_Channel_Role_SECONDARY&&!strcmp(enabled.settings.name,"Retained")&&
    enabled.settings.psk.size==1&&enabled.settings.psk.bytes[0]==1&&enabled.settings.module_settings.is_muted&&!enabled.settings.module_settings.position_precision);
  reload(f);assert(!f.radio.sends&&!f.service.config.positionSharing());
  group("secondary disable preserves name/key/mute across both transports and reload; reenabling keeps zero consent and unsupported flags refuse atomically");
 }
 std::cout<<"PASS channel privacy transport suite: "<<groups<<" groups\n";
}
