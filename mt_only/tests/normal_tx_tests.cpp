// SPDX-License-Identifier: GPL-3.0-only
// M13 normal-use TX intent, migration, reboot and profile-boundary tests.
// Host-only mocks: no hardware, serial port, credentials or RF.
#include "device_config_fixture.h"
#include "NormalTxStartup.h"

static meshtastic_AdminMessage lora(bool enabled,uint8_t model=81) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
  a.set_config.which_payload_variant=meshtastic_Config_lora_tag;auto &l=a.set_config.payload_variant.lora;
  l.region=meshtastic_Config_LoRaConfig_RegionCode_US;l.use_preset=true;
  l.modem_preset=meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;l.hop_limit=3;
  l.tx_enabled=enabled;l.tx_power=model==127?-9:2;l.channel_num=20;l.sx126x_rx_boosted_gain=true;
  return a;
}

static void sendAdmin(mt::Companion &c,Host &host,const meshtastic_AdminMessage &a,uint32_t now=1) {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
  p.to=Node;p.id=72;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
  auto payload=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
  assert(pb_encode(&payload,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=payload.bytes_written;
  uint8_t bytes[512];auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&stream,meshtastic_ToRadio_fields,&r));
  host.now=now;c.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});c.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});c.byte(stream.bytes_written>>8,now,0,mt::Companion::WorkspaceProof{true});c.byte(stream.bytes_written,now,0,mt::Companion::WorkspaceProof{true});
  for(size_t i=0;i<stream.bytes_written;++i)c.byte(bytes[i],now,0,mt::Companion::WorkspaceProof{true});
}

static bool txReadback(mt::Companion &c,Host &host) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;
  a.get_config_request=meshtastic_AdminMessage_ConfigType_LORA_CONFIG;sendAdmin(c,host,a);
  const auto r=host.reply();assert(r.which_payload_variant==meshtastic_AdminMessage_get_config_response_tag);
  assert(r.get_config_response.which_payload_variant==meshtastic_Config_lora_tag);
  return r.get_config_response.payload_variant.lora.tx_enabled;
}

static bool txReadback(Fixture &f) {
  return txReadback(f.c,f.usb);
}

static void sameExceptMigration(const mt::Config &old,const mt::Config &now) {
  for(unsigned i=0;i<mt::Config::Size;++i)
    if(i!=2&&i!=83&&i!=mt::Config::Size-2&&i!=mt::Config::Size-1)assert(old.bytes[i]==now.bytes[i]);
}
static std::vector<uint8_t> version2(const mt::Config &current) {
  std::vector<uint8_t> old(mt::Config::PreviousSize,0);memcpy(old.data(),current.bytes,83);old[2]=2;
  const uint16_t crc=mt::Config::checksum(old.data(),94);old[94]=uint8_t(crc);old[95]=uint8_t(crc>>8);return old;
}

struct FaultRadio:Radio {
  int start()override{++starts;state=0;return -EIO;}
};

int main(){
 {
  mt::Config current;assert(current.valid()&&current.bytes[2]==mt::Config::Version&&!current.txIntentSpecified()&&!current.txEnabled());
  auto old=version2(current);mt::Config migrated;
  assert(mt::Config::decode(old.data(),old.size(),migrated)&&migrated.valid()&&migrated.bytes[2]==mt::Config::Version);
  assert(!migrated.txIntentSpecified()&&!migrated.txEnabled()&&!migrated.fixedPosition());
  for(unsigned i=0;i<83;++i)if(i!=2)assert(old[i]==migrated.bytes[i]);
  old[83]=1;const auto crc=mt::Config::checksum(old.data(),94);old[94]=uint8_t(crc);old[95]=uint8_t(crc>>8);
  assert(!mt::Config::decode(old.data(),old.size(),migrated));
  std::cout<<"PASS version-2 migration preserves every unrelated config byte and rejects reserved-byte smuggling\n";
 }
 {
  Backend backend;mt::SettingsJournal journal;assert(!journal.load(backend));
  auto cfg=journal.config();cfg.bytes[4]=1;cfg.seal();assert(!journal.save(backend,cfg));
  const auto before=journal.config();const auto writes=backend.writes;
  assert(!mt::NormalTxStartup::migrate(journal,backend,true));
  assert(backend.writes==writes+1&&journal.config().txIntent()==mt::Config::TxIntent::Enabled);
  sameExceptMigration(before,journal.config());
  mt::SettingsJournal reboot;assert(!reboot.load(backend)&&reboot.config().txEnabled());
  const auto stableWrites=backend.writes;assert(!mt::NormalTxStartup::migrate(reboot,backend,true)&&backend.writes==stableWrites);
  std::cout<<"PASS provisioned unspecified intent migrates once to durable enabled before startup\n";
 }
 {
  Backend backend;mt::SettingsJournal journal;assert(!journal.load(backend));
  auto cfg=journal.config();cfg.bytes[4]=1;cfg.setTxEnabled(false);assert(!journal.save(backend,cfg));
  const auto writes=backend.writes;assert(!mt::NormalTxStartup::migrate(journal,backend,true));
  assert(backend.writes==writes&&journal.config().txIntent()==mt::Config::TxIntent::Disabled);
  Backend freshBackend;mt::SettingsJournal fresh;assert(!fresh.load(freshBackend));
  assert(!mt::NormalTxStartup::migrate(fresh,freshBackend,true)&&freshBackend.writes==0);
  std::cout<<"PASS explicit disable and unprovisioned defaults are never overwritten by migration\n";
 }
 {
  for(unsigned mode=0;mode<4;++mode){
   Radio radio;const bool allow=mode!=2;mt::Service service(radio,allow,Node,81,true,false);
   auto cfg=service.config;if(mode!=3)cfg.bytes[4]=1;cfg.setTxEnabled(mode!=1);assert(service.apply(cfg));
   assert(mt::NormalTxStartup::activate(service,true,true,10));
   if(mode==3)assert(!service.receiving()&&!service.endpointArmed()&&!radio.starts);
   else assert(service.receiving()&&service.endpointArmed()==(mode==0)&&radio.starts==1);
  }
  Radio radio;radio.state=0;mt::Service legacy(radio,true,Node,81,false,false);auto cfg=legacy.config;cfg.bytes[4]=1;cfg.setTxEnabled(true);assert(legacy.apply(cfg));
  assert(mt::NormalTxStartup::activate(legacy,false,true,0)&&!legacy.receiving()&&!legacy.endpointArmed());
  Radio privateRadio;mt::Service privateLoad(privateRadio,true,Node,81,true,false);cfg=privateLoad.config;cfg.bytes[4]=1;cfg.setTxEnabled(true);assert(privateLoad.apply(cfg));
  assert(!mt::NormalTxStartup::bootPolicyEnabled(true,true));
  assert(mt::NormalTxStartup::activate(privateLoad,mt::NormalTxStartup::bootPolicyEnabled(true,true),true,0)&&
    !privateLoad.receiving()&&!privateLoad.endpointArmed()&&!privateRadio.starts);
  std::cout<<"PASS startup separates normal TX, explicit-disable, RX-only, unprovisioned, private-load and legacy profiles\n";
 }
 {
  FaultRadio radio;mt::Service service(radio,true,Node,81,true,false);auto cfg=service.config;
  cfg.bytes[4]=1;cfg.setTxEnabled(true);assert(service.apply(cfg));
  assert(!mt::NormalTxStartup::activate(service,true,true,0)&&service.faulted());
  assert(!service.receiving()&&!service.endpointArmed()&&radio.starts==1&&!radio.sends);
  std::cout<<"PASS radio-start fault holds normal profile off and latched without traffic\n";
 }
 {
  auto f=std::make_unique<Fixture>(81);f->admin(lora(true));assert(f->usb.error()==0);
  assert(f->service.receiving()&&f->service.endpointArmed()&&f->service.config.txEnabled()&&txReadback(*f));
  mt::SettingsJournal enabled;assert(!enabled.load(f->backend)&&enabled.config().txEnabled());
  f->admin(lora(false));assert(f->usb.error()==0&&f->service.receiving()&&!f->service.endpointArmed()&&!txReadback(*f));
  mt::SettingsJournal disabled;assert(!disabled.load(f->backend)&&disabled.config().txIntent()==mt::Config::TxIntent::Disabled);
  Radio rebootRadio;mt::Service reboot(rebootRadio,true,Node,81,true,false);assert(reboot.apply(disabled.config()));
  assert(mt::NormalTxStartup::activate(reboot,true,true,100)&&reboot.receiving()&&!reboot.endpointArmed());
  std::cout<<"PASS local LoRa enable and explicit disable persist across journal reload and reboot policy\n";
 }
 {
  auto f=std::make_unique<Fixture>(81);f->admin(lora(true));assert(f->usb.error()==0&&f->service.endpointArmed());
  f->begin();assert(!f->service.endpointArmed()&&txReadback(*f));f->commit();
  assert(f->usb.error()==0&&!f->edit.open()&&f->service.endpointArmed());
  f->begin();f->owner("ZephTastic");assert(f->usb.error()==0&&!f->edit.dirty()&&txReadback(*f));f->commit();
  assert(f->usb.error()==0&&!f->edit.open()&&f->service.endpointArmed());
  f->admin(lora(false));assert(f->usb.error()==0&&!f->service.endpointArmed());f->begin();f->commit();
  assert(f->usb.error()==0&&!f->service.endpointArmed()&&!txReadback(*f));
  std::cout<<"PASS empty and unchanged edit commits restore only an explicit durable enable\n";
 }
 {
  auto f=std::make_unique<Fixture>(81);f->admin(lora(true));assert(f->usb.error()==0&&f->service.endpointArmed());
  const auto writes=f->backend.writes;assert(!f->edit.begin(100)&&!f->service.endpointArmed());
  assert(!f->edit.poll(100+mt::SettingsEdit::IdleMs-1,true)&&f->edit.open()&&!f->service.endpointArmed());
  assert(!f->edit.poll(100+mt::SettingsEdit::IdleMs,true)&&!f->edit.open()&&f->service.endpointArmed()&&f->backend.writes==writes);
  assert(!f->edit.begin(1000));f->usb.now=1100;assert(!f->edit.save(f->service.config,1100)&&!f->edit.dirty());
  assert(!f->edit.poll(1100+mt::SettingsEdit::IdleMs,true)&&!f->edit.open()&&f->service.endpointArmed()&&f->backend.writes==writes);
  std::cout<<"PASS idle empty and unchanged transactions restore explicit durable enable without storage\n";
 }
 {
  auto f=std::make_unique<Fixture>(81);f->admin(lora(true));assert(f->usb.error()==0&&txReadback(*f));
  assert(f->service.armEndpoint(false)&&!f->service.endpointArmed()&&txReadback(*f));
  f->radio.state=0;f->service.rx(nullptr,0,0,0,0);
  assert(f->service.faulted()&&!f->service.endpointArmed()&&txReadback(*f));
  mt::SettingsJournal rebooted;assert(!rebooted.load(f->backend)&&rebooted.config().txEnabled());
  std::cout<<"PASS LoRa readback keeps durable TX intent across transient disarm and radio fault\n";
 }
 {
  auto f=std::make_unique<Fixture>(81);f->admin(lora(true));assert(f->usb.error()==0&&f->service.endpointArmed());
  auto changed=f->service.config;auto node=changed.node();node.setName("Normal use",false);changed.node(node);
  assert(!f->edit.save(changed,20)&&f->service.receiving()&&f->service.endpointArmed());
  f->begin(30);assert(!f->service.endpointArmed());f->owner("After batch",false,31);f->commit();
  assert(f->usb.error()==0&&f->service.receiving()&&f->service.endpointArmed());
  std::cout<<"PASS ordinary save and committed settings batch restore durable normal-use TX intent\n";
 }
 {
  auto f=std::make_unique<Fixture>(81);f->admin(lora(true));assert(f->usb.error()==0&&f->service.endpointArmed());
  auto changed=f->service.config;auto node=changed.node();node.setName("Must fail",false);changed.node(node);
  f->backend.fail=true;assert(f->edit.save(changed,20)<0&&!f->service.receiving()&&!f->service.endpointArmed());
  assert(!f->journal.ready()&&!f->radio.sends);
  std::cout<<"PASS uncertain persistence failure holds RX and TX off without synthetic traffic\n";
 }
 {
  Radio radio;Backend backend;mt::SettingsJournal journal;assert(!journal.load(backend));
  mt::Service service(radio,true,Node,81,false,false);mt::SettingsEdit edit(service,journal,backend);
  Host host(edit,journal);mt::companion::CompanionWorkspace workspace;mt::Companion companion(service,host,81,workspace);
  auto cfg=service.config;cfg.bytes[4]=1;cfg.setTxEnabled(true);assert(!edit.save(cfg,0));
  assert(service.start(false,0)&&service.armEndpoint(true)&&txReadback(companion,host));
  uint8_t frame[17]{};for(unsigned i=0;i<4;++i){frame[i]=0xff;frame[4+i]=uint8_t(Node>>(8*i));frame[8+i]=uint8_t(0x10203040u>>(8*i));}
  assert(!service.submitEndpoint(0,0x10203040,frame,sizeof(frame),1)&&service.endpointCount()==1&&!service.endpointArmed());
  service.poll(251,1);assert(radio.sends==1);radio.done=true;service.poll(352,2);assert(!service.active()&&!txReadback(companion,host));
  assert(!edit.begin(400)&&!edit.commit(401)&&!edit.open()&&!service.endpointArmed());
  assert(!edit.begin(500)&&!edit.save(service.config,501)&&!edit.dirty()&&!edit.commit(502)&&!edit.open());
  assert(!edit.begin(600));auto changed=service.config;auto owner=changed.node();assert(owner.setName("One packet saved",false));changed.node(owner);
  const auto writes=backend.writes;assert(!edit.save(changed,601)&&edit.dirty()&&!edit.commit(602)&&!edit.open());
  assert(backend.writes==writes+1&&journal.config().same(changed)&&service.receiving()&&!service.endpointArmed());
  assert(service.endpointCount()==1&&radio.sends==1&&!service.armEndpoint(true)&&!txReadback(companion,host));
  std::cout<<"PASS one-packet empty unchanged and dirty commits never restore arm or reset consumed quota\n";
 }
}
