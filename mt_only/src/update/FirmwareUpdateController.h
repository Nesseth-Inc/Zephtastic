// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ZephyrUpdateBackend.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>

namespace mt::update {
// One owner, serialized with application settings. Staging is RAM-only; the
// owner must stop producers, flush persistent state and prove quiescence before
// execute(). No constructor, callback, timer or destructor changes boot state.
class FirmwareUpdateController {
public:
  enum class State:uint8_t {Idle,Scheduled,Preparing,Prepared,Frozen};
private:
  State state_=State::Idle;
  Mode mode_=Mode::None;
  uint8_t hash_[32]{};
  int error_=0;
  static bool loader(Mode mode){return mode==Mode::EspBleLoader||mode==Mode::EspWifiLoader;}
  void clearHash(){volatile uint8_t *p=hash_;for(unsigned i=0;i<sizeof(hash_);++i)p[i]=0;}
public:
  FirmwareUpdateController()=default;
  FirmwareUpdateController(const FirmwareUpdateController &)=delete;
  FirmwareUpdateController &operator=(const FirmwareUpdateController &)=delete;
  ~FirmwareUpdateController(){clearHash();}
  State state()const{return state_;}
  Mode mode()const{return mode_;}
  // Frozen is a persistent reboot hold for this boot. In particular, a failed
  // ESP selector handoff must not be bypassed by an ordinary software reset.
  bool pending()const{return state_==State::Scheduled||state_==State::Preparing||
    state_==State::Prepared||state_==State::Frozen;}
  int error()const{return error_;}
  // Main may encounter a storage/owner error before entering the platform.
  // Preserve that exact terminal cause without attempting or replaying prepare.
  int freeze(int error){
    if(state_==State::Frozen)return error_;
    if(state_!=State::Scheduled&&state_!=State::Preparing)return -EACCES;
    state_=State::Frozen;clearHash();return error_=error<0?error:-EIO;
  }
  int schedule(Mode mode,const uint8_t *hash=nullptr,size_t length=0){
    if(state_!=State::Idle)return -EBUSY;
    if(mode==Mode::None||(loader(mode)?!hash||length!=32:hash||length))return -EINVAL;
    if(!available(mode))return -ENOTSUP;
    mode_=mode;if(length)memcpy(hash_,hash,length);
    state_=State::Scheduled;return 0;
  }
  // A failed/uncertain attempt is terminal for this boot. It cannot be replayed
  // by another Admin packet, an ordinary reboot timer, or a transport reconnect.
  int execute(bool quiescent,const meshtastic_Config_NetworkConfig &network){
    if(state_!=State::Scheduled)return -EACCES;
    if(!quiescent){state_=State::Frozen;clearHash();return error_=-EBUSY;}
    state_=State::Preparing;
    const int rc=prepare(mode_,loader(mode_)?hash_:nullptr,network);clearHash();
    if(rc){state_=State::Frozen;return error_=rc<0?rc:-EIO;}
    state_=State::Prepared;return error_=0;
  }
  void reboot(){
    if(state_!=State::Prepared)return;
    // Platform preparation has verified the destination. A real reset does
    // not return; a returned implementation is treated as a terminal failure.
    state_=State::Frozen;error_=-EIO;rebootPrepared(mode_);
  }
};
}
