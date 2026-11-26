// SPDX-License-Identifier: GPL-3.0-only
#include "modules/TakRoleDefaults.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using Defaults = mt::modules::TakRoleDefaults;
using Projection = Defaults::Projection;

static void unchanged(const Projection &a, const Projection &b) {
  assert(a.core.same(b.core));
  assert(a.modules.same(b.modules));
  assert(a.ownerUnmessagableRequired == b.ownerUnmessagableRequired);
}

int main() {
#ifndef CONFIG_MT_TAK_TRACKER
#error This fixture must build with CONFIG_MT_TAK_TRACKER enabled.
#endif
  mt::Config core;
  assert(core.valid() && core.role() == mt::DeviceRolePolicy::Role::CLIENT_MUTE);
  assert(core.setDeviceRole(mt::DeviceRolePolicy::Role::CLIENT,
                            mt::DeviceRolePolicy::RebroadcastMode::KNOWN_ONLY));
  auto gnss = core.gnssSettings();
  gnss.enabled = true;
  gnss.updateSecs = 43;
  gnss.attemptSecs = 97;
  gnss.broadcastSecs = 300;
  gnss.smart = false;
  gnss.smartMinDistance = 111;
  gnss.smartMinInterval = 222;
  assert(core.setGnssSettings(gnss));
  mt::ModuleSettings modules;
  meshtastic_ModuleConfig telemetry{};
  assert(modules.get(telemetry, meshtastic_ModuleConfig_telemetry_tag));
  telemetry.payload_variant.telemetry.device_telemetry_enabled = true;
  telemetry.payload_variant.telemetry.environment_measurement_enabled = true;
  telemetry.payload_variant.telemetry.environment_update_interval = 515;
  telemetry.payload_variant.telemetry.power_measurement_enabled = true;
  telemetry.payload_variant.telemetry.power_update_interval = 616;
  telemetry.payload_variant.telemetry.device_update_interval = 717;
  assert(modules.set(telemetry));
  assert(core.valid() && modules.valid());

  Projection applied{};
  applied.core = core;
  applied.modules = modules;
  auto result = Defaults::project(core.role(), Defaults::RoleTakTracker, core, modules, applied);
  assert(result == Defaults::Result::Applied);
  assert(applied.ownerUnmessagableRequired);
  assert(applied.core.ownerUnmessagable());
  assert(applied.core.role() == Defaults::RoleTakTracker);
  // Role projection must preserve the already-selected broadcast mode.
  assert(applied.core.rebroadcastMode() == core.rebroadcastMode());
  assert(applied.core.node().interval() == Defaults::NodeInfoSeconds);
  const auto beforeNode = core.node();
  const auto afterNode = applied.core.node();
  assert(beforeNode.isPublic() && afterNode.isPublic());
  assert(!memcmp(beforeNode.bytes + bench::MtNodeRecord::Key,
                 afterNode.bytes + bench::MtNodeRecord::Key, 32));
  assert(!memcmp(beforeNode.bytes + bench::MtNodeRecord::Name,
                 afterNode.bytes + bench::MtNodeRecord::Name, 25));
  assert(!memcmp(beforeNode.bytes + bench::MtNodeRecord::Short,
                 afterNode.bytes + bench::MtNodeRecord::Short, 4));
  auto actual = applied.core.gnssSettings();
  assert(actual.enabled == gnss.enabled && actual.updateSecs == gnss.updateSecs && actual.attemptSecs == gnss.attemptSecs);
  assert(actual.smart && actual.positionFlags == Defaults::PositionFlags);
  assert(actual.broadcastSecs == Defaults::PositionSeconds);
  assert(actual.smartMinDistance == Defaults::SmartDistanceMeters);
  assert(actual.smartMinInterval == Defaults::SmartMinimumIntervalSeconds);
  assert(applied.core.valid());
  meshtastic_ModuleConfig actualTelemetry{};
  assert(applied.modules.get(actualTelemetry, meshtastic_ModuleConfig_telemetry_tag));
  assert(actualTelemetry.payload_variant.telemetry.device_update_interval == Defaults::TelemetrySeconds);
  assert(actualTelemetry.payload_variant.telemetry.device_telemetry_enabled);
  assert(actualTelemetry.payload_variant.telemetry.environment_measurement_enabled);
  assert(actualTelemetry.payload_variant.telemetry.environment_update_interval == 515);
  assert(actualTelemetry.payload_variant.telemetry.power_measurement_enabled);
  assert(actualTelemetry.payload_variant.telemetry.power_update_interval == 616);

  // AdminModule assigns the requested rebroadcast field before applying the
  // NodeDB role defaults. Those defaults leave that field alone, including
  // the valid TAK_TRACKER/NONE combination from a ROUTER transition.
  auto router = core;
  assert(router.setDeviceRole(mt::DeviceRolePolicy::Role::ROUTER,
                              mt::DeviceRolePolicy::RebroadcastMode::CORE_PORTNUMS_ONLY));
  Projection routerToTak{};
  assert(Defaults::project(router.role(), Defaults::RoleTakTracker, router,
                           modules, routerToTak,
                           mt::DeviceRolePolicy::RebroadcastMode::NONE) == Defaults::Result::Applied);
  assert(routerToTak.core.role() == Defaults::RoleTakTracker);
  assert(routerToTak.core.rebroadcastMode() == mt::DeviceRolePolicy::RebroadcastMode::NONE);
  assert(routerToTak.core.node().interval() == Defaults::NodeInfoSeconds);
  assert(routerToTak.core.gnssSettings().broadcastSecs == Defaults::PositionSeconds);
  meshtastic_ModuleConfig routerTelemetry{};
  assert(routerToTak.modules.get(routerTelemetry, meshtastic_ModuleConfig_telemetry_tag));
  assert(routerTelemetry.payload_variant.telemetry.device_update_interval == Defaults::TelemetrySeconds);

  // Invalid requested modes reject transactionally, even though the source
  // configuration itself is valid and the target role is supported.
  Projection invalidMode{core, modules, false};
  const auto invalidModeBefore = invalidMode;
  assert(Defaults::project(router.role(), Defaults::RoleTakTracker, router,
                           modules, invalidMode, UINT32_MAX - 1) == Defaults::Result::UnsupportedSettings);
  unchanged(invalidMode, invalidModeBefore);

  // Reapplying the same role preserves user edits exactly and does not demand
  // a fresh owner mutation. This is the stock same-role Admin path.
  Projection same{};
  assert(Defaults::project(Defaults::RoleTakTracker, Defaults::RoleTakTracker,
                           applied.core, applied.modules, same) == Defaults::Result::SameRole);
  assert(same.core.same(applied.core) && same.modules.same(applied.modules));
  assert(!same.ownerUnmessagableRequired);
  assert(same.core.ownerUnmessagable());

  // A later move to CLIENT does not clear the owner privacy bit: stock role
  // defaults set it for TAK_TRACKER and have no inverse CLIENT default.
  auto backToClient = applied.core;
  assert(backToClient.setDeviceRole(mt::DeviceRolePolicy::Role::CLIENT, backToClient.rebroadcastMode()));
  assert(backToClient.ownerUnmessagable());

  // V19 persists owner-unmessagable. The reserved byte had to be zero in V18;
  // a CRC-correct old record with it set is malformed and cannot migrate.
  mt::Config v19Roundtrip{};
  assert(mt::Config::decode(applied.core.bytes, mt::Config::Size, v19Roundtrip));
  assert(v19Roundtrip.same(applied.core) && v19Roundtrip.ownerUnmessagable());
  auto oldV18 = core;
  oldV18.bytes[2] = 18; oldV18.bytes[5] = 0; oldV18.seal();
  mt::Config migratedV18{};
  assert(mt::Config::decode(oldV18.bytes, mt::Config::Size, migratedV18));
  assert(migratedV18.valid() && !migratedV18.ownerUnmessagable());
  auto badV18 = oldV18;
  badV18.bytes[5] = 1; badV18.seal();
  mt::Config rejected{};
  assert(!mt::Config::decode(badV18.bytes, mt::Config::Size, rejected));

  // A non-target transition is outside this helper; it must not partially
  // project either persistence domain.
  Projection untouched{core, modules, false};
  const auto before = untouched;
  assert(Defaults::project(core.role(), 1, core, modules, untouched) == Defaults::Result::NotTakTrackerTransition);
  unchanged(untouched, before);

  // Stale role binding, malformed core and malformed module snapshots all
  // fail with no output mutation.
  untouched = before;
  assert(Defaults::project(2, Defaults::RoleTakTracker, core, modules, untouched) == Defaults::Result::InvalidInput);
  unchanged(untouched, before);
  mt::Config malformed = core;
  malformed.bytes[107] = 0xff;
  untouched = before;
  assert(Defaults::project(core.role(), Defaults::RoleTakTracker, malformed, modules, untouched) == Defaults::Result::InvalidInput);
  unchanged(untouched, before);
  auto badModules = modules;
  badModules.bytes[4] ^= 1;
  untouched = before;
  assert(Defaults::project(core.role(), Defaults::RoleTakTracker, core, badModules, untouched) == Defaults::Result::InvalidInput);
  unchanged(untouched, before);
  std::puts("PASS stock TAK_TRACKER transition, preservation, role-only mode, and transactional rejection");
}
