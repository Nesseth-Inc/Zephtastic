// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace mt::stock::admin::device_reset {
// An append-only erase page kept outside settings. Programming never erases
// the page. A retired tuple stays retired even if old NVS bytes are restored.
class Floor {
public:
  static constexpr size_t PageBytes=4096,SlotBytes=128,Slots=PageBytes/SlotBytes;
  enum class State:uint8_t {Unused,Retiring,Selected,Activating,Active};
  struct Head {State state=State::Unused;unsigned count=0;uint8_t oldKey[32]{},newKey[32]{};};
private:
  static bool blank(const uint8_t *b,size_t n){for(size_t i=0;i<n;++i)if(b[i]!=0xff)return false;return true;}
  static bool keyValid(const uint8_t *b){uint8_t any=0;for(unsigned i=0;i<32;++i)any|=b[i];return any&&!blank(b,32);}
  static uint32_t crc(const uint8_t *b,unsigned n){uint32_t c=~0u;while(n--){c^=*b++;for(unsigned i=0;i<8;++i)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}return ~c;}
  static void tuple(uint8_t out[48],char kind,unsigned slot,const uint8_t key[32]){
    memset(out,0,48);memcpy(out,"ZTRF",4);out[4]=1;out[5]=uint8_t(kind);out[6]=uint8_t(slot);
    memcpy(out+8,key,32);
    const uint32_t hash=crc(out,40);for(unsigned i=0;i<4;++i)out[40+i]=uint8_t(hash>>(8*i));
  }
  static void done(uint8_t out[16],unsigned slot){
    memset(out,0,16);memcpy(out,"ZTOK",4);out[4]=1;out[5]=uint8_t(slot);
  }
  template<class Flash> static int program(Flash &flash,unsigned at,const uint8_t *data,unsigned n){
    uint8_t actual[48];int rc=flash.read(at,actual,n);if(rc)return rc;
    // Allow retry after a prefix/bit interruption only when no existing bit
    // contradicts this exact already-authorized word.
    for(unsigned i=0;i<n;++i)if((actual[i]&data[i])!=data[i])return -EILSEQ;
    if(memcmp(actual,data,n)){rc=flash.program(at,data,n);if(rc)return rc;}
    rc=flash.read(at,actual,n);return rc?rc:memcmp(actual,data,n)?-EIO:0;
  }
public:
  // Earlier layouts used this address for a filesystem. Recognize the
  // enrollment header, including an interrupted first word, without writing
  // or interpreting unrelated filesystem contents as an enrollment.
  static bool enrollmentHeader(const uint8_t bytes[8]){
    constexpr uint8_t expected[]={'Z','T','R','F',1,'R',0,0};
    bool programmed=false;
    for(unsigned i=0;i<sizeof(expected);++i){
      if((bytes[i]&expected[i])!=expected[i])return false;
      if(bytes[i]!=0xff)programmed=true;
    }
    return programmed;
  }
  template<class Flash> static int inspect(Flash &flash,Head &head,unsigned *badSlot=nullptr){
    head={};Head last{};bool end=false;
    for(unsigned i=0;i<Slots;++i){
      head=last;if(badSlot)*badSlot=i;
      uint8_t slot[SlotBytes],expected[48];int rc=flash.read(i*SlotBytes,slot,sizeof(slot));if(rc)return rc;
      if(blank(slot,sizeof(slot))){end=true;continue;}
      if(end||!blank(slot+112,16)||!keyValid(slot+8))return -EILSEQ;
      tuple(expected,'R',i,slot+8);
      if(memcmp(expected,slot,48)||(i&&(last.state!=State::Active||memcmp(slot+8,last.newKey,32))))return -EILSEQ;
      last.state=State::Retiring;last.count=i+1;memcpy(last.oldKey,slot+8,32);memset(last.newKey,0,32);
      if(!blank(slot+48,48)){
        if(!keyValid(slot+56)||!memcmp(slot+56,slot+8,32))return -EILSEQ;
        tuple(expected,'N',i,slot+56);if(memcmp(expected,slot+48,48))return -EILSEQ;
        memcpy(last.newKey,slot+56,32);last.state=State::Selected;
      }
      if(!blank(slot+96,16)){
        if(last.state!=State::Selected)return -EILSEQ;
        done(expected,i);
        for(unsigned j=0;j<16;++j)if((slot[96+j]&expected[j])!=expected[j])return -EILSEQ;
        last.state=memcmp(expected,slot+96,16)?State::Activating:State::Active;
      }
    }
    head=last;return 0;
  }
  template<class Flash> static int retire(Flash &flash,const Head &h,const uint8_t key[32]){
    if(!keyValid(key)||(h.state!=State::Unused&&h.state!=State::Active))return -EINVAL;
    if(h.state==State::Active&&memcmp(h.newKey,key,32))return -EACCES;
    if(h.count>=Slots)return -ENOSPC;
    uint8_t value[48];tuple(value,'R',h.count,key);return program(flash,h.count*SlotBytes,value,sizeof(value));
  }
  template<class Flash> static int select(Flash &flash,const Head &h,const uint8_t key[32]){
    if(h.state!=State::Retiring||!h.count||!keyValid(key))return -EINVAL;
    uint8_t retired[32];
    for(unsigned i=0;i<h.count;++i){
      const int rc=flash.read(i*SlotBytes+8,retired,32);if(rc)return rc;
      if(!memcmp(key,retired,32))return -EEXIST;
    }
    uint8_t value[48];tuple(value,'N',h.count-1,key);return program(flash,(h.count-1)*SlotBytes+48,value,sizeof(value));
  }
  // Resume only a compatible interrupted tail, under an existing verified
  // request. Never append from an NVS marker alone after settings rollback.
  template<class Flash> static int recoverTail(Flash &flash,const Head &previous,unsigned bad,
                                                const uint8_t oldKey[32],const uint8_t newKey[32]){
    if(bad!=previous.count||bad>=Slots||
       (previous.state!=State::Unused&&(previous.state!=State::Active||memcmp(previous.newKey,oldKey,32))))return -EILSEQ;
    uint8_t row[SlotBytes];
    for(unsigned i=bad+1;i<Slots;++i){
      const int rc=flash.read(i*SlotBytes,row,sizeof(row));if(rc)return rc;
      if(!blank(row,sizeof(row)))return -EILSEQ;
    }
    const unsigned at=bad*SlotBytes;
    int rc=flash.read(at,row,sizeof(row));if(rc)return rc;
    if(blank(row,48)||!blank(row+96,32))return -EILSEQ;
    rc=retire(flash,previous,oldKey);if(rc)return rc;
    Head retiring=previous;retiring.state=State::Retiring;retiring.count=bad+1;
    return select(flash,retiring,newKey);
  }
  template<class Flash> static int activate(Flash &flash,const Head &h,const uint8_t verified[32]){
    if((h.state!=State::Selected&&h.state!=State::Activating)||!h.count||memcmp(verified,h.newKey,32))return -EACCES;
    uint8_t value[16];done(value,h.count-1);return program(flash,(h.count-1)*SlotBytes+96,value,sizeof(value));
  }
};
}
