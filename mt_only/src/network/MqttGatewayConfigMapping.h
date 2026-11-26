// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "MqttGatewaySettings.h"

namespace mt { namespace network {

// Typed stock MQTTConfig conversion. Operational support is checked
// separately, so callers cannot mistake preserved settings for a live gateway.
// Empty address/user/password/root values remain empty; stock MQTT.cpp resolves
// its defaults at runtime, and this mapping never embeds factory credentials.
class MqttGatewayConfigMapping {
public:
  static bool fromConfig(const meshtastic_ModuleConfig_MQTTConfig &input,MqttGatewaySettings &out){
    MqttGatewaySettings staged;if(!staged.assign(input))return false;out=staged;return true;
  }
  static bool fromConfig(const meshtastic_ModuleConfig_MQTTConfig &input,
                         const MqttGatewaySettings &current,MqttGatewaySettings &out){
    MqttGatewaySettings staged=current;if(!staged.assign(input))return false;out=staged;return true;
  }
  static bool toConfig(const MqttGatewaySettings &input,meshtastic_ModuleConfig_MQTTConfig &out){
    return input.exportConfig(out);
  }
  static int operationalStatus(const MqttGatewaySettings &input,bool runtimeSupported=false,bool trustReady=false){
    return input.admissionStatus(runtimeSupported,trustReady);
  }
};

}} // namespace mt::network
