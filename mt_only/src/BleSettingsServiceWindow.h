// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "BleSettingsAdmission.h"
#include <errno.h>

namespace mt::ble {
// pollLocked runs only in main with serviceLock held. No stack/Settings calls.
template<class Service,class Radio> class BleSettingsServiceWindow {
 Service &service_;Radio &radio_;bool held_=false,receiving_=false,armed_=false,cancelled_=false;
 uint32_t prepared_=0;
 int restore(uint32_t now) {
  if(!receiving_)return armed_?-EIO:0;
  if(service_.faulted()||!service_.config.provisioned()||!service_.start(false,now)){service_.stop();return -EIO;}
  if(armed_&&(!service_.normalMode()||!service_.txAllowed()||!service_.config.txEnabled())){service_.stop();return -EIO;}
  if(!service_.armEndpoint(armed_)||service_.faulted()){service_.stop();return -EIO;}
  return 0;
 }
public:
 BleSettingsServiceWindow(Service &service,Radio &radio):service_(service),radio_(radio){}
 // Main calls only after serviceLock is released. The waiting caller cannot
 // execute a Prepared token even if its semaphore timeout wakes it first.
 void grantUnlocked(){if(prepared_){cancelled_=!store::grant(prepared_);if(cancelled_)(void)store::cancel(prepared_,-ETIMEDOUT);prepared_=0;store::wake();}}
 bool pollLocked(uint64_t now) {
  if(held_) {
   if(cancelled_){const int restored=restore(uint32_t(now));held_=cancelled_=false;store::fail(restored?restored:-ETIMEDOUT);return false;}
   uint32_t token=0;int result=0;
   if(!store::completed(token,result))return true;
   const int restored=restore(uint32_t(now));held_=false;
   store::settle(token,restored);return false;
  }
  const uint32_t token=store::pendingToken();if(!token)return false;
  if(store::failed()){store::reject(token,-EIO);return false;}
  if(!service_.storageStopSafe()||radio_.busy())return false;
  receiving_=service_.receiving();armed_=service_.endpointArmed();
  const int paused=service_.pauseForStorage();
  if(paused){if(paused!=-EBUSY||service_.faulted())store::reject(token,-EIO);return false;}
  const bool safe=!service_.faulted()&&service_.storageQuiescent()&&radio_.ready()&&radio_.health()==0;
  if(safe&&store::prepare(token)){prepared_=token;held_=true;return true;}
  // Deadline/cancel may have won while native RX was being joined. No caller
  // receives a late grant; restore the RF state even though no I/O occurred.
  const int restored=restore(uint32_t(now));
  if(!safe||restored)store::reject(token,-EIO);
  return false;
 }
};
}
