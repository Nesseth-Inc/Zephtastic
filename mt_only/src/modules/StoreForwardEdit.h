// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "StoreForwardJournal.h"
#include "../Service.h"

namespace mt::modules {
class StoreForwardEdit {
  Service &service_;StoreForwardJournal &journal_;SettingsBackend &backend_;
  StoreForwardSettings current_;bool dirty_=false;
  int persist(const StoreForwardSettings &candidate,uint32_t now){
    if(!service_.storageStopSafe())return -EBUSY;
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
  StoreForwardEdit(Service &s,StoreForwardJournal &j,SettingsBackend &b):service_(s),journal_(j),backend_(b){}
  bool ready()const{return journal_.ready();}
  bool dirty()const{return dirty_;}
  bool trusted()const{return ready()&&!dirty_&&journal_.state()!=StoreForwardJournal::State::Recovered;}
  const StoreForwardSettings &config()const{return current_;}
  bool load(){dirty_=false;if(!ready())return false;current_=journal_.config();return true;}
  int save(const meshtastic_ModuleConfig &config,uint32_t now,bool editing){
    if(!ready())return -EACCES;
    if(config.which_payload_variant!=meshtastic_ModuleConfig_store_forward_tag)return -EINVAL;
    auto candidate=current_;if(!candidate.set(config.payload_variant.store_forward))return -EINVAL;
    const bool repair=journal_.state()==StoreForwardJournal::State::Recovered;
    if(candidate.same(current_)&&!repair)return 0;
    if(editing){current_=candidate;dirty_=repair||!current_.same(journal_.config());return 0;}
    return persist(candidate,now);
  }
  int commit(uint32_t now){if(!dirty_)return 0;if(!ready())return -EACCES;return persist(current_,now);}
};
}
