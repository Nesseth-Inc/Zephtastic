// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../stock/peripherals/CannedMessageModule.h"

namespace mt::ui {
// Views the existing pipe-delimited setting without allocating a second copy.
struct CannedMenu {
  static constexpr unsigned MaxMessages = stock::canned::MaxMessages;
  const char *list="";
  size_t size=0;
  void bind(const char *value,size_t length) { list=value?value:""; size=value?length:0; }
  unsigned count() const {
    return stock::canned::splitConfiguredMessages(list, size, [](unsigned, const char *, size_t) {});
  }
  unsigned itemCount() const {
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
    return count()+2; // configured messages, Back, Compose
#else
    return count()+1;
#endif
  }
  bool entry(unsigned index,const char *&value,size_t &length) const {
    const unsigned total = stock::canned::splitConfiguredMessages(list, size,
        [&](unsigned at, const char *text, size_t bytes) {
          if (at == index) { value = text; length = bytes; }
        });
    if (index == total) { value = "[Back]"; length = 6; }
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
    if(index==total+1){value="[Compose]";length=9;return true;}
#endif
    return index <= total;
  }
};
}
