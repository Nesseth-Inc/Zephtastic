// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrSettings.h"
#include <cassert>
#include <cstdio>

namespace {
ssize_t storedLength=336,loadedLength=336,lookupError=0,loadError=0;
int deleteResult=0;bool deleteRemoved=false;
int writeResult=0;unsigned loadCalls=0;
}
extern "C" ssize_t settings_get_val_len(const char *) {
  return lookupError?lookupError:storedLength;
}
extern "C" ssize_t settings_load_one(const char *,void *out,size_t capacity) {
  ++loadCalls;
  if(loadError)return loadError;
  if(loadedLength>0&&size_t(loadedLength)<=capacity)memset(out,0xa5,size_t(loadedLength));
  return loadedLength;
}
extern "C" int settings_delete(const char *){if(deleteRemoved && !deleteResult)storedLength=-ENOENT;return deleteResult;}
extern "C" int settings_save_one(const char *,const void *,size_t) {return writeResult;}

int main(){
  static_assert(sizeof(mt::ZephyrSettings::ExtendedDiagnostic)<=36);
  mt::ZephyrSettings backend;uint8_t bytes[336]{};
  assert(backend.read("mtextra/state0",bytes,sizeof(bytes))==336);
  auto d=backend.extendedDiagnostic();
  assert(d.reads==1&&d.writes==0&&d.failures==0&&d.lastOp==1&&d.lastSlot==0&&
         d.lastRc==336&&d.lastLength==336&&d.lastCapacity==336&&d.firstFailedOp==0);

  // Unrelated namespaces and unknown mtextra keys do not enter this record.
  storedLength=3;loadedLength=3;assert(backend.read("mtbase/state0",bytes,sizeof(bytes))==3);
  assert(backend.write("mtextra/state9",bytes,4)==0);
  d=backend.extendedDiagnostic();assert(d.reads==1&&d.writes==0&&d.failures==0);

  lookupError=-ENOENT;
  assert(backend.read("mtextra/state1",bytes,sizeof(bytes))==-ENOENT);
  d=backend.extendedDiagnostic();assert(d.failures==0&&d.lastRc==-ENOENT&&d.firstFailedOp==0);
  lookupError=0;storedLength=337;
  const unsigned loadsBefore=loadCalls;
  assert(backend.read("mtextra/state0",bytes,sizeof(bytes))==-E2BIG&&loadCalls==loadsBefore);
  assert(backend.read("mtextra/state0",bytes,sizeof(bytes))==-E2BIG);
  storedLength=336;loadedLength=335;
  assert(backend.read("mtextra/state0",bytes,sizeof(bytes))==-EIO);
  loadedLength=336;loadError=-EAGAIN;
  assert(backend.read("mtextra/state0",bytes,sizeof(bytes))==-EAGAIN);
  d=backend.extendedDiagnostic();
  assert(d.reads==6&&d.failures==4&&d.lastOp==1&&d.lastSlot==0&&d.lastRc==-EAGAIN&&
         d.lastLength==336&&d.lastCapacity==336&&d.firstFailedOp==1&&d.firstFailedSlot==0&&
         d.firstFailedRc==-E2BIG);

  writeResult=0;assert(backend.write("mtextra/state0",bytes,sizeof(bytes))==0);
  writeResult=-EROFS;assert(backend.write("mtextra/state1",bytes,sizeof(bytes))==-EROFS);
  d=backend.extendedDiagnostic();
  assert(d.reads==6&&d.writes==2&&d.failures==5&&d.lastOp==2&&d.lastSlot==1&&
         d.lastRc==-EROFS&&d.lastLength==336&&d.lastCapacity==0&&
         d.firstFailedOp==1&&d.firstFailedSlot==0&&d.firstFailedRc==-E2BIG);
  mt::ZephyrSettings missingThenWrite;lookupError=-ENOENT;
  assert(missingThenWrite.read("mtextra/state0",bytes,sizeof(bytes))==-ENOENT);
  lookupError=0;writeResult=-ENOSPC;
  assert(missingThenWrite.write("mtextra/state1",bytes,sizeof(bytes))==-ENOSPC);
  d=missingThenWrite.extendedDiagnostic();
  assert(d.reads==1&&d.writes==1&&d.failures==1&&d.lastRc==-ENOSPC&&
         d.firstFailedOp==2&&d.firstFailedSlot==1&&d.firstFailedRc==-ENOSPC);
  lookupError=0;storedLength=8;deleteResult=-EIO;
  assert(missingThenWrite.eraseVerified("mtgame/score")==-EIO);
  deleteResult=0;
  assert(missingThenWrite.eraseVerified("mtgame/score")==-EIO);
  deleteRemoved=true;
  assert(missingThenWrite.eraseVerified("mtgame/score")==0);
  assert(missingThenWrite.eraseVerified("mtgame/score")==0);
  puts("PASS ZephyrSettings numeric mtextra diagnostics: success, missing, oversize, short read, I/O failures, write result, namespace filtering, first failure retention");
}
