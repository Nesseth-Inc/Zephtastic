// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "UiConfigJournal.h"
#include "../Service.h"

namespace mt::ui {
// The main Service owner holds serialization throughout this local settings
// commit. Preserve the preexisting RX and arm state; UI settings do not change
// identity, channels, trust, or RF policy.
struct UiConfigCommit {
  static int save(Service &service,Radio &radio,UiConfigJournal &journal,
                  SettingsBackend &backend,const meshtastic_DeviceUIConfig &value,uint32_t now){
    if(!journal.ready())return -EACCES;
    if(!UiConfigSettings::valid(value))return -EINVAL;
    if(journal.state()==UiConfigJournal::State::Loaded&&UiConfigSettings::same(journal.config(),value))return 0;
    return writePaused(service,radio,now,[&]{return journal.save(backend,value);});
  }
  template<class Write> static int writePaused(Service &service,Radio &radio,uint32_t now,Write write){
    if(service.faulted())return -EIO;
    if(!service.storageStopSafe()||radio.busy())return -EBUSY;
    const bool receiving=service.receiving(),relaying=service.relaying(),armed=service.endpointArmed();
    const int paused=service.pauseForStorage();
    if(paused)return paused==-EBUSY&&!service.faulted()?-EBUSY:-EIO;
    const bool safe=service.storageQuiescent()&&!service.faulted()&&radio.ready()&&radio.health()==0;
    const int stored=safe?write():-EIO;
    bool restored=true;
    if(receiving){
      restored=!service.faulted()&&service.config.provisioned()&&service.start(relaying,now);
      if(restored&&armed)restored=service.normalMode()&&service.txAllowed()&&service.config.txEnabled();
      if(restored)restored=service.armEndpoint(armed)&&!service.faulted();
    }else if(armed)restored=false;
    if(!restored){service.stop();return -EIO;}
    return stored;
  }
};
}
