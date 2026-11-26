// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mt::mqtt {

// Validates one ordinary MQTT topic level (the channel identifier). It is
// deliberately narrower than MQTT topic strings: separators and wildcard
// bytes cannot broaden a per-channel consent filter.
struct MqttTopicLevel {
  static constexpr size_t MaxBytes=128;
  static bool valid(const char *text,size_t length) {
    if(!text||!length||length>MaxBytes)return false;
    size_t i=0;
    while(i<length) {
      const uint8_t a=static_cast<uint8_t>(text[i]);
      if(!a||a=='/'||a=='+'||a=='#'||a<0x20||a==0x7f)return false;
      if(a<0x80){++i;continue;}
      uint32_t cp=0;size_t n=0;
      if(a>=0xc2&&a<=0xdf){cp=a&0x1fu;n=2;}
      else if(a>=0xe0&&a<=0xef){cp=a&0x0fu;n=3;}
      else if(a>=0xf0&&a<=0xf4){cp=a&0x07u;n=4;}
      else return false;
      if(i+n>length)return false;
      for(size_t j=1;j<n;++j) {
        const uint8_t b=static_cast<uint8_t>(text[i+j]);
        if((b&0xc0u)!=0x80u)return false;
        if(j==1&&((a==0xe0&&b<0xa0)||(a==0xed&&b>=0xa0)||
                  (a==0xf0&&b<0x90)||(a==0xf4&&b>=0x90)))return false;
        cp=(cp<<6)|(b&0x3fu);
      }
      if(cp>=0x80&&cp<=0x9f)return false; // Unicode C1 control range
      i+=n;
    }
    return true;
  }
  static bool validCString(const char *text) {
    if(!text)return false;
    size_t n=0;while(n<=MaxBytes&&text[n])++n;
    return n<=MaxBytes&&valid(text,n);
  }
};

}  // namespace mt::mqtt
