// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Service.h"
#include "SettingsJournal.h"

namespace mt {
// Boot policy for normal companion profiles. Migration is durable before RF is
// started; failures therefore hold RX/TX off. Legacy one-packet profiles never
// call activate. RX-only builds start reception but cannot arm TX.
struct NormalTxStartup {
  static constexpr bool bootPolicyEnabled(bool normalEndpoint,bool privateLoadOnly) {
    return normalEndpoint&&!privateLoadOnly;
  }
  static int migrate(SettingsJournal &journal,SettingsBackend &backend,bool normalTxDefault) {
    if(!journal.ready())return -EACCES;
    const auto &loaded=journal.config();
    if(!DeviceRolePolicy::supportedRole(loaded.role()))return 0;
    if(!normalTxDefault||!loaded.provisioned()||loaded.txIntentSpecified())return 0;
    Config migrated=loaded;migrated.setTxEnabled(true);
    return journal.save(backend,migrated);
  }
  static bool activate(Service &service,bool normalEndpoint,bool normalTxDefault,uint32_t now) {
    if(!normalEndpoint||!service.config.provisioned())return true;
    // Preserve local configuration access for a known role whose optional
    // producer is absent. Do not coerce its settings or activate any RF.
    if(!DeviceRolePolicy::supportedRole(service.config.role()))
      return !service.receiving()&&!service.endpointArmed()&&!service.faulted();
    if(!service.start(false,now))return false;
    const bool enable=service.txAllowed()&&service.config.txEnabled(normalTxDefault);
    if(!service.armEndpoint(enable)){service.stop();return false;}
    return !service.faulted()&&service.receiving()&&service.endpointArmed()==enable;
  }
};
}
