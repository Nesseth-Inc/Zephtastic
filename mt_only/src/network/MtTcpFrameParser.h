// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace mt { namespace tcp {
struct Frame { uint16_t length=0; uint8_t payload[512]{}; };
class FrameParser {
public:
  enum class Result:uint8_t { NeedMore, FrameReady, Invalid };
  Result push(uint8_t byte,uint32_t now,uint32_t session,Frame &out) {
    if(session_!=session) reset(session);
    if(state_ && uint32_t(now-lastByte_)>2000) reset(session);
    lastByte_=now;
    switch(state_) {
    case 0:
      if(byte==0x94) state_=1;
      break;
    case 1:
      state_=(byte==0xc3)?2:(byte==0x94?1:0);
      break;
    case 2:
      length_=uint16_t(uint16_t(byte)<<8);
      state_=3;
      break;
    case 3:
      length_=uint16_t(length_|byte);
      used_=0;
      if(!length_||length_>sizeof(payload_)) {
        clearPartial();
        return Result::Invalid;
      }
      state_=4;
      break;
    case 4:
      payload_[used_++]=byte;
      if(used_==length_) {
        Frame completed{};
        completed.length=length_;
        memcpy(completed.payload,payload_,length_);
        out=completed;
        clearPartial();
        return Result::FrameReady;
      }
      break;
    default:
      clearPartial();
      return Result::Invalid;
    }
    return Result::NeedMore;
  }
  void reset(uint32_t session=0) {
    clearPartial(); session_=session; lastByte_=0;
  }
private:
  void clearPartial() {
    state_=0; length_=used_=0; memset(payload_,0,sizeof(payload_));
  }
  uint8_t payload_[512]{};
  uint16_t length_=0,used_=0;
  uint32_t session_=0,lastByte_=0;
  uint8_t state_=0;
};
} }
