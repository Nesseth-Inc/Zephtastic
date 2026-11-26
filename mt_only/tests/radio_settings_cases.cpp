// SPDX-License-Identifier: GPL-3.0-only
// Exact pinned stock preset function and actual NativeRadio methods are
// extracted by test-radio-settings.py. Driver I/O below is synthetic.
#include "Config.h"
#include "meshtastic/config.pb.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <climits>
#include <vector>
#define PRESET(name) meshtastic_Config_LoRaConfig_ModemPreset_##name
#include "stock-modem-function.h"
#include "stock-preset-names.h"
#include "native-modem-methods.h"

static void group(const char *s){std::cout<<"PASS "<<s<<'\n';}
int main() {
  for(unsigned i=0;i<17;++i){
    mt::LoRaPresets::Profile p{};assert(mt::LoRaPresets::lookup(i,p));
    for(bool wide:{false,true}){
      float bw=0;uint8_t sf=0,cr=0;
      modemPresetToParams(static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(i),wide,bw,sf,cr);
      assert(uint32_t(std::lround(bw*1000))==(wide?p.wideBandwidthHz:p.bandwidthHz));
      assert(sf==p.spreadingFactor&&cr==p.codingDenominator);
    }
  }
  mt::LoRaPresets::Profile sentinel{1,2,3,4};
  for(unsigned i:{17u,UINT_MAX}){assert(!mt::LoRaPresets::lookup(i,sentinel));assert(sentinel.bandwidthHz==1&&sentinel.wideBandwidthHz==2&&sentinel.spreadingFactor==3&&sentinel.codingDenominator==4);}
  group("all17 preset rows match exact stock function for standard and wide tables; unsupported IDs are atomic");
  for(uint32_t bw:{125000u,250000u,500000u}){
    const uint32_t count=26000000u/bw;
    for(uint32_t ch=1;ch<=count;++ch){
      mt::USChannelSelection out{};assert(mt::USChannel::lookup(bw,ch,"LongFast",out));
      assert(out.slotCount==count&&out.slot==ch-1&&out.frequencyHz==902000000+bw/2+(ch-1)*bw);
    }
    mt::USChannelSelection out{1,2,3,4};assert(!mt::USChannel::lookup(bw,count+1,"LongFast",out));assert(out.slotCount==1&&out.slot==2&&out.frequencyHz==3&&out.regionCode==4);
  }
  mt::USChannelSelection channel{};assert(mt::USChannel::lookup(250000,0,"LongFast",channel)&&channel.slot==19&&channel.frequencyHz==906875000);
  for(uint32_t bw:{0u,15600u,62500u,812500u,UINT_MAX}){const auto before=channel;assert(!mt::USChannel::lookup(bw,1,"LongFast",channel)&&!memcmp(&before,&channel,sizeof(channel)));}
  const auto channelBefore=channel;assert(!mt::USChannel::lookup(250000,UINT_MAX,"LongFast",channel)&&!memcmp(&channelBefore,&channel,sizeof(channel)));
  assert(!mt::USChannel::lookup(250000,0,nullptr,channel));group("all364 US channel centers, auto LongFast slot19 and atomic invalid selections");
  mt::Config initial;assert(initial.valid()&&initial.bytes[2]==mt::Config::Version&&initial.modemPreset()==0&&initial.channelNumber()==20&&sizeof(initial)==640);
  for(uint8_t version=4;version<=9;++version){
    std::vector<uint8_t> old(initial.bytes,initial.bytes+mt::Config::FormerSize);old[2]=version;memset(old.data()+104,0,6);
    const auto sealOld=[](std::vector<uint8_t> &b){const auto sum=mt::Config::checksum(b.data(),b.size()-2);b[b.size()-2]=uint8_t(sum);b.back()=uint8_t(sum>>8);};sealOld(old);
    mt::Config image;memcpy(image.bytes,old.data(),old.size());assert(image.validBody(version));
    mt::Config out;assert(out.setDisplay(137,true));assert(mt::Config::decode(old.data(),old.size(),out));
    assert(out.valid()&&out.bytes[2]==mt::Config::Version&&out.modemPreset()==0&&out.channelNumber()==20);
    assert(!memcmp(old.data(),out.bytes,2)&&!memcmp(old.data()+3,out.bytes+3,101)&&out.role()==1&&out.rebroadcastMode()==0&&!out.primaryName()[0]);
    for(unsigned byte=104;byte<110;++byte){auto bad=old;bad[byte]=1;sealOld(bad);const auto before=out;assert(!mt::Config::decode(bad.data(),bad.size(),out)&&out.same(before));}
  }
  group("original112 v4 through v9 migrate to v12 preserving prior fields and refusing nonzero reserved bytes atomically");
  for(unsigned preset=0;preset<17;++preset){
    const bool allowed=preset==0||preset==1||(preset>=3&&preset<=9)||preset==16;
    auto c=initial;const auto before=c;assert(c.setLoRa(preset,1)==allowed);
    if(!allowed){assert(c.same(before));continue;}
    assert(c.valid()&&c.modemPreset()==preset&&c.channelNumber()==1);
    assert(!strcmp(c.effectivePrimaryName(),DisplayFormatters::getModemPresetDisplayName(static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(preset),false,true)));
    mt::Config decoded;assert(mt::Config::decode(c.bytes,sizeof(c),decoded)&&decoded.same(c));
    mt::LoRaPresets::Profile p{};mt::USChannelSelection s{};assert(mt::RadioSettings::resolve(preset,0,p,s));
    auto clamped=c;assert(clamped.setLoRa(preset,26000000/p.bandwidthHz+1));
    mt::LoRaPresets::Profile autoProfile{};mt::USChannelSelection autoChannel{};
    assert(mt::RadioSettings::resolve(preset,0,autoProfile,autoChannel,c.effectivePrimaryName()));
    assert(clamped.channelNumber()==uint32_t(autoChannel.slot)+1);
    NativeAdapter adapter;configCalls=0;restoreCalls=0;purges=0;
    assert(adapter.start()==-EINVAL&&configCalls==0);
    assert(adapter.configure(c)==0&&configCalls==0);assert(adapter.start()==0&&configCalls==1&&restoreCalls==1&&purges==1);
    assert(applied.frequency==902000000+p.bandwidthHz/2&&applied.datarate==p.spreadingFactor&&applied.coding_rate==p.codingDenominator-4);
    assert(applied.bandwidth==(p.bandwidthHz==125000?BW_125_KHZ:p.bandwidthHz==250000?BW_250_KHZ:BW_500_KHZ));
    assert(applied.tx_power==(CONFIG_BOARD_HELTEC_T096?-9:2)&&applied.preamble_len==16&&applied.sync_word==0x2b);
  }
  group("ten US presets persist and exact native configure/start applies frequency/BW/SF/CR with retained board power");
  for(unsigned precision:{0u,1u,15u,32u}){auto c=initial;c.bytes[122]=uint8_t(precision);c.seal();mt::Config out;NativeAdapter a;
    assert(mt::Config::decode(c.bytes,sizeof(c),out)&&out.same(c)&&out.primaryPositionPrecision()==precision&&!out.positionPrecision(0));
    configCalls=0;assert(a.configure(out)==0&&configCalls==0);}
  for(unsigned mode=0;mode<6;++mode){auto c=initial;switch(mode){case 0:c.bytes[104]=2;break;case 1:c.bytes[105]=105;break;case 2:c.bytes[122]=33;break;case 3:c.bytes[2]=mt::Config::Version+1;break;case 4:c.bytes[109]=1;break;case 5:c.bytes[123]=2;break;}c.seal();mt::Config out;const auto before=out;assert(!mt::Config::decode(c.bytes,sizeof(c),out)&&out.same(before));NativeAdapter a;assert(a.configure(c)==-EINVAL&&a.start()==-EINVAL);}
  auto downlink=initial;downlink.bytes[123]=1;downlink.seal();mt::Config downlinkDecoded;
  assert(mt::Config::decode(downlink.bytes,sizeof(downlink),downlinkDecoded)&&downlinkDecoded.primaryDownlinkEnabled());
  group("current precision0..32 stays configuration-only; invalid precision preset channel name version and downlink enum refuse without mutation");
  return 0;
}
