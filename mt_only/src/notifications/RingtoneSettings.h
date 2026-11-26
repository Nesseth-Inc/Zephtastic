// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <string.h>
#include "meshtastic/admin.pb.h"
#include "RingtoneParser.h"

namespace mt::notification {
struct RingtoneSettings {
  static constexpr size_t MaxBytes=230,Capacity=MaxBytes+1;
  static constexpr char DefaultRingtone[]="meshtastic:d=4,o=5,b=140:c";
  enum class AssignResult:uint8_t {Changed,Unchanged,IgnoredEmpty,Invalid};
  char ringtone[Capacity]{};
  RingtoneSettings(){memcpy(ringtone,DefaultRingtone,sizeof(DefaultRingtone));}
  size_t length()const{const void *z=memchr(ringtone,0,sizeof(ringtone));return z?size_t(static_cast<const char*>(z)-ringtone):sizeof(ringtone);}
  bool valid()const{
    const void *z=memchr(ringtone,0,sizeof(ringtone));if(!z)return false;
    const auto *end=static_cast<const char*>(z);if(end==ringtone)return false;
    for(const char *p=end+1;p!=ringtone+sizeof(ringtone);++p)if(*p)return false;
    RingtoneParser::Settings parsed{};return RingtoneParser::parse(ringtone,size_t(end-ringtone),parsed);
  }
  bool same(const RingtoneSettings &other)const{return memcmp(ringtone,other.ringtone,Capacity)==0;}
  AssignResult assign(const char *data,size_t n){
    if(!n)return AssignResult::IgnoredEmpty;
    if(!data||n>MaxBytes||memchr(data,0,n))return AssignResult::Invalid;
    RingtoneSettings candidate;memset(candidate.ringtone,0,Capacity);memcpy(candidate.ringtone,data,n);
    if(!candidate.valid())return AssignResult::Invalid;if(same(candidate))return AssignResult::Unchanged;
    *this=candidate;return AssignResult::Changed;
  }
  AssignResult assignAdminSet(const meshtastic_AdminMessage &request){
    if(request.which_payload_variant!=meshtastic_AdminMessage_set_ringtone_message_tag)return AssignResult::Invalid;
    size_t n=0;while(n<Capacity&&request.set_ringtone_message[n])++n;
    if(n==Capacity)return AssignResult::Invalid;
    for(size_t i=n+1;i<Capacity;++i)if(request.set_ringtone_message[i])return AssignResult::Invalid;
    return assign(request.set_ringtone_message,n);
  }
  bool writeAdminResponse(meshtastic_AdminMessage &response)const{
    if(!valid())return false;response.which_payload_variant=meshtastic_AdminMessage_get_ringtone_response_tag;
    memcpy(response.get_ringtone_response,ringtone,Capacity);return true;
  }
};
static_assert(sizeof(RingtoneSettings)==RingtoneSettings::Capacity);
static_assert(sizeof(RingtoneSettings::DefaultRingtone)-1<=RingtoneSettings::MaxBytes);
}
