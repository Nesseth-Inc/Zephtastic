// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../NodeAdmin.h"
#include <errno.h>

namespace mt::admin {

// Applies the two remotely authorized local-node policy edits. The shared
// NodeAdmin barrier owns the RF-safe journal transaction and exact RX/arm/relay
// restoration; this wrapper retains only the remote-specific authorization.
struct RemoteFavoriteCommit {
public:
  static int edit(Service &service, Radio &radio, NodeJournal &journal,
                  SettingsBackend &settings, uint32_t id, bool favorite,
                  uint32_t now, bool blocked = false, NodeJournal::BeforeEdit before = nullptr) {
    if (!journal.ready() || !service.nodes.ready()) return -EACCES;
    if (id <= 1 || id >= 0xfffffffeu || id == service.route.identity())
      return -EINVAL;
    if (blocked || service.faulted()) return service.faulted() ? -EIO : -EBUSY;

    // A no-op must not interrupt RX, clear accepted correlations, or write a
    // redundant journal generation.
    const int slot = service.nodes.find(id);
    if (slot < 0) return 0;  // Matches stock favorite behavior for unknown IDs.
    const auto *entry = service.nodes.get(unsigned(slot));
    if (!entry) return -EIO;
    if (entry->favorite() == favorite) return 0;

    if (!service.normalMode() || !service.txAllowed() ||
        !service.config.txEnabled() || !service.receiving() ||
        !service.endpointArmed()) return -EACCES;
    return NodeAdmin::edit(service,radio,journal,settings,id,
      favorite?NodeCache::Edit::Favorite:NodeCache::Edit::Unfavorite,now,false,nullptr,nullptr,before);
  }
};

}  // namespace mt::admin
