// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "CompletionJournal.h"
#include "../Service.h"

namespace mt::modules {
class CompletionEdit {
  Service &service_;CompletionJournal &journal_;SettingsBackend &backend_;
  CompletionSettings current_;bool dirty_=false;
#if defined(CONFIG_MT_DISPLAY)
  bool hasNotification_=false,notificationEnabled_=false,notificationBell_=false;
#endif
  void refreshNotificationPolicy(){
#if defined(CONFIG_MT_DISPLAY)
    meshtastic_ModuleConfig wire{};
    hasNotification_=current_.get(wire,meshtastic_ModuleConfig_external_notification_tag);
    if(!hasNotification_)return;
    const auto &outputs=wire.payload_variant.external_notification;
    notificationEnabled_=outputs.enabled;
    notificationBell_=outputs.alert_bell||outputs.alert_bell_vibra||outputs.alert_bell_buzzer;
#endif
  }
public:
  CompletionEdit(Service &service,CompletionJournal &journal,SettingsBackend &backend):
    service_(service),journal_(journal),backend_(backend){}
  bool ready()const{return journal_.ready();}
  bool dirty()const{return dirty_;}
  bool trusted()const{return ready()&&!dirty_&&journal_.trusted();}
  const CompletionSettings &config()const{return current_;}
#if defined(CONFIG_MT_DISPLAY)
  // Read by the packet path under the same owner lock as load/stage. Keep the
  // generated config union off that nested receive stack.
  void textNotificationPolicy(bool &enabled,bool &bell)const{
    if(ready()&&hasNotification_){enabled=notificationEnabled_;bell=notificationBell_;}
  }
#endif
  bool load(){if(!ready())return false;current_=journal_.config();refreshNotificationPolicy();dirty_=false;return true;}
  // Owner coordinates a complete candidate under the existing begin/commit
  // lifetime. Staging does not claim durability or start optional peripherals.
  int stage(const CompletionSettings &candidate){
    if(!ready())return -EACCES;if(!candidate.valid())return -EINVAL;
    current_=candidate;refreshNotificationPolicy();dirty_=!current_.same(journal_.config());return 0;
  }
  int commit(uint32_t now){
    if(!dirty_)return 0;if(!ready())return -EACCES;
    if(!service_.storageStopSafe())return -EBUSY;
    const bool receiving=service_.receiving(),armed=service_.endpointArmed();
    const int stopped=service_.pauseForStorage();if(stopped)return stopped;
    const int saved=journal_.save(backend_,current_);if(saved)return saved;dirty_=false;
    if(receiving&&service_.config.provisioned()){
      if(!service_.start(false,now))return -EIO;
      if(armed&&!service_.armEndpoint(true))return -EIO;
    }
    return 0;
  }
};
} // namespace mt::modules
