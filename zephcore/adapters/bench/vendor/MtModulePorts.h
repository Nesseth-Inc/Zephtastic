// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
namespace bench {
// Wire-only policy keeps standalone codec callers independent of nanopb.
// Nanopb-aware signer/runtime/tests assert every value against the pinned
// generated PortNum enum. This grants no query, private or generic-port API.
constexpr uint32_t ModuleNodeStatusPort=36,ModuleRangeTestPort=66,
                   ModuleTelemetryPort=67,ModuleNeighborInfoPort=71,ModuleMeshBeaconPort=37,
                   ModuleAudioPort=9,ModulePaxcounterPort=34;
constexpr bool moduleBroadcastPort(uint32_t port) {
#if defined(CONFIG_MT_GPIO_MODULES)
  if(port==2||port==10)return true;
#endif
  return port==ModuleNodeStatusPort||port==ModuleRangeTestPort||
         port==ModuleTelemetryPort||port==ModuleNeighborInfoPort||port==ModuleMeshBeaconPort||
         port==ModuleAudioPort||port==ModulePaxcounterPort;
}
// Telemetry retains its separate existing receive path. PublicApplication
// accepts these independently verified signed module broadcasts.
constexpr bool publicSignedModulePort(uint32_t port) {
#if defined(CONFIG_MT_GPIO_MODULES)
  if(port==2||port==10)return true;
#endif
  return port==ModuleNodeStatusPort||port==ModuleRangeTestPort||port==ModuleNeighborInfoPort||port==ModuleMeshBeaconPort||
         port==ModuleAudioPort||port==ModulePaxcounterPort;
}
}
