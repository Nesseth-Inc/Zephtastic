// SPDX-License-Identifier: GPL-3.0-only
#include "modules/RouterRoleDefaults.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using Defaults=mt::modules::RouterRoleDefaults;

int main(){
  mt::Config client;
  assert(client.valid());
  assert(client.setDeviceRole(mt::DeviceRolePolicy::Role::CLIENT,
                              mt::DeviceRolePolicy::RebroadcastMode::ALL));
  assert(client.setNodeInfoSeconds(9000));
  auto gnss=client.gnssSettings();
  gnss.enabled=true;gnss.smart=false;gnss.positionFlags=5;gnss.updateSecs=31;
  gnss.attemptSecs=77;gnss.broadcastSecs=1234;gnss.smartMinDistance=444;
  gnss.smartMinInterval=555;
  assert(client.setGnssSettings(gnss));
  assert(client.setDisplay(99,true,333));
  client.setOwnerUnmessagable(false);

  mt::ModuleSettings modules;
  meshtastic_ModuleConfig telemetry{};
  assert(modules.get(telemetry,meshtastic_ModuleConfig_telemetry_tag));
  telemetry.payload_variant.telemetry.device_telemetry_enabled=true;
  telemetry.payload_variant.telemetry.device_update_interval=717;
  telemetry.payload_variant.telemetry.environment_measurement_enabled=true;
  telemetry.payload_variant.telemetry.environment_update_interval=515;
  telemetry.payload_variant.telemetry.power_measurement_enabled=true;
  telemetry.payload_variant.telemetry.power_update_interval=616;
  assert(modules.set(telemetry));
  meshtastic_ModuleConfig neighbor{};
  assert(modules.get(neighbor,meshtastic_ModuleConfig_neighbor_info_tag));
  neighbor.payload_variant.neighbor_info.enabled=true;
  neighbor.payload_variant.neighbor_info.transmit_over_lora=true;
  neighbor.payload_variant.neighbor_info.update_interval=21600;
  assert(modules.set(neighbor));
  assert(client.valid()&&modules.valid());

  auto requested=client;
  assert(requested.setDeviceRole(mt::DeviceRolePolicy::Role::ROUTER,
                                 mt::DeviceRolePolicy::RebroadcastMode::ALL));
  assert(requested.setNodeInfoSeconds(10800)); // candidate DeviceConfig survives projection
  Defaults::Projection projected{};
  assert(Defaults::project(client.role(),requested,modules,projected)==Defaults::Result::Applied);
  assert(projected.core.role()==mt::DeviceRolePolicy::Role::ROUTER);
  assert(projected.core.rebroadcastMode()==mt::DeviceRolePolicy::RebroadcastMode::CORE_PORTNUMS_ONLY);
  assert(projected.core.ownerUnmessagable());
  assert(projected.core.node().interval()==10800);
  const auto actualGnss=projected.core.gnssSettings();
  assert(actualGnss.enabled&&!actualGnss.smart&&actualGnss.positionFlags==5);
  assert(actualGnss.updateSecs==86400&&actualGnss.broadcastSecs==43200);
  assert(actualGnss.attemptSecs==77&&actualGnss.smartMinDistance==444&&actualGnss.smartMinInterval==555);
  assert(projected.core.displaySeconds()==1&&projected.core.displayFlip());
  assert(projected.core.displayCarouselSeconds()==333);
  assert(projected.unsupported==(Defaults::PowerConfigFields|Defaults::ModuleTelemetryFields));

  // Full module candidate contains both changed records in one staged value.
  meshtastic_ModuleConfig actualTelemetry{},actualNeighbor{};
  assert(projected.modules.get(actualTelemetry,meshtastic_ModuleConfig_telemetry_tag));
  assert(projected.modules.get(actualNeighbor,meshtastic_ModuleConfig_neighbor_info_tag));
  const auto &t=actualTelemetry.payload_variant.telemetry;
  assert(t.device_telemetry_enabled&&t.device_update_interval==43200);
  assert(t.environment_measurement_enabled&&t.environment_update_interval==0);
  assert(t.power_measurement_enabled&&t.power_update_interval==0);
  assert(actualNeighbor.payload_variant.neighbor_info.enabled&&
         actualNeighbor.payload_variant.neighbor_info.transmit_over_lora&&
         actualNeighbor.payload_variant.neighbor_info.update_interval==0);

  // A same-role Admin replacement must not reinstall Router defaults.
  auto sameRole=projected.core;
  assert(sameRole.setDeviceRole(mt::DeviceRolePolicy::Role::ROUTER,
                                mt::DeviceRolePolicy::RebroadcastMode::ALL));
  auto customGnss=sameRole.gnssSettings();customGnss.updateSecs=321;customGnss.broadcastSecs=654;
  assert(sameRole.setGnssSettings(customGnss));
  assert(sameRole.setDisplay(77,false,222));
  auto customModules=projected.modules;
  assert(customModules.get(actualTelemetry,meshtastic_ModuleConfig_telemetry_tag));
  actualTelemetry.payload_variant.telemetry.device_update_interval=919;
  assert(customModules.set(actualTelemetry));
  Defaults::Projection unchanged{};
  assert(Defaults::project(mt::DeviceRolePolicy::Role::ROUTER,sameRole,customModules,unchanged)==
         Defaults::Result::SameRole);
  assert(unchanged.core.same(sameRole)&&unchanged.modules.same(customModules));

  // Invalid input and a non-ROUTER destination leave output untouched.
  Defaults::Projection sentinel{client,modules,77};
  const auto sentinelCore=sentinel.core;const auto sentinelModules=sentinel.modules;
  auto invalidModules=modules;invalidModules.bytes[0]^=1;
  assert(Defaults::project(client.role(),requested,invalidModules,sentinel)==Defaults::Result::InvalidInput);
  assert(sentinel.core.same(sentinelCore)&&sentinel.modules.same(sentinelModules)&&sentinel.unsupported==77);
  auto leavingRouter=requested;
  assert(leavingRouter.setDeviceRole(mt::DeviceRolePolicy::Role::CLIENT,
                                     mt::DeviceRolePolicy::RebroadcastMode::ALL));
  assert(Defaults::project(mt::DeviceRolePolicy::Role::ROUTER,leavingRouter,modules,sentinel)==
         Defaults::Result::NotRouterTransition);
  assert(sentinel.core.same(sentinelCore)&&sentinel.modules.same(sentinelModules)&&sentinel.unsupported==77);

  std::puts("PASS M175 ROUTER role defaults: donor interval projection, full telemetry+neighbor candidate, preservation, same-role, invalid refusal; unsupported power/AQ/health/pax fields explicit");
}
