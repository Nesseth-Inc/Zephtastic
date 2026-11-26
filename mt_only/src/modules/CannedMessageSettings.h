// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../stock/peripherals/CannedMessageModule.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::modules {
// The stock companion protocol carries one pipe-delimited string in a
// char[201] field. Keep its bytes intact; parsing and menu presentation belong
// to the eventual client/input adapter, not this persistent value type.
struct CannedMessageSettings {
  static constexpr size_t MaxBytes=200;
  static constexpr size_t Capacity=MaxBytes+1;
  static constexpr char DefaultMessages[]="Hi|Bye|Yes|No|Ok";
  using AssignResult = stock::canned::AssignResult;

  char messages[Capacity]{};

  CannedMessageSettings(){memcpy(messages,DefaultMessages,sizeof(DefaultMessages));}

  bool valid()const {
    const void *terminator=memchr(messages,0,sizeof(messages));
    if(!terminator)return false;
    const auto *end=static_cast<const char *>(terminator);
    for(const char *p=end+1;p!=messages+sizeof(messages);++p)if(*p)return false;
    return end!=messages; // Empty setter values are ignored by the stock API.
  }

  bool same(const CannedMessageSettings &other)const {
    return memcmp(messages,other.messages,sizeof(messages))==0;
  }

  size_t length()const {
    const void *terminator=memchr(messages,0,sizeof(messages));
    return terminator?size_t(static_cast<const char *>(terminator)-messages):sizeof(messages);
  }

  // `length` counts string bytes, not a terminating NUL. UTF-8 is treated as
  // opaque bytes so multibyte entries fit the same 200-byte wire limit.
  AssignResult assign(const char *value,size_t length) {
    return stock::canned::handleSetCannedMessageModuleMessages(messages, value, length);
  }
};

static_assert(sizeof(CannedMessageSettings)==CannedMessageSettings::Capacity);
static_assert(sizeof(CannedMessageSettings::DefaultMessages)-1<=CannedMessageSettings::MaxBytes);
}
