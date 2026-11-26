// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "SettingsJournal.h"

namespace mt {
// A selector lives in the original NVS key. Its random incarnation fences
// old/orphan files on first migration. The file replacement is atomic and
// the two rollback slots use different names.
class LargeNodeFiles {
public:
  struct Token {uint8_t bytes[28];};
  virtual bool mounted()const=0;
  virtual int nonce(uint8_t *out,size_t n)=0;
  virtual int load(unsigned slot,const Token&,uint8_t *out,size_t capacity)=0;
  virtual int replace(unsigned slot,const Token&,const uint8_t *data,size_t n,bool enrolled)=0;
  virtual int discard(unsigned slot)=0;
};

class LargeNodeRecords {
  using Token=LargeNodeFiles::Token;
  LargeNodeFiles *files_=nullptr;
  Token tokens_[2]{};
  uint8_t active_=0;
  static bool marked(const uint8_t *buf,int n){return n==int(sizeof(Token))&&!memcmp(buf,"ZNX1",4);}
public:
  void bind(LargeNodeFiles &files){files_=&files;}
  void reset(){active_=0;}
  bool available()const{return files_&&files_->mounted();}
  bool needed()const{return active_!=0;}
  bool selected(unsigned slot)const{return (active_&(1u<<slot))!=0;}
  int read(SettingsBackend &native,const char *key,unsigned slot,uint32_t self,uint8_t *raw,size_t cap){
    int n=native.read(key,raw,cap);
    if(!marked(raw,n)){active_&=uint8_t(~(1u<<slot));return n;}
    auto &t=tokens_[slot];memcpy(t.bytes,raw,sizeof(t));
    if(NodeCache::Entry::get32(t.bytes+4)!=self||t.bytes[8]!=slot||t.bytes[9]||t.bytes[10]||t.bytes[11])return -EILSEQ;
    uint8_t any=0;for(unsigned i=12;i<sizeof(t);++i)any|=t.bytes[i];
    if(!any||!files_)return -EILSEQ;
    active_|=uint8_t(1u<<slot);
    n=files_->load(slot,t,raw,cap);
    return n==-ENOENT?-EILSEQ:n;
  }
  int write(SettingsBackend &native,const char *key,unsigned slot,uint32_t self,const uint8_t *raw,size_t n,bool large,bool &fileFull){
    fileFull=false;
    if(!large){
      const int rc=native.write(key,raw,n);
      if(!rc)active_&=uint8_t(~(1u<<slot));
      return rc;
    }
    if(!files_)return -ENODEV;
    const bool enrolled=selected(slot);
    auto &t=tokens_[slot];
    if(!enrolled){
      t={};memcpy(t.bytes,"ZNX1",4);NodeCache::Entry::put32(t.bytes+4,self);t.bytes[8]=uint8_t(slot);
      int rc=files_->nonce(t.bytes+12,16);if(rc)return rc;
      uint8_t any=0;for(unsigned i=12;i<sizeof(t);++i)any|=t.bytes[i];if(!any)return -EIO;
    }
    const int saved=files_->replace(slot,t,raw,n,enrolled);
    if(saved){fileFull=saved==-ENOSPC;return saved;}
    if(enrolled)return 0;
    const int rc=native.write(key,t.bytes,sizeof(t));if(!rc)active_|=uint8_t(1u<<slot);
    return rc;
  }
  int retired(unsigned slot){return files_->discard(slot);}
};
}
