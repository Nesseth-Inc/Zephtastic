// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiProvisioning.h"
#include "provisioning/FreshDomain.h"
#include "ZephyrPkiStorage.h"

namespace mt::pki {
// Both adapters resolve the same already-mounted Zephyr SETTINGS_NVS backend.
// No caller-selected store, mount, repair, export or namespace is accepted.
class NvsProvisioningSettings final:public FreshProvisioningSettings {
  NvsBootSettings boot_;ZephyrSettings writer_;bool reading_=false;
  NvsBootSettings::ScanProgress progress_=nullptr;
  void *progressContext_=nullptr;
  int beginScan(){return boot_.beginWithProgress(progress_,progressContext_);}
public:
  void setScanProgress(NvsBootSettings::ScanProgress progress, void *context){
    progress_=progress;progressContext_=context;
  }
  int begin()override {
    if(reading_)return -EBUSY;
    const int result=beginScan();if(!result)reading_=true;return result;
  }
  void end()override {boot_.end();reading_=false;}
  int read(const char *name,uint8_t *out,size_t capacity)override {
    return reading_?boot_.read(name,out,capacity):-EACCES;
  }
  int persist(const char *name,const uint8_t *bytes,size_t size)override {
    if(reading_||!name||!bytes)return -EACCES;
    const bool identity=!strcmp(name,"mtidentity/state0")&&size==IdentityJournal::RecordSize;
    const bool ledger=(!strcmp(name,"mtpki/ledger0")||!strcmp(name,"mtpki/ledger1"))&&size==PkiLedger::RecordSize;
    const bool freshRoot=!strcmp(name,PkiFreshProvisioning::RootKey)&&size==PkiFreshRoot::RecordBytes;
    const bool freshIntent=!strcmp(name,PkiFreshIntent::Key)&&size==PkiFreshIntent::RecordBytes;
    const bool freshCounter=(!strcmp(name,"mtpki/nonces0")||!strcmp(name,"mtpki/nonces1"))&&size==PkiNonceCounter::RecordBytes;
    const bool freshContacts=(!strcmp(name,"mtpki/contacts0")||!strcmp(name,"mtpki/contacts1"))&&size==PkiContactStore::RecordBytes;
    const bool immutableGenesis=identity||ledger||freshRoot||freshIntent||freshCounter||freshContacts;
    if(immutableGenesis){
      uint8_t prior[PkiLedger::RecordSize>PkiContactStore::RecordBytes?PkiLedger::RecordSize:PkiContactStore::RecordBytes]{};
      const int start=beginScan();if(start)return start;
      const int found=boot_.read(name,prior,size);boot_.end();eraseIdentityBytes(prior,sizeof(prior));
      if(found!=-ENOENT)return found<0?found:-EEXIST;
    }
    return immutableGenesis?writer_.write(name,bytes,size):-EACCES;
  }
};
}
