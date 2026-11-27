// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "BeaconExcursion.h"
#include "../Config.h"
#include "MtNodeInfo.h"
#include <stddef.h>
#include <stdint.h>

namespace mt {
class Service;
namespace modules {

// Owner-loop bridge between stock MeshBeacon target selection and the native
// Service radio. Queue records own their encoded frame until TX completion.
class BeaconTargetRuntime {
public:
  using Draw=uint32_t (*)();
  static uint32_t defaultDraw();
  explicit BeaconTargetRuntime(Service &service,Draw draw=defaultDraw):service_(service),draw_(draw){}
  bool enqueue(const BeaconExcursion::Request &request,const uint8_t *data,size_t size,
               uint32_t random,uint32_t now,uint32_t *acceptedId=nullptr);
  bool targetSupported(const BeaconExcursion::RadioProfile &profile);
  void poll(uint32_t now,uint32_t settingsEpoch,uint32_t identityEpoch,bool mainReady);
  void receiveAccepted();
  void cancelAll(uint32_t settingsEpoch,uint32_t identityEpoch);
  bool busy()const{return radioHeld()||excursion_.queued()!=0;}
  bool radioHeld()const{return excursion_.holdsRadio()||txActive_||retuned_;}
  size_t queued()const{return excursion_.queued();}
  int lastError()const{return lastError_;}

  static BeaconExcursion::RadioProfile runningProfile(const Config &config);

private:
  static constexpr size_t Rows=BeaconExcursion::QueueCapacity;
  struct Row {
    uint32_t id=0;
    uint32_t enqueuedAt=0;
    BeaconExcursion::Request request{};
    bench::Frame frame{};
    bool used=false;
  };
  Service &service_;
  Draw draw_;
  BeaconExcursion excursion_{};
  Row rows_[Rows]{};
  bool txActive_=false,retuned_=false,homeArmed_=false,homeRelaying_=false;
  bool completionAwaitingRestore_=false,pendingTxSuccess_=false;
  bool waitingForAdmission_=false;
  uint32_t deadline_=0,activeId_=0;
  uint32_t activeSince_=0,slotMs_=0;
  bool activeSinceSet_=false;
  int lastError_=0;
  BeaconExcursion::Action pendingAction_{};
  bool pendingActionReady_=false;

  Row *row(uint32_t id);
  bool encode(const BeaconExcursion::Request &request,const uint8_t *data,size_t size,
              uint32_t random,uint32_t now,bench::Frame &out);
  bool configureProfile(const BeaconExcursion::RadioProfile &profile,Config &candidate,uint32_t *slotMs=nullptr);
  void contend(const BeaconExcursion::Action &action,uint32_t now);
  bool retune(const BeaconExcursion::RadioProfile &profile);
  bool restoreHome();
  void apply(const BeaconExcursion::Action &action,uint32_t now,
             uint32_t settingsEpoch,uint32_t identityEpoch);
  void release(uint32_t id);
  bool admission(const Config &radioConfig,bool polite);
  void expireQueued(uint32_t now,uint32_t settingsEpoch,uint32_t identityEpoch);
};
} // namespace modules
} // namespace mt
