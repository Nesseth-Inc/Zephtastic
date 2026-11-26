// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../Config.h"

namespace mt::modules {
struct GpioModuleChannels {
  static bool gpio(const Config &config,unsigned slot) {
    ChannelRecord channel;
    if(slot>=8||!config.channelRecord(slot,channel)||!channel.role())return false;
    const char *name=channel.name()[0]?channel.name():config.modemName();
    const char expected[]="gpio";
    for(unsigned i=0;i<sizeof(expected);++i) {
      unsigned ch=static_cast<unsigned char>(name[i]);
      if(ch>='A'&&ch<='Z')ch+='a'-'A';
      if(ch!=static_cast<unsigned char>(expected[i]))return false;
      if(!ch)return true;
    }
    return false;
  }
  // Pinned Channels::isDefaultChannel(0) over our stock-facing primary
  // representation: Companion projects the normalized default key as alias 1.
  // The displayed name must also equal the active modem preset name. The
  // existing Config cannot recover whether that key originally arrived as a
  // full key or an alias; a renamed public-key channel remains distinct.
  static bool stockDefaultPrimary(const Config &config) {
    ChannelRecord channel;
    return config.channelRecord(0,channel)&&channel.role()&&
      PrimaryChannel::defaultKey(config.node())&&
      !strcmp(config.effectivePrimaryName(),config.modemName());
  }
};
}
