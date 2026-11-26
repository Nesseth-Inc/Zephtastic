// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "NotificationJournal.h"
#include "NotificationConfig.h"
#include "../Service.h"

namespace mt {
// Same verified-write/quiescence contract as ModuleEdit, independent keys.
class NotificationEdit {
  Service &service_;NotificationJournal &journal_;SettingsBackend &backend_;
  NotificationSettings current_;bool dirty_=false;
  int persist(const NotificationSettings &candidate,uint32_t now){
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
  NotificationEdit(Service&s,NotificationJournal&j,SettingsBackend&b):service_(s),journal_(j),backend_(b){}
  bool ready()const{return journal_.ready();}
  bool dirty()const{return dirty_;}
  const NotificationSettings &config()const{return current_;}
  bool load(){dirty_=false;if(!ready())return false;current_=journal_.config();return true;}
  int save(const meshtastic_ModuleConfig &config,uint32_t now,bool editing,bool available,uint32_t boardPin){
    if(!ready())return -EACCES;
    auto candidate=current_;if(!NotificationConfig::set(candidate,config,available,boardPin))return -EINVAL;
    if(candidate.same(current_))return 0;
    if(editing){current_=candidate;dirty_=!current_.same(journal_.config());return 0;}
    return persist(candidate,now);
  }
  int setHeartbeatDisabled(bool disabled,uint32_t now,bool editing){
    if(!ready())return -EACCES;
    auto candidate=current_;if(!candidate.setHeartbeatDisabled(disabled))return -EINVAL;
    if(candidate.same(current_))return 0;
    if(editing){current_=candidate;dirty_=!current_.same(journal_.config());return 0;}
    return persist(candidate,now);
  }
  int commit(uint32_t now){if(!dirty_)return 0;if(!ready())return -EACCES;return persist(current_,now);}
};
}
