// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
#include <limits>
static meshtastic_AdminMessage radioConfig(unsigned preset,uint32_t channel=20) {
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
 a.set_config.which_payload_variant=meshtastic_Config_lora_tag;auto &l=a.set_config.payload_variant.lora;
 l.region=meshtastic_Config_LoRaConfig_RegionCode_US;l.use_preset=true;l.modem_preset=static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(preset);
 l.channel_num=channel;l.hop_limit=3;l.sx126x_rx_boosted_gain=true;return a;
}
static auto radioGetter(Fixture &f,bool ble) {
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;
 a.get_config_request=meshtastic_AdminMessage_ConfigType_LORA_CONFIG;f.admin(a,ble);return (ble?f.ble:f.usb).reply().get_config_response.payload_variant.lora;
}
static void group(const char *s){std::cout<<"PASS "<<s<<'\n';}
int main(){
 for(bool ble:{false,true})for(unsigned model:{81u,127u}){
  auto f=std::make_unique<Fixture>(uint8_t(model),false);f->radio.configurable=true;f->provision();
  auto &transport=ble?f->ble:f->usb;
  auto request=radioConfig(0,20);auto &l=request.set_config.payload_variant.lora;
  const auto initial=f->service.config;
  l.fem_lna_mode=meshtastic_Config_LoRaConfig_FEM_LNA_Mode_ENABLED;
  f->admin(request,ble);assert(transport.error()==meshtastic_Routing_Error_BAD_REQUEST&&f->service.config.same(initial));
  l.fem_lna_mode=meshtastic_Config_LoRaConfig_FEM_LNA_Mode_NOT_PRESENT;
  l.override_duty_cycle=true;
  f->admin(request,ble);assert(transport.error()==meshtastic_Routing_Error_BAD_REQUEST&&f->service.config.same(initial));
  l.override_duty_cycle=false;
  f->admin(request,ble);
  assert(transport.error()==(model==81?meshtastic_Routing_Error_NONE:meshtastic_Routing_Error_BAD_REQUEST));
  l.fem_lna_mode=meshtastic_Config_LoRaConfig_FEM_LNA_Mode_DISABLED;
  f->admin(request,ble);assert(!transport.error()&&!f->radio.sends);
  assert(radioGetter(*f,!ble).fem_lna_mode==(model==81?meshtastic_Config_LoRaConfig_FEM_LNA_Mode_NOT_PRESENT:
                                                     meshtastic_Config_LoRaConfig_FEM_LNA_Mode_DISABLED));
 }
 group("XIAO external FEM absence accepts stock and legacy echoes without enabling an amplifier or bypassing radio policy");
 for(bool ble:{false,true})for(unsigned preset:{0u,1u,3u,4u,5u,6u,7u,8u,9u,16u}){
  auto f=std::make_unique<Fixture>();f->radio.configurable=true;f->provision();const auto before=f->service.config;auto a=radioConfig(preset,1);f->admin(a,ble);assert((ble?f->ble:f->usb).error()==0);
  assert(f->service.receiving()&&!f->service.endpointArmed()&&!f->radio.sends&&f->radio.selected.modemPreset()==preset&&f->radio.selected.channelNumber()==1);
  assert(!memcmp(f->service.config.node().bytes+bench::MtNodeRecord::Key,before.node().bytes+bench::MtNodeRecord::Key,32)&&!memcmp(f->service.config.bytes+84,before.bytes+84,20));assert(mt::PrimaryChannel::defaultKey(f->service.config.node())&&f->service.config.channelConfigured());auto l=radioGetter(*f,!ble);assert(unsigned(l.modem_preset)==preset&&l.channel_num==1&&l.hop_limit==3&&!l.tx_enabled);
  mt::SettingsJournal reboot;assert(!reboot.load(f->backend)&&reboot.config().same(f->service.config));const auto writes=f->backend.writes,starts=f->radio.starts;f->admin(a,ble);assert((ble?f->ble:f->usb).error()==0&&f->backend.writes==writes&&f->radio.starts==starts);
 }
 group("ten presets traverse actual USB/BLE setters, opposite getters, durable reboot and unchanged no-op with zero RF");
 for(unsigned mode=0;mode<8;++mode){auto f=std::make_unique<Fixture>();auto a=radioConfig(0);auto &l=a.set_config.payload_variant.lora;
  switch(mode){case 0:l.hop_limit=8;break;case 1:l.override_duty_cycle=true;break;case 2:l.config_ok_to_mqtt=true;break;case 3:l.region=meshtastic_Config_LoRaConfig_RegionCode_EU_868;break;case 4:l.use_preset=false;l.bandwidth=8;l.coding_rate=4;break;case 5:l.use_preset=false;l.bandwidth=123;break;case 6:l.override_frequency=std::numeric_limits<float>::quiet_NaN();break;case 7:l.override_frequency=929;break;}
  const auto before=f->service.config;f->admin(a);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&f->service.config.same(before)&&!f->backend.writes&&!f->radio.starts&&!f->radio.sends);
 }
 for(int power:{-129,-128,127,128}){
  uint8_t wire[16]{};auto output=pb_ostream_from_buffer(wire,sizeof(wire));
  assert(pb_encode_tag(&output,PB_WT_VARINT,meshtastic_Config_LoRaConfig_tx_power_tag)&&pb_encode_varint(&output,uint64_t(int64_t(power))));
  meshtastic_Config_LoRaConfig decoded{};auto input=pb_istream_from_buffer(wire,output.bytes_written);
  const bool accepted=pb_decode(&input,meshtastic_Config_LoRaConfig_fields,&decoded);
  assert(accepted==(power>=-128&&power<=127));if(accepted)assert(decoded.tx_power==power);
 }
 group("unsupported region hop control flags manual width CR4 and invalid frequency refuse atomically");
 for(bool ble:{false,true})for(unsigned preset:{2u,17u}){
  auto f=std::make_unique<Fixture>();f->radio.configurable=true;f->provision();auto a=radioConfig(preset,105);a.set_config.payload_variant.lora.bandwidth=125;
  f->admin(a,ble);assert((ble?f->ble:f->usb).error()==0);const auto &cfg=f->service.config;
  assert(cfg.modemPreset()==0&&cfg.channelNumber()==20&&cfg.usePreset()&&cfg.radioRequest().bandwidth==125&&cfg.effectivePower(true)==22&&!f->radio.sends);
  const auto l=radioGetter(*f,!ble);assert(l.use_preset&&unsigned(l.modem_preset)==0&&l.channel_num==20&&l.bandwidth==125&&l.tx_power==30);
 }
 group("stock Admin normalizes unsupported presets and oversized channels while retaining preset bandwidth and auto power");
 for(bool ble:{false,true})for(uint32_t bandwidth:{8u,10u,16u,21u,31u,42u,62u,125u,250u,500u}){
  auto f=std::make_unique<Fixture>();f->radio.configurable=true;f->provision();auto a=radioConfig(0,0);auto &l=a.set_config.payload_variant.lora;
  l.use_preset=false;l.bandwidth=bandwidth;l.spread_factor=9;l.coding_rate=5;l.override_frequency=915;l.frequency_offset=0.125f;l.tx_power=30;
  f->admin(a,ble);assert((ble?f->ble:f->usb).error()==0);const auto &cfg=f->service.config;assert(!cfg.usePreset()&&cfg.channelNumber()==0&&cfg.effectivePower(true)==22&&!strcmp(cfg.effectivePrimaryName(),"Custom"));
  const auto got=radioGetter(*f,!ble);assert(!got.use_preset&&got.bandwidth==bandwidth&&got.spread_factor==9&&got.coding_rate==5&&got.override_frequency==915&&got.frequency_offset==0.125f&&got.tx_power==30);
  mt::LoRaPresets::Profile profile{};mt::USChannelSelection selected{};assert(cfg.resolveRadio(profile,selected)&&selected.frequencyHz==915125000&&profile.spreadingFactor==9&&profile.codingDenominator==5);
  mt::SettingsJournal reboot;assert(!reboot.load(f->backend)&&reboot.config().same(cfg)&&f->service.receiving()&&!f->radio.sends);
 }
 group("all ten manual widths cross USB BLE Admin getter durable reboot and retained frequency with zero RF");
 for(int power:{-128,-9,0,2,30,127}){
  auto f=std::make_unique<Fixture>();f->radio.configurable=true;f->provision();auto a=radioConfig(0);auto &l=a.set_config.payload_variant.lora;l.use_preset=false;l.tx_power=power;
  f->admin(a);assert(f->usb.error()==0);const auto got=radioGetter(*f,true);assert(!got.use_preset&&got.bandwidth==250&&got.spread_factor==11&&got.coding_rate==5&&got.tx_power==(power==0||power>30?30:power));
  assert(f->service.config.effectivePower(true)==(power< -9?-9:power==0||power>22?22:power)&&!f->radio.sends);
 }
 group("manual defaults normalize zero fields and getter regional power remains distinct from chip power");
 {auto f=std::make_unique<Fixture>();f->radio.configurable=true;f->provision();assert(f->service.start(false,0));f->begin();const auto writes=f->backend.writes;f->admin(radioConfig(4,0));assert(f->usb.error()==0&&f->edit.open()&&f->edit.dirty()&&f->backend.writes==writes&&f->service.config.modemPreset()==4&&f->radio.selected.modemPreset()==4);f->commit();assert(f->usb.error()==0&&!f->edit.open());mt::SettingsJournal reboot;assert(!reboot.load(f->backend)&&reboot.config().modemPreset()==4&&reboot.config().channelNumber()==0&&!f->radio.sends);}
 group("cross-transport edit exposes modem selection and commits once with ordinary journal semantics");
 {auto f=std::make_unique<Fixture>();f->radio.configurable=true;f->provision();f->admin(radioConfig(4,1));assert(f->usb.error()==0);meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=81;f->usb.out.clear();f->input(r);bool seen=false;for(const auto &m:f->usb.out)if(m.which_payload_variant==meshtastic_FromRadio_channel_tag&&m.channel.index==0){assert(!m.channel.settings.name[0]&&!strcmp(f->service.config.effectivePrimaryName(),"MediumFast")&&m.channel.settings.psk.size==1&&m.channel.settings.psk.bytes[0]==1);seen=true;}assert(seen&&!f->radio.sends);}
 group("new-empty-primary-name-follows-preset-with-matching-default-alias-hash");
 return 0;
}
