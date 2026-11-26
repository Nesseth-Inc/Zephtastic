// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ModuleJournal.h"
#include "Service.h"

namespace mt {
// Shares the application's lock and the core begin/commit transaction's
// lifetime. Live visibility is immediate during a batch; verified persistence
// precedes publication outside it. Core settings and identity are untouched.
class ModuleEdit {
  Service &service_;ModuleJournal &journal_;SettingsBackend &backend_;
  ModuleSettings current_;bool dirty_=false;
  int persist(const ModuleSettings &candidate,uint32_t now){
    const bool receiving=service_.receiving(),armed=service_.endpointArmed();
    const int paused=service_.pauseForStorage();if(paused)return paused;
    const int saved=journal_.save(backend_,candidate);if(saved)return saved;
    current_=candidate;dirty_=false;
    if(receiving&&service_.config.provisioned()){
      if(!service_.start(false,now))return -EIO;
      if(armed&&!service_.armEndpoint(true))return -EIO;
    }
    return 0;
  }
public:
  ModuleEdit(Service&s,ModuleJournal&j,SettingsBackend&b):service_(s),journal_(j),backend_(b){}
  bool ready()const{return journal_.ready();}
  bool dirty()const{return dirty_;}
  const ModuleSettings &config()const{return current_;}
  bool load(){dirty_=false;if(!ready())return false;current_=journal_.config();return true;}
  int save(const meshtastic_ModuleConfig &config,uint32_t now,bool editing){
    if(!ready())return -EACCES;
    auto candidate=current_;if(!candidate.set(config))return -EINVAL;
    if(candidate.same(current_))return 0;
    if(editing){current_=candidate;dirty_=!current_.same(journal_.config());return 0;}
    return persist(candidate,now);
  }
  // Stage or persist a complete validated module projection. This is used
  // only when one stock role transition changes multiple module records; the
  // existing SettingsCommit remains responsible for ordered verified writes.
  int saveSettings(const ModuleSettings &candidate,uint32_t now,bool editing){
    if(!ready())return -EACCES;
    if(!candidate.valid())return -EINVAL;
    if(candidate.same(current_))return 0;
    if(editing){current_=candidate;dirty_=!current_.same(journal_.config());return 0;}
    return persist(candidate,now);
  }
  int commit(uint32_t now){
    if(!dirty_)return 0;
    if(!ready())return -EACCES;
    return persist(current_,now);
  }
};
}
