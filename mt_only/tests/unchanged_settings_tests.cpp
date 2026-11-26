// SPDX-License-Identifier: GPL-3.0-only
// M17 strict unchanged owner/channel/LoRa saves. Memory-only USB/BLE framing;
// no physical transport, credentials or RF.
#include "device_config_fixture.h"
#include "NormalTxStartup.h"
#include "relay_fixture_provider.h"
#include <algorithm>

static meshtastic_AdminMessage owner(const char *longName=nullptr,const char *shortName=nullptr) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;
  if(longName)strcpy(a.set_owner.long_name,longName);
  if(shortName)strcpy(a.set_owner.short_name,shortName);
  return a;
}
static meshtastic_AdminMessage channel(bool enabled=true,bool fullKey=false) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;
  auto &c=a.set_channel;c.index=0;c.has_settings=true;c.settings.has_module_settings=true;
  c.role=enabled?meshtastic_Channel_Role_PRIMARY:meshtastic_Channel_Role_DISABLED;
  if(enabled) {
    c.settings.psk.size=fullKey?16:1;
    if(fullKey)memcpy(c.settings.psk.bytes,bench::MtNodeRecord::PublicKey,16);
    else c.settings.psk.bytes[0]=1;
  }
  return a;
}
static meshtastic_AdminMessage lora(uint8_t model,bool enabled,bool defaults=false,bool region=true) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
  a.set_config.which_payload_variant=meshtastic_Config_lora_tag;auto &l=a.set_config.payload_variant.lora;
  l.region=region?meshtastic_Config_LoRaConfig_RegionCode_US:meshtastic_Config_LoRaConfig_RegionCode_UNSET;
  l.use_preset=true;l.modem_preset=meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;l.hop_limit=3;
  l.tx_enabled=enabled;l.tx_power=model==127?-9:2;l.channel_num=defaults?0:20;l.sx126x_rx_boosted_gain=true;
  return a;
}
static std::vector<uint8_t> encodeAdmin(const meshtastic_AdminMessage &a) {
  uint8_t raw[512];auto out=pb_ostream_from_buffer(raw,sizeof(raw));assert(pb_encode(&out,meshtastic_AdminMessage_fields,&a));
  return {raw,raw+out.bytes_written};
}
static void rawAdmin(Fixture &f,const std::vector<uint8_t> &admin,bool ble,uint32_t now=1) {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
  p.to=Node;p.id=71;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
  assert(admin.size()<=sizeof(p.decoded.payload.bytes));memcpy(p.decoded.payload.bytes,admin.data(),admin.size());p.decoded.payload.size=admin.size();
  f.input(r,ble,now);
}
static void sendAdmin(mt::Companion &companion,Host &host,const meshtastic_AdminMessage &a,uint32_t now) {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
  p.to=Node;p.id=72;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
  auto payload=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
  assert(pb_encode(&payload,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=payload.bytes_written;
  uint8_t bytes[512];auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&stream,meshtastic_ToRadio_fields,&r));
  host.now=now;companion.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});companion.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});companion.byte(stream.bytes_written>>8,now,0,mt::Companion::WorkspaceProof{true});companion.byte(stream.bytes_written,now,0,mt::Companion::WorkspaceProof{true});
  for(size_t i=0;i<stream.bytes_written;++i)companion.byte(bytes[i],now,0,mt::Companion::WorkspaceProof{true});
}
static void explicitDefault(std::vector<uint8_t> &wire,const std::vector<uint8_t> &field) {
  assert(wire.size()>3&&size_t(wire[2])+3==wire.size()&&size_t(wire[2])+field.size()<128);
  wire[2]=uint8_t(wire[2]+field.size());wire.insert(wire.end(),field.begin(),field.end());
}
static void provision(Fixture &f,bool enabled) {
  auto cfg=f.service.config;cfg.bytes[4]=1;cfg.setTxEnabled(enabled);assert(!f.edit.save(cfg,0));
  assert(f.service.start(false,0));assert(f.service.armEndpoint(enabled));
}
static void noSideEffects(const Fixture &f,unsigned saves,unsigned writes,unsigned starts,unsigned stops,unsigned free,bool armed) {
  assert(f.usb.saves==saves&&f.ble.saves==0&&f.backend.writes==writes&&f.radio.starts==starts&&f.radio.stops==stops);
  assert(f.service.endpointFree()==free&&f.service.endpointArmed()==armed&&f.service.receiving()&&!f.service.faulted());
}

static meshtastic_AdminMessage readSetting(Fixture &f,pb_size_t tag,bool ble) {
  meshtastic_AdminMessage a{};a.which_payload_variant=tag;
  if(tag==meshtastic_AdminMessage_get_owner_request_tag)a.get_owner_request=true;
  if(tag==meshtastic_AdminMessage_get_channel_request_tag)a.get_channel_request=2;
  if(tag==meshtastic_AdminMessage_get_config_request_tag)a.get_config_request=meshtastic_AdminMessage_ConfigType_LORA_CONFIG;
  f.admin(a,ble);return (ble?f.ble:f.usb).reply();
}
static void androidProfile(bool ble,bool omitZeroMac) {
  uint8_t sampleKey[32];memset(sampleKey,0x65,sizeof(sampleKey));
  RelayFixtureProvider identity(Node,sampleKey);
  auto f=std::make_unique<Fixture>(meshtastic_HardwareModel_SEEED_XIAO_S3,false);
  f->radio.configurable=true;assert(f->service.bindPrivateRuntime(identity));
  const auto before=readSetting(*f,meshtastic_AdminMessage_get_owner_request_tag,ble).get_owner_response;
  assert(before.public_key.size==32&&before.id[0]&&before.hw_model==meshtastic_HardwareModel_SEEED_XIAO_S3);
  auto edit=owner();edit.set_owner=before;
  strcpy(edit.set_owner.long_name,"Restored XIAO");strcpy(edit.set_owner.short_name,"BETA");
  edit.set_owner.has_is_unmessagable=true;edit.set_owner.is_unmessagable=true;
  auto &transport=ble?f->ble:f->usb;
  for(unsigned i=0;i<7;++i){
    auto wrong=edit;auto &u=wrong.set_owner;
    switch(i){
    case 0:strcpy(u.id,"!deadbeef");break;
    case 1:u.hw_model=static_cast<meshtastic_HardwareModel>(127);break;
    case 2:u.role=meshtastic_Config_DeviceConfig_Role_ROUTER;break;
    case 3:u.is_licensed=true;break;
    case 4:u.macaddr[0]=1;break;
    case 5:u.public_key.bytes[0]^=1;break;
    case 6:u.public_key.size=31;break;
    }
    const auto original=f->service.config;f->admin(wrong,ble);
    assert(transport.error()==meshtastic_Routing_Error_BAD_REQUEST&&f->service.config.same(original));
  }
  auto wire=encodeAdmin(edit);
  if(omitZeroMac){
    // The stock proto3 encoder may omit the fixed-length all-zero MAC.
    const std::vector<uint8_t> mac{0x22,6,0,0,0,0,0,0};
    auto field=std::search(wire.begin()+3,wire.end(),mac.begin(),mac.end());
    assert(wire[0]==0x82&&wire[1]==2&&wire[2]==wire.size()-3&&wire[2]<128&&field!=wire.end());
    wire.erase(field,field+mac.size());wire[2]-=uint8_t(mac.size());
  }
  auto unknown=wire;unknown[2]+=2;unknown.insert(unknown.end(),{0x50,1});
  rawAdmin(*f,unknown,ble);assert(transport.error()==meshtastic_Routing_Error_BAD_REQUEST);
  f->op(meshtastic_AdminMessage_begin_edit_settings_tag,ble,30);assert(!transport.error());
  rawAdmin(*f,wire,ble,31);assert(!transport.error());
  auto ch=channel();ch.set_channel.index=1;ch.set_channel.role=meshtastic_Channel_Role_SECONDARY;
  strcpy(ch.set_channel.settings.name,"BetaAux");ch.set_channel.settings.psk.size=16;
  memset(ch.set_channel.settings.psk.bytes,0x45,16);
  f->admin(ch,ble,32);assert(!transport.error());
  auto req=lora(81,false);auto &radio=req.set_config.payload_variant.lora;
  radio.hop_limit=1;radio.fem_lna_mode=meshtastic_Config_LoRaConfig_FEM_LNA_Mode_NOT_PRESENT;
  f->admin(req,ble,33);assert(!transport.error());
  f->op(meshtastic_AdminMessage_commit_edit_settings_tag,ble,34);assert(!transport.error());
  auto cold=std::make_unique<Fixture>(meshtastic_HardwareModel_SEEED_XIAO_S3,false);
  cold->backend.entries=f->backend.entries;
  assert(!cold->journal.load(cold->backend)&&cold->service.apply(cold->journal.config())&&cold->service.bindPrivateRuntime(identity));
  const auto after=readSetting(*cold,meshtastic_AdminMessage_get_owner_request_tag,!ble).get_owner_response;
  assert(!strcmp(after.long_name,"Restored XIAO")&&!strcmp(after.short_name,"BETA")&&after.has_is_unmessagable&&after.is_unmessagable);
  assert(!strcmp(after.id,before.id)&&after.hw_model==before.hw_model&&after.role==before.role&&after.is_licensed==before.is_licensed&&
         after.public_key.size==32&&!memcmp(after.public_key.bytes,sampleKey,32)&&!memcmp(after.macaddr,before.macaddr,sizeof(after.macaddr)));
  const auto actual=readSetting(*cold,meshtastic_AdminMessage_get_config_request_tag,!ble).get_config_response.payload_variant.lora;
  assert(actual.region==radio.region&&actual.hop_limit==1&&actual.channel_num==20&&!actual.tx_enabled&&
         actual.fem_lna_mode==meshtastic_Config_LoRaConfig_FEM_LNA_Mode_NOT_PRESENT);
  const auto aux=readSetting(*cold,meshtastic_AdminMessage_get_channel_request_tag,!ble).get_channel_response;
  assert(aux.index==1&&aux.role==ch.set_channel.role&&!strcmp(aux.settings.name,"BetaAux")&&aux.settings.psk.size==16&&
         !memcmp(aux.settings.psk.bytes,ch.set_channel.settings.psk.bytes,16)&&!f->radio.sends&&!cold->radio.sends);
}

int main() {
  for(bool ble:{false,true})for(bool omit:{false,true})androidProfile(ble,omit);
  std::cout<<"PASS stock Android full owner, channel and LoRa profile through USB/BLE with cold readback and immutable-field refusal\n";
  for(bool ble:{false,true})for(uint8_t model:{uint8_t(81),uint8_t(127)}) {
    auto f=std::make_unique<Fixture>(model);provision(*f,true);const auto saved=f->service.config;
    const auto writes=f->backend.writes,starts=f->radio.starts,stops=f->radio.stops;
    auto send=[&](const meshtastic_AdminMessage &a,uint32_t now=1){f->admin(a,ble,now);assert((ble?f->ble:f->usb).error()==0);};
    send(owner("ZephTastic"));send(owner(nullptr,"ZT"));send(owner("ZephTastic","ZT"));
    send(channel(true,false));send(channel(true,true));send(lora(model,true,false));send(lora(model,true,true));
    assert(f->service.config.same(saved));
    if(ble)assert(f->ble.saves==0&&f->usb.saves==0);
    else noSideEffects(*f,0,writes,starts,stops,4,true);
    std::cout<<"PASS unchanged stock-shaped owner/channel/LoRa aliases are inert "<<unsigned(model)<<' '<<ble<<"\n";
  }
  {
    auto f=std::make_unique<Fixture>(81);provision(*f,true);text(*f,700,10);assert(f->service.endpointFree()==3);
    const auto writes=f->backend.writes,starts=f->radio.starts,stops=f->radio.stops;
    f->admin(owner("ZephTastic"));assert(f->usb.error()==0);f->admin(channel());assert(f->usb.error()==0);f->admin(lora(81,true,true));assert(f->usb.error()==0);
    noSideEffects(*f,0,writes,starts,stops,3,true);
    f->service.poll(300,9);assert(f->radio.sends==1&&f->service.active());
    const auto free=f->service.endpointFree();f->admin(owner(nullptr,"ZT"));assert(f->usb.error()==0);
    f->admin(channel(true,true));assert(f->usb.error()==0);f->admin(lora(81,true));assert(f->usb.error()==0);
    assert(f->radio.sends==1&&f->service.active()&&f->service.endpointFree()==free&&f->backend.writes==writes&&f->radio.starts==starts&&f->radio.stops==stops);
    std::cout<<"PASS unchanged saves preserve queued and active retry-capable TX plus runtime arm\n";
  }
  {
    auto f=std::make_unique<Fixture>(81);provision(*f,true);assert(f->service.armEndpoint(false));
    const auto writes=f->backend.writes,starts=f->radio.starts,stops=f->radio.stops;f->admin(lora(81,true));
    assert(f->usb.error()==0&&f->service.endpointArmed()&&f->usb.saves==1&&f->backend.writes==writes&&f->radio.starts==starts&&f->radio.stops==stops);
    f->radio.state=0;f->service.rx(nullptr,0,0,0,0);assert(f->service.faulted()&&!f->service.endpointArmed());
    f->admin(lora(81,true));assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&!f->service.endpointArmed());
    std::cout<<"PASS persisted equality never falsely ACKs runtime re-arm or a faulted radio\n";
  }
  {
    auto f=std::make_unique<Fixture>(81);provision(*f,true);const auto writes=f->backend.writes;
    f->begin(100);assert(!f->service.endpointArmed());assert(!f->edit.poll(100+mt::SettingsEdit::IdleMs-1,true)&&f->edit.open());
    assert(!f->edit.poll(100+mt::SettingsEdit::IdleMs,true)&&!f->edit.open()&&f->service.endpointArmed()&&f->backend.writes==writes);
    f->begin(1000);f->owner("ZephTastic",false,1100);assert(!f->edit.dirty()&&!f->service.endpointArmed());
    assert(!f->edit.poll(1100+mt::SettingsEdit::IdleMs-1,true)&&f->edit.open());
    assert(!f->edit.poll(1100+mt::SettingsEdit::IdleMs,true)&&!f->edit.open()&&f->service.endpointArmed()&&f->backend.writes==writes);
    std::cout<<"PASS clean empty and unchanged idle commits restore explicit durable TX without storage\n";
  }
  {
    auto f=std::make_unique<Fixture>(81);provision(*f,false);const auto writes=f->backend.writes,starts=f->radio.starts,stops=f->radio.stops;
    f->begin(100);f->admin(lora(81,false),false,200);assert(f->usb.error()==0&&!f->edit.dirty()&&f->edit.open());
    assert(f->backend.writes==writes&&f->radio.starts==starts+1&&f->radio.stops==stops+1&&f->service.receiving()&&!f->service.endpointArmed());
    assert(!f->edit.poll(100+mt::SettingsEdit::IdleMs,true)&&f->edit.open());
    assert(!f->edit.poll(200+mt::SettingsEdit::IdleMs,true)&&!f->edit.open()&&!f->service.endpointArmed()&&f->backend.writes==writes);
    std::cout<<"PASS unchanged disabled LoRa inside a batch refreshes activity without queue/radio/storage work\n";
  }
  for(bool uncertain:{false,true}) {
    auto f=std::make_unique<Fixture>(81);provision(*f,true);f->begin(100);f->owner("M17 pending",false,200);
    assert(f->usb.error()==0&&f->edit.open()&&f->edit.dirty());const auto live=f->service.config;
    const auto writesBeforeFailure=f->backend.writes,starts=f->radio.starts,stops=f->radio.stops,saves=f->usb.saves;
    f->owner("M17 pending",false,300);assert(f->usb.error()==0&&f->edit.dirty()&&f->backend.writes==writesBeforeFailure);
    f->admin(channel(),false,400);assert(f->usb.error()==0&&f->edit.dirty()&&f->backend.writes==writesBeforeFailure);
    f->admin(lora(81,true),false,500);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&f->edit.dirty());
    assert(f->radio.starts==starts&&f->radio.stops==stops&&f->service.config.same(live));
    f->backend.fail=true;f->backend.uncertain=uncertain;f->commit();
    assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&f->edit.open()&&f->edit.dirty());
    const auto writes=f->backend.writes,failedStarts=f->radio.starts,failedStops=f->radio.stops,failedSaves=f->usb.saves;
    f->admin(owner("M17 pending"));assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE);
    f->admin(channel());assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE);
    f->admin(lora(81,true));assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE);
    assert(f->backend.writes==writes&&f->radio.starts==failedStarts&&f->radio.stops==failedStops&&
      f->usb.saves==failedSaves&&failedSaves>=saves&&f->service.config.same(live)&&f->edit.open()&&f->edit.dirty());
  }
  std::cout<<"PASS dirty and failed-storage batches preserve pending state and refuse unsafe no-op success without retry\n";
  {
    for(unsigned kind=0;kind<3;++kind)for(bool ble:{false,true}) {
      auto f=std::make_unique<Fixture>(81);provision(*f,true);
      const auto request=kind==0?owner("ZephTastic"):kind==1?channel():lora(81,true);
      const auto canonical=encodeAdmin(request);const auto writes=f->backend.writes;
      auto unknown=canonical;explicitDefault(unknown,{0x98,0x06,0x00});rawAdmin(*f,unknown,ble);assert((ble?f->ble:f->usb).error()==meshtastic_Routing_Error_BAD_REQUEST);
      auto duplicate=canonical;duplicate.insert(duplicate.end(),canonical.begin(),canonical.end());rawAdmin(*f,duplicate,ble);assert((ble?f->ble:f->usb).error()==meshtastic_Routing_Error_BAD_REQUEST);
      auto session=canonical;session.insert(session.end(),{0xaa,0x06,0x01,0x42});rawAdmin(*f,session,ble);assert((ble?f->ble:f->usb).error()==meshtastic_Routing_Error_BAD_REQUEST);
      auto malformed=canonical;malformed.pop_back();rawAdmin(*f,malformed,ble);assert((ble?f->ble:f->usb).error()==meshtastic_Routing_Error_BAD_REQUEST);
      assert(f->backend.writes==writes&&f->service.endpointArmed()&&f->radio.sends==0);
    }
    std::cout<<"PASS unknown duplicate passkey-bearing and malformed no-op wires reject on USB and BLE\n";
  }
  {
    auto f=std::make_unique<Fixture>(81);provision(*f,true);const auto original=f->service.config;
    auto a=owner("ZephTastic");a.set_owner.hw_model=static_cast<meshtastic_HardwareModel>(99);f->admin(a);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    a=owner("ZephTastic");a.set_owner.has_is_unmessagable=true;a.set_owner.is_unmessagable=false;
    const auto writes=f->backend.writes;f->admin(a);
    assert(f->usb.error()==0&&f->backend.writes==writes&&!f->service.config.ownerUnmessagable()&&f->service.config.same(original));
    auto c=channel(false);c.set_channel.settings.name[0]='x';f->admin(c);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    c=channel();c.set_channel.role=static_cast<meshtastic_Channel_Role>(99);f->admin(c);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    auto l=lora(81,true);l.set_config.payload_variant.lora.region=static_cast<meshtastic_Config_LoRaConfig_RegionCode>(99);f->admin(l);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    assert(f->service.config.same(original)&&f->service.endpointArmed()&&f->radio.sends==0);
    std::cout<<"PASS disallowed owner/channel/LoRa metadata and raw enums reject without mutation\n";
  }
  {
    auto f=std::make_unique<Fixture>(81);provision(*f,true);const auto writes=f->backend.writes;
    f->admin(owner("M17 changed","M17"));assert(f->usb.error()==0&&f->backend.writes==writes+1&&f->service.endpointArmed());
    f->admin(channel(false));assert(f->usb.error()==0&&f->backend.writes==writes+2&&!f->service.receiving()&&!f->service.endpointArmed());
    std::cout<<"PASS changed saves retain verified persistence and radio-transition paths\n";
  }
  {
    Radio radio;Backend backend;mt::SettingsJournal journal;assert(!journal.load(backend));
    mt::Service rxOnly(radio,false,Node,81,true,false);mt::SettingsEdit edit(rxOnly,journal,backend);Host host(edit,journal);mt::companion::CompanionWorkspace workspace;mt::Companion companion(rxOnly,host,81,workspace);
    auto cfg=rxOnly.config;cfg.bytes[4]=1;cfg.setTxEnabled(false);assert(!edit.save(cfg,0)&&rxOnly.start(false,0));
    sendAdmin(companion,host,lora(81,false),1);assert(host.error()==0&&!rxOnly.endpointArmed()&&host.saves==0);
    sendAdmin(companion,host,lora(81,true),2);assert(host.error()==meshtastic_Routing_Error_BAD_REQUEST&&!rxOnly.endpointArmed());
    Radio oneRadio;mt::Service one(oneRadio,true,Node,81,false,false);assert(one.apply(cfg)&&one.start(false,0));
    mt::SettingsEdit oneEdit(one,journal,backend);Host oneHost(oneEdit,journal);mt::companion::CompanionWorkspace oneWorkspace;mt::Companion oneCompanion(one,oneHost,81,oneWorkspace);
    sendAdmin(oneCompanion,oneHost,lora(81,false),3);assert(oneHost.error()==0&&oneHost.saves==1&&!one.endpointArmed());
    std::cout<<"PASS RX-only disables enable and one-packet profile cannot take normal no-op shortcut\n";
  }
  static_assert(mt::NormalTxStartup::bootPolicyEnabled(true,false));
  static_assert(!mt::NormalTxStartup::bootPolicyEnabled(true,true));
  static_assert(!mt::NormalTxStartup::bootPolicyEnabled(false,false));
  std::cout<<"PASS private-load and non-normal profiles are excluded from normal boot activation\n";
}
