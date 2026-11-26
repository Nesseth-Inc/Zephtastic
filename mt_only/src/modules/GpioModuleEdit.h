// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <errno.h>
#include "GpioModuleJournal.h"
#include "../Service.h"

namespace mt::modules {

// Transactional GPIO module settings edit. Board capability admission remains
// the caller's responsibility and must pass before either module is enabled.
class GpioModuleEdit {
  Service &service_;
  GpioModuleJournal &journal_;
  SettingsBackend &backend_;
  GpioModuleSettings current_{};
  bool dirty_ = false;

  int persist(const GpioModuleSettings &candidate, uint32_t now) {
    if (!service_.storageStopSafe()) return -EBUSY;
    const bool receiving = service_.receiving();
    const bool armed = service_.endpointArmed();
    const int paused = service_.pauseForStorage();
    if (paused) return paused;
    const int saved = journal_.save(backend_, candidate);
    if (saved) return saved;
    current_ = candidate;
    dirty_ = false;
    if (receiving && service_.config.provisioned()) {
      if (!service_.start(false, now)) return -EIO;
      if (armed && !service_.armEndpoint(true)) return -EIO;
    }
    return 0;
  }

public:
  GpioModuleEdit(Service &service, GpioModuleJournal &journal, SettingsBackend &backend)
      : service_(service), journal_(journal), backend_(backend) {}

  bool ready() const { return journal_.ready(); }
  bool dirty() const { return dirty_; }
  bool trusted() const {
    return ready() && !dirty_ && journal_.state() != GpioModuleJournal::State::Recovered;
  }
  const GpioModuleSettings &config() const { return current_; }

  bool load() {
    dirty_ = false;
    if (!ready()) return false;
    current_ = journal_.config();
    return true;
  }

  int save(const meshtastic_ModuleConfig &config, uint32_t now, bool editing) {
    if (!ready()) return -EACCES;
    if (config.which_payload_variant != GpioModuleSettings::RemoteTag &&
        config.which_payload_variant != GpioModuleSettings::DetectionTag) return -EINVAL;
    GpioModuleSettings candidate = current_;
    if (!candidate.set(config)) return -EINVAL;
    const bool repair = journal_.state() == GpioModuleJournal::State::Recovered;
    if (candidate.same(current_) && !repair) return 0;
    if (editing) {
      current_ = candidate;
      dirty_ = repair || !current_.same(journal_.config());
      return 0;
    }
    return persist(candidate, now);
  }

  int commit(uint32_t now) {
    if (!dirty_) return 0;
    if (!ready()) return -EACCES;
    return persist(current_, now);
  }
};

}  // namespace mt::modules
