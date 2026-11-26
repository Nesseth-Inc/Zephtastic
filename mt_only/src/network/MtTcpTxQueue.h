// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
namespace mt { namespace tcp {
template<size_t Depth> class FixedTxQueue {
public:
  static_assert(Depth>=4&&Depth<=64,"bounded companion frame capacity");
  static constexpr size_t Capacity=Depth;
  struct Frame { uint16_t length=0; uint8_t bytes[516]{}; };
  bool pushFramed(const uint8_t *framed,size_t size,bool idleOnly=false) {
    if(idleOnly && count_)return false;
    // Canonical MT record: 0x94 0xc3 sync, BE16 body length, 1..512-byte
    // protobuf body, 5..516 total. Invalid input returns false and leaves
    // the queue, flags, and contents unchanged. When full: reject, set
    // congested_, preserve every queued frame. No blocking/eviction/retry.
    if(!framed||size<5||size>516)return false; // size<5 rejects zero body
    if(framed[0]!=0x94||framed[1]!=0xc3)return false;
    const size_t body=size-4;
    if((size_t(framed[2])<<8|framed[3])!=body)return false;
    if(count_>=Capacity){congested_=true;return false;}
    Frame &slot=slots_[tail_];
    slot.length=uint16_t(size); // total framed record size, for host send
    memcpy(slot.bytes,framed,size);
    tail_=(tail_+1)%Capacity;
    ++count_;
    return true;
  }
  bool pop(Frame &out) {
    if(!count_)return false;
    Frame next=slots_[head_];slots_[head_]=Frame{};
    head_=(head_+1)%Capacity;--count_;
    if(count_<=Capacity/3)congested_=false;
    out=next;return true;
  }
  size_t size()const{return count_;}
  bool busy()const{return congested_||count_>=Capacity*2/3;}
  void reset(){for(auto &slot:slots_)slot=Frame{};head_=tail_=count_=0;congested_=false;}
private:
  Frame slots_[Capacity]{};size_t head_=0,tail_=0,count_=0;bool congested_=false;
};
using TxQueue=FixedTxQueue<4>;
} }
