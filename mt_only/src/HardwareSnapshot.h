// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include "Observed.h"
#include "meshtastic/telemetry.pb.h"
namespace mt {
// One main-thread snapshot feeds local, public, private and scheduled metrics.
// Absence is distinct from zero. The ADC does not detect battery presence.
struct HardwareSnapshot {
  zt::Observed<uint16_t> cellMilliVolts;
  zt::Observed<uint8_t> cellPercent;
  zt::Observed<bool> externalPower;
#if defined(CONFIG_MT_MAX17048)
  // Fuel-gauge charging and external supply are distinct observations.
  zt::Observed<bool> gaugeCharging;
#endif
  uint64_t capturedMs=0;
  HardwareSnapshot fresh(uint64_t now)const {
    return now>=capturedMs&&now-capturedMs<=30000?*this:HardwareSnapshot{};
  }
  void apply(meshtastic_DeviceMetrics &out)const {
    if(cellMilliVolts&&*cellMilliVolts>=2000&&*cellMilliVolts<=4500){
      out.has_voltage=true;out.voltage=float(*cellMilliVolts)/1000.0f;
      if(cellPercent&&*cellPercent<=100){out.has_battery_level=true;out.battery_level=*cellPercent;}
    }
    // Stock wire sentinel for externally powered. This says nothing about
    // whether a cell is installed, charging, or safe to charge.
#if defined(CONFIG_MT_MAX17048)
    if(gaugeCharging){
      if(*gaugeCharging){out.has_battery_level=true;out.battery_level=101;}
      return;
    }
#endif
    if(externalPower&&*externalPower){out.has_battery_level=true;out.battery_level=101;}
  }
};
}
