// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Service.h"
#include "BluetoothJournal.h"
namespace mt {
// Service owner only, never a BLE callback/session-locked request. This journal
// is desired app configuration, not a bond database or cross-journal commit.
class BluetoothSettingsCommit {
  static int restore(Service &s,bool receiving,bool armed,uint32_t now) {
    if(!receiving)return armed?-EIO:0;
    if(s.faulted()||!s.config.provisioned()||!s.start(false,now)){
      s.stop();return -EIO;
    }
    if(armed&&(!s.normalMode()||!s.txAllowed()||!s.config.txEnabled())){
      s.stop();return -EIO;
    }
    if(!s.armEndpoint(armed)||s.faulted()){
      s.stop();return -EIO;
    }
    return 0;
  }
public:
  static int save(Service &s,Radio &radio,BluetoothJournal &journal,
                  SettingsBackend &backend,const BluetoothSettings &candidate,
                  uint32_t now,bool force=false) {
    if(!journal.ready())return -EACCES;
    if(journal.state()==BluetoothJournal::State::Recovered&&!force)return -EACCES;
    if(!BluetoothSettings::valid(candidate,true))return -EINVAL;
    if(!force&&candidate.same(journal.config()))return 0;
    if(s.faulted())return -EIO;
    if(!s.storageStopSafe()||radio.busy())return -EBUSY;
    const bool receiving=s.receiving(),armed=s.endpointArmed();
    const int paused=s.pauseForStorage();
    // pauseForStorage owns failure settlement. An EBUSY preserves live RX;
    // uncertain/native-fault outcomes must not be retried or reported saved.
    if(paused)return paused==-EBUSY&&!s.faulted()?-EBUSY:-EIO;
    const bool safe=!s.faulted()&&s.storageQuiescent()&&radio.ready()&&radio.health()==0;
    const int saved=safe?journal.save(backend,candidate,force):-EIO;
    const int restored=restore(s,receiving,armed,now);
    // Candidate validation already succeeded. Native/storage failures are an
    // unavailable interface, including journal-poisoning backend EBUSY.
    return restored||saved?-EIO:0;
  }
};
}
