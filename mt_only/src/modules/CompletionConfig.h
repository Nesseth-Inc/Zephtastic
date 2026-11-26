// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "CompletionSettings.h"
#include "../ModuleSettings.h"
#include "../notifications/NotificationConfig.h"
#include "ExtendedModuleSettings.h"
namespace mt::modules {
// Older records retain authority for their existing fields. A role/default edit
// cannot accidentally resurrect stale device/environment/power settings from
// the supplemental record. Optional fields live only in the new namespace.
inline bool completionConfig(const CompletionSettings &supplement,const ModuleSettings &legacy,
  const NotificationSettings &notification,const ExtendedModuleSettings &extended,
  meshtastic_ModuleConfig &out,unsigned tag){
  const bool stored=supplement.get(out,tag);
  if(tag==meshtastic_ModuleConfig_telemetry_tag){
    meshtastic_ModuleConfig base{};if(!legacy.get(base,tag))return false;
    if(!stored)out=base;
    else {auto &v=out.payload_variant.telemetry;const auto &old=base.payload_variant.telemetry;
      v.device_telemetry_enabled=old.device_telemetry_enabled;v.device_update_interval=old.device_update_interval;
      v.environment_measurement_enabled=old.environment_measurement_enabled;v.environment_update_interval=old.environment_update_interval;
      v.power_measurement_enabled=old.power_measurement_enabled;v.power_update_interval=old.power_update_interval;}
    return true;
  }
  if(stored)return true;
  if(tag==meshtastic_ModuleConfig_external_notification_tag)return NotificationConfig::get(notification,out);
  if(tag==meshtastic_ModuleConfig_serial_tag||tag==meshtastic_ModuleConfig_canned_message_tag)return extended.get(out,tag);
  if(tag==meshtastic_ModuleConfig_audio_tag||tag==meshtastic_ModuleConfig_ambient_lighting_tag||tag==meshtastic_ModuleConfig_paxcounter_tag){
    out={};out.which_payload_variant=tag;return true;
  }
  return legacy.get(out,tag);
}
// Project only the old record's understood fields. Keep its bytes and format
// stable; complete generated config is independently staged in CompletionEdit.
inline meshtastic_ModuleConfig legacyProjection(const meshtastic_ModuleConfig &value){
  auto out=value;
  if(out.which_payload_variant==meshtastic_ModuleConfig_telemetry_tag){auto &v=out.payload_variant.telemetry;
    v.environment_screen_enabled=false;v.environment_display_fahrenheit=false;
    v.air_quality_enabled=false;v.air_quality_interval=0;v.air_quality_screen_enabled=false;
    v.power_screen_enabled=false;v.health_measurement_enabled=false;v.health_update_interval=0;v.health_screen_enabled=false;
  } else if(out.which_payload_variant==meshtastic_ModuleConfig_external_notification_tag){auto &v=out.payload_variant.external_notification;
    v.use_pwm=false;v.output_vibra=0;v.output_buzzer=0;v.alert_message_vibra=false;v.alert_bell_vibra=false;
    v.alert_message_buzzer=false;v.alert_bell_buzzer=false;v.use_i2s_as_buzzer=false;
  } else if(out.which_payload_variant==meshtastic_ModuleConfig_range_test_tag){out.payload_variant.range_test.save=false;out.payload_variant.range_test.clear_on_reboot=false;}
  return out;
}
} // namespace mt::modules
