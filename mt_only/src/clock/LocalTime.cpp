// SPDX-License-Identifier: GPL-3.0-only
// Expose the pinned Picolibc setenv() declaration only in the presentation clock.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "LocalTime.h"

namespace mt::clock {
uint32_t LocalTime::project(const char *zone,uint32_t utc){
    if(!utc||!*zone)return utc;
    const char *active=getenv("TZ");
    if(!active||strcmp(active,zone)){
      if(setenv("TZ",zone,1))return 0;
      tzset();
    }
    const time_t time=utc;struct tm civil{};
    if(!gmtime_r(&time,&civil))return 0;
    civil.tm_isdst=-1;
    const time_t interpreted=mktime(&civil);if(interpreted==time_t(-1))return 0;
    const int64_t result=int64_t(utc)*2-int64_t(interpreted);
    return result>0&&result<=UINT32_MAX?uint32_t(result):0;
  }
}
