// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ExtendedModuleJournal.h"
#include "../Service.h"

namespace mt::modules {
// Same application lock and core edit lifetime as the other verified journals.
// An explicit save can repair a recovered record even when its value is equal.
// Scheduled traffic never writes to a recovered or poisoned journal.
class ExtendedModuleEdit {
  Service &service_;ExtendedModuleJournal &journal_;SettingsBackend &backend_;
  ExtendedModuleSettings current_;bool dirty_=false;
  void updateBeaconListener(bool acceptedEdit=false){
    meshtastic_ModuleConfig_MeshBeaconConfig beacon{};
    const bool trustedConfig=ready()&&journal_.state()!=ExtendedModuleJournal::State::Recovered&&
      (acceptedEdit||!dirty_);
    const bool enabled=trustedConfig&&current_.beacon(beacon)&&
      (beacon.flags&meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_LISTEN_ENABLED);
    service_.meshBeaconListening(enabled);
  }
  int persist(const ExtendedModuleSettings &candidate,uint32_t now){
    if(!service_.storageStopSafe())return -EBUSY;
    const bool receiving=service_.receiving(),armed=service_.endpointArmed();
    const int paused=service_.pauseForStorage();if(paused)return paused;
    const int saved=journal_.save(backend_,candidate);if(saved)return saved;
    current_=candidate;dirty_=false;updateBeaconListener();
    if(receiving&&service_.config.provisioned()){
      if(!service_.start(false,now))return -EIO;
      if(armed&&!service_.armEndpoint(true))return -EIO;
    }
    return 0;
  }
public:
  ExtendedModuleEdit(Service &s,ExtendedModuleJournal &j,SettingsBackend &b):service_(s),journal_(j),backend_(b){}
  bool ready()const{return journal_.ready();}
  bool dirty()const{return dirty_;}
  bool trusted()const{return ready()&&!dirty_&&journal_.state()!=ExtendedModuleJournal::State::Recovered;}
  const ExtendedModuleSettings &config()const{return current_;}
  const ExtendedModuleSettings &committedConfig()const{return journal_.config();}
  bool load(){dirty_=false;if(!ready()){service_.meshBeaconListening(false);return false;}current_=journal_.config();updateBeaconListener();return true;}
  int save(const meshtastic_ModuleConfig &config,uint32_t now,bool editing){
    if(!ready())return -EACCES;
    auto candidate=current_;if(!candidate.set(config))return -EINVAL;
    const bool needsRepair=journal_.state()==ExtendedModuleJournal::State::Recovered;
    if(candidate.same(current_)&&!needsRepair)return 0;
    if(editing){current_=candidate;dirty_=needsRepair||!current_.same(journal_.config());updateBeaconListener(true);return 0;}
    return persist(candidate,now);
  }
  int commit(uint32_t now){if(!dirty_)return 0;if(!ready())return -EACCES;return persist(current_,now);}
  // Persist before enqueueing a scheduled broadcast. Failure consumes no ID,
  // queues no packet, and may hold radio service stopped until explicit repair.
  int markBeaconSent(uint32_t utc,uint32_t now){
    if(!trusted())return -EACCES;
    auto candidate=current_;candidate.markSent(utc);return persist(candidate,now);
  }
};
}
