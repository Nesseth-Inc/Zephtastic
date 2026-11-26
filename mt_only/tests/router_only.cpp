// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
#include "NormalTxStartup.h"
#include "modules/RouterInitialSettings.h"

int main() {
  using Role = meshtastic_Config_DeviceConfig_Role;
  constexpr auto router = meshtastic_Config_DeviceConfig_Role_ROUTER;
  constexpr auto late = meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
  Fixture f(127, false);
  mt::ModuleJournal modules;
  mt::modules::CompletionJournal completion;
  assert(modules.load(f.backend) == 0 && completion.load(f.backend) == 0);
  assert(mt::modules::seedRouterDefaults(f.journal, modules, completion));
  assert(f.service.apply(f.journal.config()) && !f.backend.writes);
  const auto &config = f.service.config;
  meshtastic_ModuleConfig telemetry{};
  meshtastic_Config_PowerConfig power{};
  assert(config.ownerUnmessagable() && config.gnssSettings().broadcastSecs == 43200);
  assert(modules.config().get(telemetry, meshtastic_ModuleConfig_telemetry_tag));
  assert(telemetry.payload_variant.telemetry.device_update_interval == 43200);
  assert(completion.config().power(power) && power.ls_secs == 86400 && power.wait_bluetooth_secs == 1);
  assert(config.role() == router && config.rebroadcastMode() == mt::DeviceRolePolicy::RebroadcastMode::CORE_PORTNUMS_ONLY);
  assert(!config.provisioned() && mt::NormalTxStartup::activate(f.service, true, true, 0));
  assert(!f.service.receiving() && !f.service.endpointArmed() && !f.radio.starts);
  std::cout << "PASS fresh router exposes local configuration with region unset and RF silent\n";

  mt::Config old = config;
  old.bytes[4] = 1;
  old.setOwnerUnmessagable(false);
  assert(old.setDisplay(77, false));
  old.bytes[107] = mt::DeviceRolePolicy::Role::CLIENT;
  old.bytes[108] = mt::DeviceRolePolicy::RebroadcastMode::ALL;
  old.seal();
  assert(old.valid() && f.journal.save(f.backend, old) == 0);
  mt::SettingsJournal reboot;
  assert(reboot.load(f.backend) == 0 && reboot.config().same(old));
  auto customized = modules.config();
  telemetry.payload_variant.telemetry.device_update_interval = 731;
  assert(customized.set(telemetry) && modules.save(f.backend, customized) == 0);
  auto customPower = completion.config();
  power.wait_bluetooth_secs = 42;
  assert(customPower.setPower(power) && completion.save(f.backend, customPower) == 0);
  assert(mt::modules::seedRouterDefaults(reboot, modules, completion));
  assert(reboot.config().same(old) && modules.config().same(customized) && completion.config().same(customPower));
  auto writes = f.backend.writes;
  assert(!mt::NormalTxStartup::migrate(reboot, f.backend, true) && f.backend.writes == writes);
  assert(f.service.apply(reboot.config()) && mt::NormalTxStartup::activate(f.service, true, true, 1));
  assert(!f.radio.starts && !f.service.receiving() && !f.service.endpointArmed());
  std::cout << "PASS stored companion role remains readable and unchanged with RX and TX held\n";

  auto change = device();
  for (auto role : {router, late}) {
    change.set_config.payload_variant.device.role = role;
    f.admin(change);
    assert(!f.usb.error() && config.role() == unsigned(role));
  }
  for (auto role : {0, 1, 3, 4, 5, 8, 12}) {
    change.set_config.payload_variant.device.role = static_cast<Role>(role);
    f.admin(change);
    assert(f.usb.error() != 0 && config.role() == late);
  }
  change.set_config.payload_variant.device.role = router;
  f.admin(change);
  assert(!f.usb.error());
  text(f, 151, 50);
  assert(f.usb.error() == meshtastic_Routing_Error_NO_INTERFACE && !f.radio.sends);
  std::cout << "PASS local USB role edits admit router roles and refuse client RF origination\n";

  const uint32_t sender = 0x24681357, destination = 0x97531246;
  uint8_t frame[255]{};
  const uint8_t textData[] = {8, 1, 18, 3, 'b', 'a', 't', 72, 0};
  size_t size = 0;
  assert(f.service.codec.encodeZeroHopData(sender, destination, 301, textData, sizeof(textData), frame, sizeof(frame), size));
  frame[12] = (2u << 5) | 2u;
  f.service.stop();
  mt::Config disabled = config;
  disabled.setTxEnabled(false);
  assert(f.service.apply(disabled) && mt::NormalTxStartup::activate(f.service, true, true, 100));
  assert(f.service.receiving() && !f.service.endpointArmed());
  f.service.rx(frame, size, 3, 1000, 0);
  f.service.poll(10000, 0);
  assert(!f.radio.sends && !f.service.route.pending());
  f.service.stop();
  disabled.setTxEnabled(true);
  assert(f.service.apply(disabled) && mt::NormalTxStartup::activate(f.service, true, true, 20000));
  f.service.rx(frame, size, 3, 21000, 0);
  assert(f.service.route.pending() == 1);
  f.service.poll(30000, 0);
  assert(f.radio.sends == 1 && f.service.active());
  f.radio.done = true;
  f.service.poll(30100, 0);
  assert(f.service.sent == 1 && !f.service.active() && !f.service.route.pending());
  std::cout << "PASS autonomous RF relay honors TX disable and completes one eligible transmission\n";
}
