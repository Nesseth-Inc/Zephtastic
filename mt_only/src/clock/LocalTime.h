// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace mt::clock {
class LocalTime {
  static bool alpha(char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z');}
  static bool digit(char c){return c>='0'&&c<='9';}
  static bool name(const char *&p){
    const bool quoted=*p=='<';if(quoted)++p;
    const char *first=p;
    while(alpha(*p)||(quoted&&(digit(*p)||*p=='+'||*p=='-')))++p;
    if(p-first<3||p-first>10)return false;
    if(quoted){if(*p!='>')return false;++p;}
    return true;
  }
  static bool number(const char *&p,unsigned low,unsigned high){
    if(!digit(*p))return false;
    unsigned n=0;
    do{n=n*10+unsigned(*p++-'0');if(n>high)return false;}while(digit(*p));
    return n>=low;
  }
  static bool offset(const char *&p){
    if(*p=='-'||*p=='+')++p;
    if(!number(p,0,24))return false;
    for(unsigned n=0;n<2&&*p==':';++n){++p;if(!number(p,0,59))return false;}
    return true;
  }
  static bool rule(const char *&p){
    if(*p=='M'){
      ++p;
      if(!number(p,1,12)||*p++!='.'||!number(p,1,5)||*p++!='.'||!number(p,0,6))return false;
    }else if(*p=='J'){++p;if(!number(p,1,365))return false;}
    else if(!number(p,0,365))return false;
    if(*p!='/')return true;
    ++p;return offset(p);
  }
public:
  // POSIX TZ rule strings, including second-resolution offsets, Julian days,
  // month/week/weekdays and angle-quoted numeric names. No zoneinfo filesystem.
  static bool valid(const char *p){
    if(!*p)return true;
    if(*p==':')++p;
    if(!name(p)||!offset(p))return false;
    if(!*p)return true;
    if(!name(p))return false;
    if(*p&&*p!=','&&!offset(p))return false;
    if(!*p)return true;
    return *p++==','&&rule(p)&&*p++==','&&rule(p)&&!*p;
  }
  // The application owner serializes local-clock display/log projections.
  // Match pinned RTC::getTZOffset: interpret the UTC civil fields with
  // mktime's automatic DST selection. UTC storage/telemetry never call here.
  static uint32_t project(const char *zone,uint32_t utc);
  // Stock graphics/SharedUIDisplay.cpp clock label.
  static void label(uint32_t local,bool twelve,char (&out)[10]){
    if(!local){strcpy(out,"--:--");return;}
    const unsigned h=local/3600%24,m=local/60%60;
    if(twelve)snprintf(out,sizeof(out),"%u:%02u%c",h%12?h%12:12,m,h>=12?'p':'a');
    else snprintf(out,sizeof(out),"%u:%02u",h,m);
  }
};
}
