// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
#include "NormalTxStartup.h"
#include "RegionRuntimePolicy.h"
#include "modules/BeaconTargetRuntime.h"
#include <cmath>

namespace donor {
#include "my919_stock_hash.inc"
struct { const char *getName(unsigned)const{return name;} unsigned getPrimaryIndex()const{return 0;} const char *name="LongFast"; } channels;
struct DisplayFormatters {static const char *getModemPresetDisplayName(unsigned,bool,bool){return "LongFast";}};
constexpr int OVERRIDE_SLOT_PRESET_HASH=-1,OVERRIDE_SLOT_DEFAULT_CHANNEL_HASH=0;
uint32_t tune(unsigned channel, float bw, float overrideMHz=0, float offset=0) {
  struct Settings{unsigned modem_preset;bool use_preset;unsigned channel_num;float override_frequency,frequency_offset;};
  Settings loraConfig{0,true,channel,overrideMHz,offset};
  const auto *region=mt::radio::RegionPolicy::lookup(17);
  struct Profile{float spacing,padding;} profile{region->profile->spacingMHz,region->profile->paddingMHz};
  struct Region{float freqStart,freqEnd;int overrideSlot;Profile *profile;} geometry{region->freqStartMHz,region->freqEndMHz,region->overrideSlot,&profile};
  auto *newRegion=&geometry;
  int channel_num=0;bool uses_default_frequency_slot=false;float freq=0,result=0;
  auto saveChannelNum=[](int){};auto saveFreq=[&](float mhz){result=mhz;};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "my919_stock_slot.inc"
#pragma GCC diagnostic pop
  return uint32_t(llround(double(result)*1000000));
}
}

static meshtastic_AdminMessage request(unsigned region,unsigned channel,bool transmit) {
  meshtastic_AdminMessage message{};message.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
  message.set_config.which_payload_variant=meshtastic_Config_lora_tag;
  auto &l=message.set_config.payload_variant.lora;
  l.region=static_cast<meshtastic_Config_LoRaConfig_RegionCode>(region);
  l.use_preset=true;l.modem_preset=meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
  l.channel_num=channel;l.hop_limit=3;l.sx126x_rx_boosted_gain=true;l.tx_enabled=transmit;
  return message;
}
static void reject(Fixture &f,meshtastic_AdminMessage message) {
  const auto before=f.service.config;const auto writes=f.backend.writes,starts=f.radio.starts;
  f.admin(message);assert(f.usb.error()==meshtastic_Routing_Error_BAD_REQUEST);
  assert(f.service.config.same(before)&&f.backend.writes==writes&&f.radio.starts==starts&&!f.radio.sends);
}
int main() {
  Fixture f(81);f.radio.configurable=true;
  // Switch in one canonical stock SET from a previously TX-enabled US profile.
  f.admin(request(1,1,true));assert(f.usb.error()==0&&f.service.config.txEnabled());
  for(unsigned slot:{0u,1u,7u,20u}) {
    f.admin(request(17,slot,false));assert(f.usb.error()==0);
    const auto &cfg=f.service.config;
    assert(cfg.valid()&&cfg.regionCode()==17&&cfg.txIntent()==mt::Config::TxIntent::Disabled);
    mt::LoRaPresets::Profile profile{};mt::USChannelSelection selected{};
    assert(cfg.resolveRadio(profile,selected)&&selected.regionCode==17&&selected.slotCount==20);
    assert(selected.frequencyHz==donor::tune(slot,250));
    meshtastic_AdminMessage get{};get.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;
    get.get_config_request=meshtastic_AdminMessage_ConfigType_LORA_CONFIG;f.admin(get);
    const auto read=f.usb.reply().get_config_response.payload_variant.lora;
    assert(read.region==17&&!read.tx_enabled&&read.channel_num==slot);
  }
  auto frequency=request(17,1,false);auto &l=frequency.set_config.payload_variant.lora;
  l.override_frequency=922.5f;l.frequency_offset=.03125f;f.admin(frequency);assert(f.usb.error()==0);
  mt::LoRaPresets::Profile p{};mt::USChannelSelection s{};
  assert(f.service.config.resolveRadio(p,s)&&s.frequencyHz==donor::tune(1,250,922.5f,.03125f));
  l.override_frequency=928;reject(f,frequency);reject(f,request(17,21,false));reject(f,request(17,2,true));
  for(unsigned region:{13u,27u,28u,33u,37u,34u,35u,36u})reject(f,request(region,1,false));
  auto malformed=request(17,1,false);malformed.set_config.payload_variant.lora.sx126x_rx_boosted_gain=false;reject(f,malformed);
  for(uint8_t intent:{0u,2u}){
    auto bad=f.service.config;bad.bytes[83]=intent;bad.seal();mt::Config restored;
    assert(!bad.valid()&&!mt::Config::decode(bad.bytes,sizeof(bad.bytes),restored));
  }
  // Independent journal load and normal cold-start with the profile default TX on.
  Fixture cold(81);cold.radio.configurable=true;cold.backend=f.backend;
  assert(!cold.journal.load(cold.backend)&&cold.journal.config().same(f.service.config));
  auto writes=cold.backend.writes;assert(!mt::NormalTxStartup::migrate(cold.journal,cold.backend,true)&&writes==cold.backend.writes);
  assert(cold.service.apply(cold.journal.config()));
  assert(!cold.service.start(true,4));
  assert(mt::NormalTxStartup::activate(cold.service,true,true,7)&&cold.service.receiving());
  assert(!cold.service.relaying()&&!cold.service.endpointArmed()&&!cold.service.armEndpoint(true));
  assert(cold.radio.selected.resolveRadio(p,s)&&s.frequencyHz==donor::tune(1,250,922.5f,.03125f));
  assert(mt::radio::RegionRuntimePolicy::duty(17,false,false,true,0)==mt::TxAdmission::Decision::Unavailable);
  mt::modules::BeaconTargetRuntime beacons(cold.service);
  auto target=mt::modules::BeaconTargetRuntime::runningProfile(cold.service.config);
  assert(!beacons.targetSupported(target));
  cold.service.poll(20000,2);assert(!cold.radio.sends);
  cold.admin(request(1,1,true));assert(cold.usb.error()==0&&cold.service.config.txEnabled());
  cold.admin(request(17,1,false));assert(cold.usb.error()==0);
  cold.admin(request(0,1,false));assert(cold.usb.error()==0&&!cold.service.config.provisioned()&&!cold.service.receiving());
  puts("PASS XIAO MY_919 stock fixed-slot oracle, canonical SET/GET, disabled-only reload, cold RX, relay/arm/beacon refusal, US and UNSET transitions");
}
