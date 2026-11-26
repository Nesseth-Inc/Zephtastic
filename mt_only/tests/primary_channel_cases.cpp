// SPDX-License-Identifier: GPL-3.0-only
// Canonical USB/BLE and memory-journal tests. The provider below is a readiness
// fixture only: no crypto, nonce durability or hardware result is inferred.
#include "device_config_fixture.h"
#include "ui/Model.h"
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
void group(const char *s){++groups;std::cout<<"PASS "<<s<<'\n';}
meshtastic_AdminMessage channel(size_t width,bool aead=false) {
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;
 auto &c=a.set_channel;c.index=0;c.role=meshtastic_Channel_Role_PRIMARY;c.has_settings=true;
 c.settings.psk.size=width;c.settings.psk.bytes[0]=width==1?1:0x51;c.settings.use_aead=aead;
 strcpy(c.settings.name,"LongFast");return a;
}
meshtastic_Channel get(Fixture &f,bool ble) {
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_channel_request_tag;a.get_channel_request=1;
 f.admin(a,ble);return (ble?f.ble:f.usb).reply().get_channel_response;
}
uint8_t stockHash(const meshtastic_Channel &c) {
 uint8_t value=0;for(const char n:"LongFast")value^=uint8_t(n);
 for(size_t i=0;i<c.settings.psk.size;++i)value^=c.settings.psk.bytes[i];
 return value;
}
}
int main(){
 for(uint8_t model:{81,127})for(bool ble:{false,true})for(size_t width:{size_t(16),size_t(32)}) {
  Provider provider;Fixture f(model,false);assert(f.service.bindPrivateRuntime(provider));f.provision();
  auto before=f.service.config;const auto identity=f.service.route.identity();auto a=channel(width);const auto writes=f.backend.writes;
  f.admin(a,ble);assert((ble?f.ble:f.usb).error()==0&&f.backend.writes==writes+1);
  auto record=f.service.config.node();assert(f.service.config.provisioned()&&!record.isPublic());
  assert(record.bytes[3]==width&&record.bytes[4]==stockHash(a.set_channel));
  assert(!memcmp(record.bytes+record.Key,a.set_channel.settings.psk.bytes,width));
  before.node(record);assert(before.setPrimaryName("LongFast"));assert(before.same(f.service.config)&&identity==f.service.route.identity());
  for(bool transport:{false,true}){const auto c=get(f,transport);assert(c.role==meshtastic_Channel_Role_PRIMARY&&c.settings.psk.size==width&&
    !memcmp(c.settings.psk.bytes,a.set_channel.settings.psk.bytes,width)&&!strcmp(c.settings.name,"LongFast"));}
  mt::SettingsJournal reload;assert(!reload.load(f.backend)&&reload.config().same(f.service.config));
  // A fresh non-PKI Service may retain/display a valid stored channel but may
  // not start RX/TX until the boot composition binds a ready provider.
  Radio radio;mt::Service fresh(radio,true,Node,model,true,false);assert(fresh.apply(reload.config()));
  assert(!fresh.start(false,0)&&!fresh.armEndpoint(true)&&!radio.starts&&!radio.sends);
  Provider rebootProvider;assert(fresh.bindPrivateRuntime(rebootProvider)&&fresh.start(false,0));
  auto duplicate=f.backend.writes;f.admin(a,ble);assert((ble?f.ble:f.usb).error()==0&&f.backend.writes==duplicate);
  group("16/32-byte fixed LongFast primary roundtrips USB/BLE, journal and guarded fresh Service on both models");
 }
 {
  for(uint8_t model:{81,127})for(bool ble:{false,true}) {
   Provider provider;Fixture f(model,false);assert(f.service.bindPrivateRuntime(provider));f.provision();
   auto a=channel(16,true);const auto writes=f.backend.writes;f.admin(a,ble);
   assert((ble?f.ble:f.usb).error()==0&&f.backend.writes==writes+1);
   auto record=f.service.config.node();const uint8_t base=stockHash(a.set_channel);
   assert(record.bytes[4]==base&&record.bytes[3]==16&&f.service.config.primaryAead());
   mt::PrimaryChannel::Selection selected;assert(f.service.config.channelSelection(0,selected)&&selected.aead&&selected.hash==uint8_t(base^0xAEu));
   mt::PrimaryChannel::clear(selected);
   for(bool transport:{false,true}){const auto c=get(f,transport);assert(c.settings.use_aead&&c.settings.psk.size==16&&c.settings.psk.bytes[0]==a.set_channel.settings.psk.bytes[0]);}
   mt::SettingsJournal reload;assert(!reload.load(f.backend)&&reload.config().same(f.service.config)&&reload.config().primaryAead());
   assert(sizeof(mt::Config)==640&&mt::SettingsJournal::RecordSize==656);
   // Turning AEAD off restores the unchanged stock CTR hash for the same key.
   a.set_channel.settings.use_aead=false;f.admin(a,ble);assert((ble?f.ble:f.usb).error()==0&&!f.service.config.primaryAead());
   assert(f.service.config.channelSelection(0,selected)&&!selected.aead&&selected.hash==base);mt::PrimaryChannel::clear(selected);
  }
  group("primary AEAD mode roundtrips USB/BLE and durable journal with base-node hash, XOR discriminator, fixed sizes, and CTR restore");
 }
 {
  for(uint8_t model:{81,127})for(bool ble:{false,true})for(size_t width:{size_t(1),size_t(2),size_t(15),size_t(17),size_t(31)}) {
   Provider provider;Fixture f(model,false);assert(f.service.bindPrivateRuntime(provider));f.provision();
   auto a=channel(width);if(width==1)a.set_channel.settings.psk.bytes[0]=7;
   mt::PrimaryChannel::Selection expected;assert(mt::PrimaryChannel::normalize(a.set_channel.settings.psk.bytes,width,expected));
   f.admin(a,ble);assert((ble?f.ble:f.usb).error()==0);
   const auto record=f.service.config.node();assert(record.bytes[3]==expected.length&&record.bytes[4]==expected.hash&&
    !memcmp(record.bytes+record.Key,expected.bytes,expected.length));
   for(bool transport:{false,true}){const auto c=get(f,transport);assert(c.role==meshtastic_Channel_Role_PRIMARY&&c.settings.psk.size==expected.length&&
    !memcmp(c.settings.psk.bytes,expected.bytes,expected.length));}
   mt::SettingsJournal reload;assert(!reload.load(f.backend)&&reload.config().same(f.service.config));
   const auto writes=f.backend.writes;f.admin(a,ble);assert((ble?f.ble:f.usb).error()==0&&f.backend.writes==writes);
   mt::PrimaryChannel::clear(expected);
  }
  group("stock nondefault alias and padded AES128/AES256 keys roundtrip normalized USB BLE journal and unchanged saves on both models");
 }
 {
  mt::Config current;
  assert(current.setSecurityOptions(true,true));
  assert(current.setSecurityControls(true,false));
  assert(current.debugLogApiEnabled()&&current.adminChannelEnabled()&&current.isManaged()&&!current.serialEnabled());
  assert(!current.primaryAead()&&!current.licensedOperation());

  mt::Config v22=current;v22.bytes[2]=22;v22.seal();
  assert(v22.validBody(22)&&!v22.primaryAead());
  mt::Config migrated;assert(mt::Config::decode(v22.bytes,mt::Config::Size,migrated));
  assert(migrated.valid()&&migrated.bytes[2]==mt::Config::Version&&
    migrated.debugLogApiEnabled()&&migrated.adminChannelEnabled()&&migrated.isManaged()&&!migrated.serialEnabled());
  assert(migrated.bytes[125]==current.bytes[125]&&!migrated.primaryAead());
  assert(!memcmp(migrated.bytes,current.bytes,mt::Config::Size));

  Backend backend;mt::SettingsJournal initial;assert(initial.load(backend)==0);
  assert(initial.save(backend,migrated)==0);mt::SettingsJournal reboot;
  assert(reboot.load(backend)==0&&reboot.config().same(migrated));
  assert(reboot.config().debugLogApiEnabled()&&reboot.config().adminChannelEnabled()&&
    reboot.config().isManaged()&&!reboot.config().serialEnabled()&&!reboot.config().primaryAead());
  group("V22 decode preserves debug log Admin managed and serial controls before V23 journal reload");
 }
 {
  Fixture f;auto a=channel(16);const auto before=f.service.config;const auto writes=f.backend.writes;
  f.admin(a);assert(f.usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&f.service.config.same(before)&&f.backend.writes==writes&&!f.usb.saves);
  Provider provider;assert(f.service.bindPrivateRuntime(provider));provider.available=false;
  f.admin(a,true);assert(f.ble.error()==meshtastic_Routing_Error_BAD_REQUEST&&f.service.config.same(before)&&f.backend.writes==writes&&!f.ble.saves);
  group("missing and unready durable providers reject custom setters before save");
 }
 {
  Provider provider;Fixture f;assert(f.service.bindPrivateRuntime(provider));const auto before=f.service.config;
  for(unsigned mode=0;mode<8;++mode){auto a=channel(16);auto &c=a.set_channel;
   switch(mode){case 0:c.settings.psk.size=0;break;case 1:c.settings.psk.size=1;c.settings.psk.bytes[0]=0;break;case 2:c.settings.downlink_enabled=true;break;
    case 3:c.settings.psk.bytes[0]=0;for(const char x:"LongFast")c.settings.psk.bytes[0]^=uint8_t(x);break;
    case 4:strcpy(c.settings.name,"bad\x01");break;
    case 5:c.index=8;break;case 6:c.role=meshtastic_Channel_Role_SECONDARY;break;case 7:c.settings.uplink_enabled=true;break;}
   f.admin(a,mode&1);assert((mode&1?f.ble:f.usb).error()==meshtastic_Routing_Error_BAD_REQUEST);
   assert(f.service.config.same(before)&&!f.backend.writes&&!f.usb.saves&&!f.ble.saves&&!f.radio.sends);
  }
  group("encryption-off invalid-name zero-hash secondary and forwarding settings remain inert refusals");
 }
 {
  Provider provider;Fixture f;assert(f.service.bindPrivateRuntime(provider));auto a=channel(32);
  memset(a.set_channel.settings.psk.bytes,0,32);memcpy(a.set_channel.settings.psk.bytes,bench::MtNodeRecord::PublicKey,16);
  f.admin(a);assert(f.usb.error()==0&&!f.service.config.node().isPublic()&&get(f,false).settings.psk.size==32);
  f.admin(channel(1));assert(f.usb.error()==0&&f.service.config.node().isPublic()&&get(f,true).settings.psk.size==1);
  meshtastic_AdminMessage disabled{};disabled.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;
  disabled.set_channel.role=meshtastic_Channel_Role_DISABLED;f.admin(disabled);
  assert(f.usb.error()==0&&!f.service.config.provisioned()&&get(f,false).role==meshtastic_Channel_Role_DISABLED);
  f.admin(channel(1));assert(f.usb.error()==0&&f.service.config.node().isPublic());
  group("AES256 default-prefix remains custom; alias1 and disabled/default transitions preserve canonical semantics");
 }
 {
  Provider provider;Fixture f;assert(f.service.bindPrivateRuntime(provider));auto a=channel(16);f.admin(a);assert(!f.usb.error());
  Radio radio;mt::Service bench(radio,true,Node,81,false);assert(!bench.apply(f.service.config)&&!radio.sends);
  f.provision();assert(f.service.start(false,0)&&f.service.armEndpoint(true));
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;r.packet.id=0x751;r.packet.to=0xffffffffu;
  r.packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;r.packet.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
  r.packet.decoded.payload.size=1;r.packet.decoded.payload.bytes[0]='x';
  // Provider refuses signing: no fallback to custom-key boot-local public CTR.
  f.input(r);assert(!f.radio.sends&&f.service.endpointFree()==4);
  group("legacy bench refuses custom configuration and signed-message failure has no public fallback");
 }
 {
  mt::ui::Model model;assert(!strcmp(model.channelLabel(),"CHANNEL UNKNOWN"));
  assert(mt::ui::set(model.radio,true,false,nullptr,nullptr,0,0)&&!strcmp(model.channelLabel(),"CH0 DISABLED"));
  assert(mt::ui::set(model.radio,true,true,"Custom","LongSlow",906875000,-9)&&!strcmp(model.channelLabel(),"Custom"));
  assert(mt::ui::set(model.radio,true,true,"LongFast","LongFast",906875000,2)&&!strcmp(model.channelLabel(),"LongFast"));
  group("channel display distinguishes custom, exact default Public and disabled without exposing keys");
 }
 std::cout<<"PASS primary-channel suite: "<<groups<<" groups\n";
}
