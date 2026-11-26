// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
#include <cassert>
#include <limits>
#include <vector>

namespace {
void resealV17(mt::Config &config) {
  config.bytes[2]=17;
  const auto crc=mt::Config::checksum(config.bytes,sizeof(config.bytes)-2);
  config.bytes[mt::Config::Size-2]=uint8_t(crc);
  config.bytes[mt::Config::Size-1]=uint8_t(crc>>8);
}

void appendVarint(std::vector<uint8_t> &out,uint64_t value) {
  while(value>=0x80){out.push_back(uint8_t(value)|0x80);value>>=7;}
  out.push_back(uint8_t(value));
}

std::vector<uint8_t> rawRegionAdmin(uint64_t region,bool duplicate=false,bool truncated=false) {
  std::vector<uint8_t> lora{0x38};appendVarint(lora,region); // LoRaConfig.region, field 7.
  if(duplicate){lora.push_back(0x38);appendVarint(lora,region);}
  if(truncated){lora.push_back(0x38);lora.push_back(0x80);}
  std::vector<uint8_t> config{0x32,uint8_t(lora.size())};config.insert(config.end(),lora.begin(),lora.end());
  std::vector<uint8_t> admin{0x92,0x02,uint8_t(config.size())};admin.insert(admin.end(),config.begin(),config.end());
  return admin;
}

void sendRawAdmin(Fixture &f,const std::vector<uint8_t> &admin) {
  meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;
  auto &packet=request.packet;packet.to=Node;packet.id=89;packet.want_ack=true;
  packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  packet.decoded.portnum=meshtastic_PortNum_ADMIN_APP;packet.decoded.want_response=true;
  assert(admin.size()<=sizeof(packet.decoded.payload.bytes));
  packet.decoded.payload.size=admin.size();memcpy(packet.decoded.payload.bytes,admin.data(),admin.size());
  f.input(request,false,5);
}
}

int main() {
  {
    mt::Config config;
    assert(config.valid()&&config.regionCode()==0&&!config.provisioned());
    assert(!strcmp(config.regionName(),"UNSET"));
    mt::LoRaPresets::Profile profile{};mt::USChannelSelection selection{};
    assert(config.resolveRadio(profile,selection)&&selection.regionCode==0);
    auto request=config.radioRequest();request.regionCode=0;request.usePreset=true;request.preset=6;request.channel=20;
    assert(config.setRadio(request));assert(config.regionCode()==0&&!config.provisioned());
    assert(config.resolveRadio(profile,selection)&&selection.regionCode==0);
    request=config.radioRequest();request.regionCode=1;
    assert(config.setRadio(request));assert(config.regionCode()==1&&config.provisioned());
    request=config.radioRequest();request.regionCode=3;request.usePreset=true;request.preset=10;
    request.channel=0;assert(config.setRadio(request));
    assert(config.regionCode()==29&&config.modemPreset()==10);
  }

  for(bool manual:{false,true})for(uint8_t oldRegion:{uint8_t(0),uint8_t(1)}) {
    mt::Config original;auto request=original.radioRequest();request.regionCode=oldRegion;
    request.usePreset=!manual;
    if(manual){request.bandwidth=250;request.spreadingFactor=11;request.codingRate=5;}
    else {request.preset=6;}
    request.channel=20;assert(original.setRadio(request));assert(original.regionCode()==oldRegion);
    resealV17(original);const auto v17=original;
    mt::Config migrated;assert(mt::Config::decode(v17.bytes,sizeof(v17.bytes),migrated));
    assert(migrated.valid()&&migrated.regionCode()==oldRegion);
    for(unsigned i=0;i<mt::Config::Size;++i)
      if(i!=2&&i<mt::Config::Size-2)assert(migrated.bytes[i]==v17.bytes[i]);
    assert(migrated.bytes[2]==mt::Config::Version);
    assert(migrated.bytes[mt::Config::Size-2]!=v17.bytes[mt::Config::Size-2]||
           migrated.bytes[mt::Config::Size-1]!=v17.bytes[mt::Config::Size-1]);
    auto corrupt=v17;corrupt.bytes[108]^=1;mt::Config unchanged;const auto before=unchanged;
    assert(!mt::Config::decode(corrupt.bytes,sizeof(corrupt.bytes),unchanged));assert(unchanged.same(before));
  }
  {
    auto old=mt::Config{};auto request=old.radioRequest();request.regionCode=0;assert(old.setRadio(request));
    resealV17(old);old.bytes[4]=2;const auto crc=mt::Config::checksum(old.bytes,sizeof(old.bytes)-2);
    old.bytes[mt::Config::Size-2]=uint8_t(crc);old.bytes[mt::Config::Size-1]=uint8_t(crc>>8);
    mt::Config out;assert(!mt::Config::decode(old.bytes,sizeof(old.bytes),out));
  }

  {
    Fixture f;
    meshtastic_AdminMessage request{};request.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
    request.set_config.which_payload_variant=meshtastic_Config_lora_tag;
    auto &l=request.set_config.payload_variant.lora;l.region=meshtastic_Config_LoRaConfig_RegionCode_UNSET;
    l.use_preset=true;l.modem_preset=meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW;
    l.channel_num=20;l.hop_limit=3;l.sx126x_rx_boosted_gain=true;l.tx_enabled=false;
    f.admin(request);assert(f.usb.error()==meshtastic_Routing_Error_NONE);
    assert(f.service.config.regionCode()==0&&!f.service.config.provisioned());
    assert(!f.radio.sends&&f.radio.starts==0&&!f.service.endpointArmed());
    auto enabled=request;enabled.set_config.payload_variant.lora.tx_enabled=true;
    const auto unchanged=f.service.config;const auto writes=f.backend.writes;
    f.admin(enabled,false,6);assert(f.usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    assert(f.service.config.same(unchanged)&&f.backend.writes==writes&&!f.radio.sends&&!f.radio.starts);
  }
  {
    Fixture f;f.radio.configurable=true;meshtastic_AdminMessage request{};
    request.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
    request.set_config.which_payload_variant=meshtastic_Config_lora_tag;
    auto &l=request.set_config.payload_variant.lora;
    l.region=meshtastic_Config_LoRaConfig_RegionCode_EU_866;l.use_preset=true;
    l.modem_preset=static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(10);
    l.channel_num=0;l.hop_limit=3;l.sx126x_rx_boosted_gain=true;
    f.admin(request);assert(f.usb.error()==meshtastic_Routing_Error_NONE);
    assert(f.service.config.regionCode()==29&&f.service.config.modemPreset()==10);
    assert(!f.radio.sends);
  }
  {
    Fixture f;f.radio.configurable=true;f.provision();
    meshtastic_AdminMessage request{};request.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
    request.set_config.which_payload_variant=meshtastic_Config_lora_tag;
    auto &l=request.set_config.payload_variant.lora;l.region=meshtastic_Config_LoRaConfig_RegionCode_US;
    l.use_preset=true;l.modem_preset=meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    l.channel_num=1;l.hop_limit=3;l.sx126x_rx_boosted_gain=true;
    f.admin(request);assert(f.usb.error()==meshtastic_Routing_Error_NONE);
    assert(f.service.config.regionCode()==1&&f.service.config.provisioned()&&!f.radio.sends);
  }
  for(bool ble:{false,true})for(unsigned preset:{0u,1u,3u,4u,5u,6u,7u,8u,9u,16u}) {
    Fixture f;f.radio.configurable=true;f.provision();
    meshtastic_AdminMessage request{};request.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
    request.set_config.which_payload_variant=meshtastic_Config_lora_tag;
    auto &l=request.set_config.payload_variant.lora;l.region=meshtastic_Config_LoRaConfig_RegionCode_US;
    l.use_preset=true;l.modem_preset=static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(preset);
    l.channel_num=1;l.hop_limit=3;l.sx126x_rx_boosted_gain=true;
    f.admin(request,ble);
    if((ble?f.ble:f.usb).error()!=meshtastic_Routing_Error_NONE)
      std::cerr<<"preset="<<preset<<" transport="<<(ble?"BLE":"USB")<<" error="<<(ble?f.ble.error():f.usb.error())<<"\n";
    assert((ble?f.ble:f.usb).error()==meshtastic_Routing_Error_NONE);
    assert(f.service.config.regionCode()==1&&f.service.config.modemPreset()==preset&&!f.radio.sends);
  }

  for(uint64_t region:{uint64_t(257),uint64_t(300),uint64_t(0xffffffffu),
                       uint64_t(0x100000001ULL),uint64_t(15),uint64_t(30),
                       uint64_t(31),uint64_t(13),uint64_t(17),uint64_t(27)}) {
    Fixture f;const auto before=f.service.config;const auto writes=f.backend.writes;
    sendRawAdmin(f,rawRegionAdmin(region));
    assert(f.usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    assert(f.service.config.same(before)&&f.backend.writes==writes&&!f.radio.sends&&!f.radio.starts);
  }
  for(const auto &raw:{rawRegionAdmin(1,true,false),rawRegionAdmin(1,false,true)}) {
    Fixture f;const auto before=f.service.config;const auto writes=f.backend.writes;
    sendRawAdmin(f,raw);assert(f.usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
    assert(f.service.config.same(before)&&f.backend.writes==writes&&!f.radio.sends&&!f.radio.starts);
  }
  for(uint8_t region:{uint8_t(13),uint8_t(15),uint8_t(17),uint8_t(27),uint8_t(30),uint8_t(31)}) {
    auto bytes=mt::Config{};bytes.bytes[4]=region;
    const auto crc=mt::Config::checksum(bytes.bytes,sizeof(bytes.bytes)-2);
    bytes.bytes[mt::Config::Size-2]=uint8_t(crc);bytes.bytes[mt::Config::Size-1]=uint8_t(crc>>8);
    mt::Config out;assert(!mt::Config::decode(bytes.bytes,sizeof(bytes.bytes),out));
  }
  {
    mt::Config inconsistent;
    auto initial=inconsistent.radioRequest();initial.regionCode=1;assert(inconsistent.setRadio(initial));
    auto request=inconsistent.radioRequest();request.regionCode=29;request.preset=10;request.channel=0;
    assert(inconsistent.setRadio(request));assert(inconsistent.regionCode()==29);
    inconsistent.bytes[4]=3;inconsistent.seal();
    assert(!inconsistent.valid());mt::Config output;const auto before=output;
    assert(!mt::Config::decode(inconsistent.bytes,sizeof(inconsistent.bytes),output));
    assert(output.same(before));
  }
  puts("PASS M129 UNSET/V17 migration, explicit provisioning, regional presets, raw full-width/duplicate/malformed Companion region admission");
}
