// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "RingtoneJournal.h"
#include "../Service.h"

namespace mt::notification {
// Service-owner only. Ringtone persistence uses its own two-slot namespace
// and never touches legacy settings or module configuration storage.
struct RingtoneCommit {
private:
  static int restore(Service &service,bool receiving,bool armed,uint32_t now){
    if(!receiving)return armed?-EIO:0;
    if(service.faulted()||!service.config.provisioned()||!service.start(false,now)){service.stop();return -EIO;}
    if(armed&&(!service.normalMode()||!service.txAllowed()||!service.config.txEnabled())){service.stop();return -EIO;}
    if(!service.armEndpoint(armed)||service.faulted()){service.stop();return -EIO;}return 0;
  }
public:
  static int save(Service &service,Radio &radio,RingtoneJournal &journal,SettingsBackend &backend,const RingtoneSettings &candidate,uint32_t now){
    if(!journal.ready())return -EACCES;if(!candidate.valid())return -EINVAL;if(candidate.same(journal.config()))return 0;
    if(service.faulted())return -EIO;if(!service.storageStopSafe()||radio.busy())return -EBUSY;
    const bool receiving=service.receiving(),armed=service.endpointArmed();const int paused=service.pauseForStorage();
    if(paused)return paused==-EBUSY&&!service.faulted()?-EBUSY:-EIO;
    const bool safe=!service.faulted()&&service.storageQuiescent()&&radio.ready()&&radio.health()==0;
    const int saved=safe?journal.save(backend,candidate):-EIO;const int restored=restore(service,receiving,armed,now);
    return restored||saved?-EIO:0;
  }
};
}
