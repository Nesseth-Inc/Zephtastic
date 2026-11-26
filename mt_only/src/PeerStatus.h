// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
namespace mt {
// RAM-only value, separated from the Qwen-proposed wire decoder so native
// cache/journal code has no protobuf dependency. Known empty differs from absent.
class PeerStatus {
  char text_[80]{};uint8_t flags_=0;
public:
  bool present()const{return flags_&1;}
  bool replayable()const{return flags_&2;}
  void allowReplay(bool allowed){flags_=uint8_t((flags_&1)|(allowed?2:0));}
  const char *text()const{return text_;}
  void clear(){memset(text_,0,sizeof(text_));flags_=0;}
  bool assign(const char (&text)[80]){
    const auto end=static_cast<const char*>(memchr(text,0,sizeof(text)));
    if(!end)return false;
    for(size_t n=size_t(end-text);n<sizeof(text);++n)if(text[n])return false;
    memcpy(text_,text,sizeof(text_));flags_=1;return true;
  }
};
static_assert(sizeof(PeerStatus)==81,"Volatile peer status RAM budget");
}
