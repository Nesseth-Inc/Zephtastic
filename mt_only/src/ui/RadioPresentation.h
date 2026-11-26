// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "RadioView.h"
#include "../Config.h"
namespace mt::ui {
// Resolve from the same Config/profile/power policy used by the radio driver.
// Loading failures remain unknown, rather than displaying a disabled channel.
inline bool radioView(const Config &config,bool loaded,bool t096,RadioView &out) {
  RadioView staged;
  if(!loaded)return set(out,false,false,nullptr,nullptr,0,0);
  if(!config.valid())return false;
  if(!config.channelConfigured())return set(out,true,false,nullptr,nullptr,0,0);
  LoRaPresets::Profile profile{};USChannelSelection channel{};
  if(!config.resolveRadio(profile,channel)||
     !set(staged,true,true,config.effectivePrimaryName(),config.modemName(),channel.frequencyHz,config.effectivePower(t096)))return false;
  staged.regionUS=config.bytes[4]==1;staged.bandwidthHz=profile.bandwidthHz;staged.spreadingFactor=uint8_t(profile.spreadingFactor);
  staged.codingDenominator=uint8_t(profile.codingDenominator);out=staged;return true;
}
}
