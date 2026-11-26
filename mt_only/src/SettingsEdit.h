// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Service.h"
#include "SettingsJournal.h"

namespace mt {
// Stock begin/commit batches persistence, not visibility or rollback. One
// instance is shared by all local transports under the application's lock.
class SettingsEdit {
  Service &service_;
  SettingsJournal &journal_;
  SettingsBackend &backend_;
  bool open_=false,dirty_=false;
  uint32_t activity_=0;
  int restoreDurableIntent() {
    const auto &c=service_.config;
    // The quota-limited one-packet profile deliberately never restores an arm:
    // durable normal-use intent belongs only to the normal endpoint.
    if(!service_.normalMode()||!service_.receiving()||!c.provisioned()||!c.txEnabled()||!service_.txAllowed())return 0;
    return service_.armEndpoint(true)?0:-EIO;
  }
  int apply(const Config &candidate,uint32_t now,bool persist,bool restoreIntent) {
    if(!journal_.ready()||!candidate.valid())return -EACCES;
    // Copy before apply: commit passes service.config itself.
    const Config c=candidate;
    // Classify against the last verified durable record, not the current live
    // Config: an open batch may already expose one or more position edits.
    // Any non-position change in the same transaction keeps the full apply.
    const bool positionOnly=journal_.config().valid()&&service_.config.valid()&&
      journal_.config().sameExceptPosition(c)&&service_.config.sameExceptPosition(c);
    const bool channelMuteOnly=journal_.config().valid()&&service_.config.valid()&&
      journal_.config().sameExceptChannelMute(c)&&service_.config.sameExceptChannelMute(c);
#if defined(CONFIG_MT_LOCAL_CLOCK)
    const bool clockOnly=journal_.config().sameExceptClock(c)&&service_.config.sameExceptClock(c);
#endif
    const bool receiving=service_.receiving();
    const int paused=service_.pauseForStorage();
    if(paused)return paused;
    if(persist){const int ret=journal_.save(backend_,c);if(ret)return ret;}
    if(!(
#if defined(CONFIG_MT_LOCAL_CLOCK)
         clockOnly?service_.applyLocalClock(c):
#endif
         positionOnly?service_.applyPosition(c):
         channelMuteOnly?service_.applyChannelMute(c):service_.apply(c)))return -EINVAL;
    if(open_&&!persist){dirty_=true;activity_=now;}
    if(receiving&&c.provisioned()) {
      if(!service_.start(false,now))return -EIO;
      // A settings save is not an implicit user TX-disable. Restore only a
      // durable explicit enable; RX-only builds still reject the arm safely.
      if(restoreIntent&&restoreDurableIntent())return -EIO;
    }
    return 0;
  }
public:
  static constexpr uint32_t IdleMs=60000;
  SettingsEdit(Service &s,SettingsJournal &j,SettingsBackend &b):service_(s),journal_(j),backend_(b){}
  bool open()const{return open_;}
  bool dirty()const{return dirty_;}
  bool due(uint32_t now)const{return open_&&uint32_t(now-activity_)>=IdleMs;}
  void activity(uint32_t now){if(open_)activity_=now;}

  int begin(uint32_t now) {
    if(!journal_.ready())return -EACCES;
    // Establish a native RX barrier as well as checking both endpoint queues.
    // This prevents a completed event racing the edit's deliberate TX disarm.
    const bool receiving=service_.receiving();
    const int paused=service_.pauseForStorage();
    if(paused)return paused;
    if(receiving&&!service_.start(false,now))return -EIO;
    open_=true;activity_=now;return 0;
  }
  int save(const Config &c,uint32_t now) {
    if(!journal_.ready()||!c.valid())return -EACCES;
    // Accepted unchanged setters are inert; inside a batch they still refresh
    // transaction activity, as in stock.
    if(c.same(service_.config)){if(open_)activity_=now;return 0;}
    return apply(c,now,!open_,!open_);
  }
  // Remote core-Config writes become durable before their response is queued,
  // while the radio continues using the existing live Config. This is not a
  // live apply and does not fold role defaults or other journals into the
  // same transaction.
  int persistForRemoteReply(const Config &candidate,uint32_t now,
                            bool &durable,uint32_t &generation,
                            int (*beforePersist)(void *)=nullptr,void *context=nullptr) {
    durable=false;generation=0;
    if(!journal_.ready()||!candidate.valid())return -EACCES;
    if(open_)return -EBUSY;
    if(!service_.normalMode())return -ENOTSUP;
    if(!service_.storageStopSafe())return -EBUSY;
    const bool wasReceiving=service_.receiving();
    const bool wasRelaying=service_.relaying();
    const bool wasArmed=service_.endpointArmed();
    if(!wasReceiving&&(wasRelaying||wasArmed))return -EIO;

    const int paused=service_.pauseForStorage();
    if(paused)return paused;
    // An optional typed multi-journal intent owner runs under this same
    // verified pause. Any partial/uncertain result leaves RF stopped.
    if(beforePersist){const int prepared=beforePersist(context);if(prepared)return prepared;}
    int persistence=journal_.save(backend_,candidate);
    if(persistence)return persistence; // uncertain storage state stays RF-off
    durable=true;generation=journal_.generation();

    // Only a verified durable commit permits restoring the unchanged live
    // Config and previous runtime intent. A failed resume remains a held
    // recovery case even though durable is true.
    int restoration=0;
    if(wasReceiving) {
      if(!service_.start(wasRelaying,now))restoration=-EIO;
      else if(wasArmed&&!service_.armEndpoint(true))restoration=-EIO;
      if(!restoration&&(service_.receiving()!=wasReceiving||
         service_.relaying()!=wasRelaying||service_.endpointArmed()!=wasArmed))
        restoration=-EIO;
    }
    return restoration;
  }
  int commit(uint32_t now) {
    if(!open_)return 0;
    if(dirty_){
      const int ret=apply(service_.config,now,true,true);
      if(ret){activity_=now;return ret;}
    }else {
      // begin() quiesces TX even when no setting changes. Closing an empty or
      // unchanged batch must not turn that temporary state into an implicit
      // disable when the verified record still says explicitly enabled.
      const int ret=restoreDurableIntent();
      if(ret){activity_=now;return ret;}
    }
    open_=dirty_=false;return 0;
  }
  // The caller proves native state agrees with the Service. A clean batch can
  // close in healthy RX and restore durable intent without a storage or radio
  // transition. Dirty persistence retains the strict fully-off quiescence gate.
  int poll(uint32_t now,bool coherent) {
    if(!open_||uint32_t(now-activity_)<IdleMs||!coherent||!journal_.ready())return 0;
    if(dirty_&&!service_.storageQuiescent())return 0;
    return commit(now);
  }
};
}
