// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <string.h>
#include "meshtastic/module_config.pb.h"

namespace mt::audio {
template<typename Enum> static uint32_t enumNumber(const Enum &value){
  static_assert(sizeof(Enum)==1||sizeof(Enum)==2||sizeof(Enum)==4);
  if constexpr(sizeof(Enum)==1){uint8_t raw;memcpy(&raw,&value,sizeof(raw));return raw;}
  else if constexpr(sizeof(Enum)==2){uint16_t raw;memcpy(&raw,&value,sizeof(raw));return raw;}
  else {uint32_t raw;memcpy(&raw,&value,sizeof(raw));return raw;}
}
struct AudioConfig {
  bool enabled=false;
  uint8_t pttPin=0,ws=0,dataIn=0,dataOut=0,clock=0;
  meshtastic_ModuleConfig_AudioConfig_Audio_Baud bitrate=meshtastic_ModuleConfig_AudioConfig_Audio_Baud_CODEC2_700C;
  static bool fromWire(const meshtastic_ModuleConfig &wire,AudioConfig &out) {
    if(wire.which_payload_variant!=meshtastic_ModuleConfig_audio_tag)return false;
    const auto &a=wire.payload_variant.audio;
    const uint32_t mode=enumNumber(a.bitrate);
    if(mode>uint32_t(meshtastic_ModuleConfig_AudioConfig_Audio_Baud_CODEC2_450))return false;
    AudioConfig c;c.enabled=a.codec2_enabled;c.pttPin=a.ptt_pin;c.ws=a.i2s_ws;c.dataIn=a.i2s_sd;c.dataOut=a.i2s_din;c.clock=a.i2s_sck;
    // Removed Codec2 700/700B modes map to the stock-compatible 700C mode.
    c.bitrate=(mode==0||mode==uint32_t(meshtastic_ModuleConfig_AudioConfig_Audio_Baud_CODEC2_700)||
      mode==uint32_t(meshtastic_ModuleConfig_AudioConfig_Audio_Baud_CODEC2_700B))?
      meshtastic_ModuleConfig_AudioConfig_Audio_Baud_CODEC2_700C:
      static_cast<meshtastic_ModuleConfig_AudioConfig_Audio_Baud>(mode);
    out=c;return true;
  }
};
}
