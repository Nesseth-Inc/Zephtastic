// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "AdminKeyJournal.h"
#include "../Service.h"
#include <errno.h>

namespace mt::admin {

// Service-owner only. Admin-key edits are durable local policy; this helper
// never changes the live SecurityConfig or transfers service ownership.
struct AdminKeyCommit {
private:
  static int restore(Service &service, bool receiving, bool armed, uint32_t now) {
    if (!receiving) return armed ? -EIO : 0;
    if (service.faulted() || !service.config.provisioned() || !service.start(false, now)) {
      service.stop();
      return -EIO;
    }
    if (armed && (!service.normalMode() || !service.txAllowed() || !service.config.txEnabled())) {
      service.stop();
      return -EIO;
    }
    if (!service.armEndpoint(armed) || service.faulted()) {
      service.stop();
      return -EIO;
    }
    return 0;
  }

public:
  static int save(Service &service, Radio &radio, AdminKeyJournal &journal,
                  SettingsBackend &backend, const AdminKeySettings &candidate,
                  uint32_t now) {
    if (!journal.ready()) return -EACCES;
    if (!candidate.valid()) return -EINVAL;
    // An identical allowlist is inert and must not interrupt active RX.
    if (candidate.same(journal.settings())) return 0;
    if (service.faulted()) return -EIO;
    if (!service.storageStopSafe() || radio.busy()) return -EBUSY;

    const bool receiving = service.receiving();
    const bool armed = service.endpointArmed();
    const int paused = service.pauseForStorage();
    // pauseForStorage leaves clean CAD races receiving and faults hard native
    // failures itself; do not retry or write after either refusal.
    if (paused) return paused == -EBUSY && !service.faulted() ? -EBUSY : -EIO;

    const bool safe = !service.faulted() && service.storageQuiescent() &&
                      radio.ready() && radio.health() == 0;
    const int saved = safe ? journal.save(backend, candidate) : -EIO;

    // Settle the exact prior RX/armed intent after both verified persistence
    // and uncertain storage failure. A restoration failure remains visible.
    const int restored = restore(service, receiving, armed, now);
    return restored || saved ? -EIO : 0;
  }
};

}  // namespace mt::admin
