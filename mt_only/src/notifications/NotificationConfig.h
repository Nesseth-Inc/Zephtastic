// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "NotificationSettings.h"
#include "meshtastic/module_config.pb.h"

namespace mt {
// Only the board's declared LED is admitted. Zero selects that board default;
// arbitrary GPIO, PWM, vibration and audio need their own hardware adapters.
struct NotificationConfig {
  static bool set(NotificationSettings &out,const meshtastic_ModuleConfig &wire,bool available,uint32_t boardPin){
    if(wire.which_payload_variant!=meshtastic_ModuleConfig_external_notification_tag)return false;
    const auto &v=wire.payload_variant.external_notification;
    if(v.use_pwm||v.output_vibra||v.output_buzzer||v.alert_message_vibra||v.alert_message_buzzer||
       v.alert_bell_vibra||v.alert_bell_buzzer||v.use_i2s_as_buzzer||
       (v.output&&(!available||v.output!=boardPin))||(v.enabled&&!available))return false;
    return out.set(v.enabled,v.alert_message,v.alert_bell,v.active,v.output,v.output_ms,v.nag_timeout);
  }
  static bool get(const NotificationSettings &stored,meshtastic_ModuleConfig &out){
    if(!stored.valid())return false;
    out={};out.which_payload_variant=meshtastic_ModuleConfig_external_notification_tag;
    auto &v=out.payload_variant.external_notification;
    v.enabled=stored.enabled();v.alert_message=stored.alertMessage();v.alert_bell=stored.alertBell();
    v.active=stored.active();v.output=stored.output();v.output_ms=stored.outputMs();
    v.nag_timeout=uint16_t(stored.nagTimeoutSeconds());return true;
  }
};
}
