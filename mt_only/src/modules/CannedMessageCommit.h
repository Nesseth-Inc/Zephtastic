// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "CannedMessageJournal.h"
#include "../Service.h"

namespace mt::modules {
// Service-owner only. A saved message list is local application settings; it
// never creates a packet or changes radio/identity/channel state.
struct CannedMessageCommit {
private:
  static int restore(Service &service,bool receiving,bool armed,uint32_t now){
    if(!receiving)return armed?-EIO:0;
    if(service.faulted()||!service.config.provisioned()||!service.start(false,now)){
      service.stop();return -EIO;
    }
    if(armed&&(!service.normalMode()||!service.txAllowed()||!service.config.txEnabled())){
      service.stop();return -EIO;
    }
    if(!service.armEndpoint(armed)||service.faulted()){
      service.stop();return -EIO;
    }
    return 0;
  }

public:
  static int save(Service &service,Radio &radio,CannedMessageJournal &journal,
                  SettingsBackend &backend,const CannedMessageSettings &candidate,uint32_t now){
    if(!journal.ready())return -EACCES;
    if(!candidate.valid())return -EINVAL;
    // Repeated stock set requests are inert and do not interrupt RX.
    if(candidate.same(journal.config()))return 0;
    if(service.faulted())return -EIO;
    if(!service.storageStopSafe()||radio.busy())return -EBUSY;

    const bool receiving=service.receiving(),armed=service.endpointArmed();
    const int paused=service.pauseForStorage();
    // A race-free CAD busy result keeps RX intact; hard/native faults are not
    // reported as a retryable storage busy response.
    if(paused)return paused==-EBUSY&&!service.faulted()?-EBUSY:-EIO;

    const bool safe=!service.faulted()&&service.storageQuiescent()&&radio.ready()&&radio.health()==0;
    const int saved=safe?journal.save(backend,candidate):-EIO;
    // Always settle the prior receive/arm state, even after an uncertain NVS
    // write. Persistence outcome and native restoration remain separate facts.
    const int restored=restore(service,receiving,armed,now);
    return restored||saved?-EIO:0;
  }
};
}
