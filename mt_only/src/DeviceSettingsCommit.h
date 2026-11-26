// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "SettingsCommit.h"
namespace mt {
// Reuse the existing verified multi-journal coordinator for one DeviceConfig
// replacement spanning core fields and the independent LED preference. Like
// commit_edit_settings, persistence is verified per journal, not atomic across
// journals. An original failure remains a failure and never retries here.
struct DeviceSettingsCommit {
  static int save(Service &service,SettingsEdit &core,NotificationEdit &led,
                  SettingsCommit &commit,const Config &candidate,bool disabled,
                  bool available,uint32_t now,ModuleEdit *modules=nullptr,
                  const meshtastic_ModuleConfig *moduleConfig=nullptr,
                  const ModuleSettings *projectedModules=nullptr){
    // When supplied, `commit` must be the application coordinator constructed
    // with this same ModuleEdit. Its journal order is verified but not atomic.
    if((modules!=nullptr)!=(moduleConfig!=nullptr||projectedModules!=nullptr)||
       (moduleConfig&&projectedModules))return -EINVAL;
    if(!candidate.valid())return -EINVAL;
    if(!available&&!disabled)return -ENOTSUP;
    ModuleSettings moduleCandidate{};
    bool moduleChanged=false;
    if(modules){
      if(!modules->ready())return -EACCES;
      if(projectedModules){
        if(!projectedModules->valid())return -EINVAL;
        moduleCandidate=*projectedModules;
      }else{
        moduleCandidate=modules->config();
        if(!moduleCandidate.set(*moduleConfig))return -EINVAL;
      }
      moduleChanged=!moduleCandidate.same(modules->config());
    }
    // Preserve the exact legacy fast paths for null/unchanged optional module
    // settings. The extended path below stages all changed values in the core
    // edit and delegates ordered verified persistence to SettingsCommit.
    if(!moduleChanged){
    const bool oldDisabled=!led.ready()||led.config().heartbeatDisabled();
    if(disabled==oldDisabled)return core.save(candidate,now);
    if(!led.ready())return -EACCES;
    if(candidate.same(service.config)){
      const int result=led.setHeartbeatDisabled(disabled,now,core.open());
      if(!result)core.activity(now);
      return result;
    }
    const bool alreadyOpen=core.open();
    if(!alreadyOpen){const int result=core.begin(now);if(result)return result;}
    const int saved=core.save(candidate,now);if(saved)return saved;
    const int staged=led.setHeartbeatDisabled(disabled,now,true);if(staged)return staged;
    core.activity(now);
    return alreadyOpen?0:commit.commit(now);
    }

    const bool oldDisabled=!led.ready()||led.config().heartbeatDisabled();
    if(disabled!=oldDisabled&&!led.ready())return -EACCES;
    const bool alreadyOpen=core.open();
    if(!alreadyOpen){const int result=core.begin(now);if(result)return result;}
    const int coreResult=core.save(candidate,now);
    if(coreResult)return coreResult;
    meshtastic_ModuleConfig staged{};
    const int moduleResult=projectedModules?
      modules->saveSettings(moduleCandidate,now,true):
      (staged=*moduleConfig,modules->save(staged,now,true));
    if(moduleResult)return moduleResult;
    if(disabled!=oldDisabled){
      const int ledResult=led.setHeartbeatDisabled(disabled,now,true);
      if(ledResult)return ledResult;
    }
    core.activity(now);
    return alreadyOpen?0:commit.commit(now);
  }
};
}
