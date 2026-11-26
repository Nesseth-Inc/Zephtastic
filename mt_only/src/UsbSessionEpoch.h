// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

namespace mt {
// A measured CDC connection transition delimits transport-local framing,
// output and non-idempotent request history. Unknown line-control state is not
// treated as a disconnect. The first high observation preserves boot-epoch
// bytes; a first measured-low observation establishes a closed epoch.
class UsbSessionEpoch {
  bool known_=false,connected_=false;
  uint32_t generation_=0;
public:
  enum class Transition:uint8_t {None,Connected,Disconnected};
  struct Snapshot {bool measured=false,connected=false;uint32_t generation=0;Transition transition=Transition::None;};
  // Stack-local to one accepted USB FLASH restore. Only the policy fence
  // itself can bind the result to its immediate successor generation.
  class RestoreReceipt {
    const uint32_t request_,source_;
    uint32_t destination_=0;
    bool valid_=false;
  public:
    RestoreReceipt(uint32_t request,uint32_t source):request_(request),source_(source){}
    RestoreReceipt(const RestoreReceipt &)=delete;
    uint32_t source()const{return source_;}
    bool granted()const{return valid_;}
    void bind(const Snapshot &before,uint32_t after){
      valid_=request_&&before.generation==source_&&(!before.measured||before.connected)&&
        after==uint32_t(source_+1);
      destination_=after;
    }
    bool take(uint32_t request,const Snapshot &line){
      const bool allowed=valid_&&request==request_&&line.generation==destination_&&(!line.measured||line.connected);
      valid_=false;return allowed;
    }
  };
private:
  Transition observe(bool measured,bool connected) {
    if(!measured)return Transition::None;
    if(!known_){known_=true;connected_=connected;if(!connected){++generation_;return Transition::Disconnected;}return Transition::None;}
    if(connected_==connected)return Transition::None;
    connected_=connected;++generation_;
    return connected?Transition::Connected:Transition::Disconnected;
  }
public:
  template<class Down,class Up,class Low> Snapshot bind(bool measured,bool connected,Down down,Up up,Low low) {
    const auto transition=observe(measured,connected);
    if(transition==Transition::Disconnected)down();
    else if(transition==Transition::Connected)up();
    if(measured&&!connected)low();
    return {measured,measured?connected:connected_,generation_,transition};
  }
  bool known()const{return known_;}
  bool connected()const{return connected_;}
  uint32_t generation()const{return generation_;}
  // Owner must hold the same IRQ exclusion as bind(). A policy transition
  // invalidates accepted frames without inventing a physical DTR change.
  void fence(){++generation_;}
};
}
