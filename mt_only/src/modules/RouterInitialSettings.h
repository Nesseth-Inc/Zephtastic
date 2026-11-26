// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "RouterRoleDefaults.h"
#include "CompletionJournal.h"
#include "../ModuleJournal.h"

namespace mt::modules {
// Only an absent journal can accept a default; verified records (including
// recovered records) and read failures remain under their original authority.
inline bool seedRouterDefaults(SettingsJournal &core, ModuleJournal &modules, CompletionJournal &completion) {
  if (!core.ready() || core.config().role() != RouterRoleDefaults::RoleRouter) return true;
  const bool freshCore = core.state() == SettingsJournal::State::Fresh;
  const bool freshModules = modules.state() == ModuleJournal::State::Fresh;
  const bool freshCompletion = completion.state() == CompletionJournal::State::Fresh;
  if (!freshCore && !freshModules && !freshCompletion) return true;

  RouterRoleDefaults::Projection projected;
  meshtastic_ModuleConfig telemetry{}, pax{};
  meshtastic_Config_PowerConfig power{};
  const auto result = RouterRoleDefaults::project(DeviceRolePolicy::Role::CLIENT_MUTE, core.config(),
      ModuleSettings{}, projected, &telemetry.payload_variant.telemetry, &power);
  if (result != RouterRoleDefaults::Result::Applied) return false;
  if (freshCompletion) {
    // A supplemental telemetry record overrides fields in the primary module
    // journal. Preserve those values when the operator has already set them.
    const auto &primary = freshModules ? projected.modules : modules.config();
    if (!primary.get(telemetry, meshtastic_ModuleConfig_telemetry_tag)) return false;
    telemetry.payload_variant.telemetry.air_quality_interval = projected.telemetry.air_quality_interval;
    telemetry.payload_variant.telemetry.health_update_interval = projected.telemetry.health_update_interval;
    pax.which_payload_variant = meshtastic_ModuleConfig_paxcounter_tag;
    pax.payload_variant.paxcounter.paxcounter_update_interval = projected.paxcounterInterval;
    CompletionSettings supplement;
    if (!supplement.setPower(projected.power) || !supplement.set(telemetry) || !supplement.set(pax)) return false;
    completion.seedFresh(supplement);
  }
  if (freshCore) core.seedFresh(projected.core);
  if (freshModules) modules.seedFresh(projected.modules);
  return true;
}
}
