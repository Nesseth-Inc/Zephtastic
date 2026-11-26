// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../Config.h"

namespace mt {
// Independent explicit bytes, with disabled zero defaults. GPIO capability
// admission belongs to the platform adapter; this record only bounds the ID.
//
// Inner record versions (magic at bytes0..3):
//   v1 "ZTN1": bytes4 bits0..3 are the notification flags; the upper nibble
//   (bits4..7) must be zero. A v1 record implies heartbeat disabled.
//   v2 "ZTN2": bytes4 bits0..3 are the notification flags, bit4 is
//   heartbeatDisabled (default true), and bits5..7 must be zero.
// bytes5..13 and the CRC16/CCITT seal at bytes14..15 are unchanged across
// versions. The 32-byte journal wrapper is not touched by this record.
//
// Mutation contract: both set(...) and setHeartbeatDisabled(...) first
// require valid() on the current record. An invalid record (bad magic,
// out-of-bounds field, or broken seal) is never repaired, canonicalized, or
// migrated by a mutator: both return false and preserve every byte exactly.
struct NotificationSettings {
  static constexpr size_t Size=16;
  static constexpr uint32_t MaxOutput=63,MaxOutputMs=0x7fffffffu;
  // The slot stays uint32 LE, but the current generated wire field is uint16.
  static constexpr uint32_t MaxNagTimeoutSeconds=UINT16_MAX;
  // bytes4 bit4: heartbeat disabled (set = disabled). Default true.
  static constexpr uint8_t HeartbeatBit=0x10;
  uint8_t bytes[Size]{};
  // Default record is v2 with heartbeat disabled and no notification flags.
  NotificationSettings(){bytes[0]='Z';bytes[1]='T';bytes[2]='N';bytes[3]='2';bytes[4]=HeartbeatBit;seal();}
  void seal(){const auto c=Config::checksum(bytes,Size-2);bytes[14]=uint8_t(c);bytes[15]=uint8_t(c>>8);}
  bool valid()const{
    const bool v1=isV1(),v2=isV2();
    if(!v1&&!v2)return false;
    if(v1){if(bytes[4]&0xf0)return false;}
    else{if(bytes[4]&0xe0)return false;}
    if(bytes[5]>MaxOutput||Config::get32(bytes+6)>MaxOutputMs||Config::get32(bytes+10)>MaxNagTimeoutSeconds)return false;
    const auto c=Config::checksum(bytes,Size-2);return bytes[14]==uint8_t(c)&&bytes[15]==uint8_t(c>>8);
  }
  bool same(const NotificationSettings &other)const{return !memcmp(bytes,other.bytes,Size);}
  bool enabled()const{return bytes[4]&1;}
  bool alertMessage()const{return bytes[4]&2;}
  bool alertBell()const{return bytes[4]&4;}
  bool active()const{return bytes[4]&8;}
  // v1 implies disabled; a valid v2 record reports bit4. For an invalid
  // record (bad magic, out-of-bounds field, or broken seal) this
  // deliberately reports the safe default true (heartbeat disabled) so a
  // corrupt record is never read as an enabled heartbeat. This documented
  // read-only default is NOT permission for a mutator to repair or
  // canonicalize an invalid record: both mutators below refuse invalid
  // input and preserve every byte.
  bool heartbeatDisabled()const{return valid()&&isV2()?((bytes[4]&HeartbeatBit)!=0):true;}
  uint8_t output()const{return bytes[5];}
  uint32_t outputMs()const{return Config::get32(bytes+6);}
  uint32_t nagTimeoutSeconds()const{return Config::get32(bytes+10);}
  // Requires valid() on the current record. Invalid records are never
  // repaired or migrated: returns false and preserves every byte exactly.
  // On success, copy-on-success: build a candidate, reseal, validate, then
  // assign. A valid v2 preserves the current heartbeat-disabled preference
  // (bit4 carried through); a valid v1 migrates to v2 with heartbeat
  // disabled (v1's implied preference).
  bool set(bool enable,bool message,bool bell,bool outputActive,uint32_t pin,uint32_t milliseconds,uint32_t nagSeconds){
    if(!valid())return false;
    if(pin>MaxOutput||milliseconds>MaxOutputMs||nagSeconds>MaxNagTimeoutSeconds)return false;
    const uint8_t flags=uint8_t((enable?1:0)|(message?2:0)|(bell?4:0)|(outputActive?8:0));
    NotificationSettings candidate=*this;
    if(isV2()){
      // Preserve the current heartbeat-disabled preference; rebuild the flags.
      candidate.bytes[4]=uint8_t(flags|((bytes[4]&HeartbeatBit)?HeartbeatBit:0));
    }else{
      // Valid v1 migrates to v2 carrying v1's implied disabled=true.
      candidate.bytes[3]='2';
      candidate.bytes[4]=uint8_t(flags|HeartbeatBit);
    }
    candidate.bytes[5]=uint8_t(pin);
    Config::put32(candidate.bytes+6,milliseconds);Config::put32(candidate.bytes+10,nagSeconds);
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  // Requires valid() on the current record; an invalid record (bad magic,
  // out-of-bounds field, or broken seal) is left unchanged — no repair or
  // canonicalization. Promotes a valid v1 to v2, preserves bits0..3 and
  // notification bytes5..13, and reseals + validates the candidate before
  // assigning.
  bool setHeartbeatDisabled(bool disabled){
    if(!valid())return false;
    NotificationSettings candidate=*this;
    candidate.bytes[3]='2';
    candidate.bytes[4]=uint8_t((candidate.bytes[4]&0x0f)|(disabled?HeartbeatBit:0));
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
private:
  bool isV1()const{return bytes[0]=='Z'&&bytes[1]=='T'&&bytes[2]=='N'&&bytes[3]=='1';}
  bool isV2()const{return bytes[0]=='Z'&&bytes[1]=='T'&&bytes[2]=='N'&&bytes[3]=='2';}
};
static_assert(sizeof(NotificationSettings)==NotificationSettings::Size);
}
