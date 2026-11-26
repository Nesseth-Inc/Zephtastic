// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../Service.h"
#include "../SettingsJournal.h"
#include "WifiStationJournal.h"
#include <errno.h>
#include <string.h>

namespace mt { namespace network {
class WifiStationEdit {
  Service &service_;
  WifiStationJournal &journal_;
  SettingsBackend &backend_;
  WifiStationSettings current_;
  bool dirty_=false;
  static bool same(const WifiStationSettings &a,const WifiStationSettings &b) {
    return a.same(b);
  }
  int persist(const WifiStationSettings &candidate,uint32_t now) {
    const bool receiving=service_.receiving(),armed=service_.endpointArmed();
    const int paused=service_.pauseForStorage();if(paused)return paused;
    const int saved=journal_.save(backend_,candidate);if(saved)return saved;
    current_=candidate;dirty_=false;
    if(receiving&&service_.config.provisioned()) {
      if(!service_.start(false,now))return -EIO;
      if(armed&&!service_.armEndpoint(true))return -EIO;
    }
    return 0;
  }
public:
  WifiStationEdit(Service&s,WifiStationJournal&j,SettingsBackend&b):service_(s),journal_(j),backend_(b){}
  bool ready()const{return journal_.ready();}
  bool dirty()const{return dirty_;}
  const WifiStationSettings &config()const{return current_;}
  bool load(){dirty_=false;if(!ready())return false;current_=journal_.config();return true;}
  int save(const WifiStationSettings &candidate,uint32_t now,bool editing) {
    if(!ready())return -EACCES;
    if(!candidate.valid())return -EINVAL;
    if(same(candidate,current_))return 0;
    if(editing){
      current_=candidate;
      dirty_=!same(candidate,journal_.config());
      return 0;
    }
    return persist(candidate,now);
  }
  int commit(uint32_t now){if(!dirty_)return 0;if(!ready())return -EACCES;return persist(current_,now);}
};
} }
