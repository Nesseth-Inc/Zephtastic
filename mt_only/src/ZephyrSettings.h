// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "SettingsJournal.h"
#include <zephyr/settings/settings.h>
#include <string.h>

namespace mt {
// All accesses run under serviceLock. A length mismatch is an I/O failure,
// never a truncated record that may accidentally be accepted as fresh state.
class ZephyrSettings final:public SettingsBackend {
public:
  // Volatile numeric-only diagnostics for the independent extended-module
  // journal. This deliberately records neither setting names nor value bytes.
  // All access is serialized by serviceLock, like the backend itself.
  // Ops are 1=read/2=write; slots are 0/1. For reads lastLength is the
  // observed stored/result length and lastCapacity is requested capacity;
  // for writes lastLength is requested size and lastCapacity is zero.
  struct ExtendedDiagnostic {
    uint32_t reads=0,writes=0,failures=0;
    int32_t lastRc=0,firstFailedRc=0;
    uint16_t lastLength=0,lastCapacity=0;
    uint8_t lastOp=0,lastSlot=0xff,firstFailedOp=0,firstFailedSlot=0xff;
  };
  static_assert(sizeof(ExtendedDiagnostic)<=36,"extended diagnostics must remain small");
private:
  ExtendedDiagnostic extendedDiagnostic_{};
  static uint16_t smallLength(size_t value){return uint16_t(value>UINT16_MAX?UINT16_MAX:value);}
  static uint16_t smallLength(ssize_t value){return value<=0?0:smallLength(size_t(value));}
  static uint8_t extendedSlot(const char *key){
    if(!key)return 0xff;
    if(!strcmp(key,"mtextra/state0"))return 0;
    if(!strcmp(key,"mtextra/state1"))return 1;
    return 0xff;
  }
  void noteExtended(uint8_t op,uint8_t slot,int rc,size_t length,size_t capacity){
    if(slot>1)return;
    auto &d=extendedDiagnostic_;
    uint32_t &count=op==1?d.reads:d.writes;if(count!=UINT32_MAX)++count;
    d.lastOp=op;d.lastSlot=slot;d.lastRc=rc;
    d.lastLength=smallLength(length);d.lastCapacity=smallLength(capacity);
    // Missing slots are normal for an unused journal; preserve their lastRc
    // while excluding them from failure counts and first-unexpected-failure.
    const bool failed=op==1?(rc<0&&rc!=-ENOENT):rc!=0;
    if(failed){if(d.failures!=UINT32_MAX)++d.failures;
      if(!d.firstFailedOp){d.firstFailedOp=op;d.firstFailedSlot=slot;d.firstFailedRc=rc;}}
  }
public:
  int read(const char *key,uint8_t *out,size_t capacity)override {
    const uint8_t slot=extendedSlot(key);
    const ssize_t length=settings_get_val_len(key);
    int result=0;size_t observed=0;
    if(length<0)result=int(length);
    else if(!length)result=-ENOENT;
    else if(size_t(length)>capacity){observed=size_t(length);result=-E2BIG;}
    else {
      observed=size_t(length);
      const ssize_t count=settings_load_one(key,out,capacity);
      if(count<0)result=int(count);
      else {observed=size_t(count);result=count==length?int(count):-EIO;}
    }
    noteExtended(1,slot,result,observed,capacity);return result;
  }
  int write(const char *key,const uint8_t *data,size_t length)override {
    const uint8_t slot=extendedSlot(key);const int result=settings_save_one(key,data,length);
    noteExtended(2,slot,result,length,0);return result;
  }
  // A reset owns an enumerated application key and verifies absence, including
  // an interrupted earlier pass that already removed it.
  int eraseVerified(const char *key){
    const auto length=settings_get_val_len(key);
    if(length==-ENOENT||length==0)return 0;
    if(length<0)return int(length);
    const int rc=settings_delete(key);if(rc&&rc!=-ENOENT)return rc;
    const auto remaining=settings_get_val_len(key);
    return remaining==-ENOENT||remaining==0?0:remaining<0?int(remaining):-EIO;
  }
  ExtendedDiagnostic extendedDiagnostic()const{return extendedDiagnostic_;}
};
}
