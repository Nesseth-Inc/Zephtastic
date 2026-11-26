// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "provisioning/FreshDomain.h"
#include <stdio.h>

namespace mt::pki {
// Public-only local setup protocol. stage never writes; confirm must repeat
// the exact public candidate. A persisted intent can be resumed only through
// another explicit stage/confirm after reboot, never by the ordinary startup.
template<class UsbSession> class PkiFreshUsbIngress {
  PkiFreshProvisioning &core_;UsbSession &session_;
  static int nibble(uint8_t c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;}
public:
  static constexpr char Prefix[]="zt_pki_fresh_v1 ";
  static constexpr size_t PrefixLength=sizeof(Prefix)-1;
  PkiFreshUsbIngress(PkiFreshProvisioning &core,UsbSession &session):core_(core),session_(session){}
  void disconnect(){if(core_.state()==PkiFreshProvisioning::State::Staged)(void)core_.cancel();session_.clear();}
  void tick(){if(core_.state()==PkiFreshProvisioning::State::Staged){
    if(!session_.current())disconnect();else (void)core_.tick();}}
  int execute(const uint8_t *data,size_t size,uint32_t epoch,char *out,size_t capacity){
    if(!data||!out||capacity<180||size<PrefixLength||memcmp(data,Prefix,PrefixLength))return -EINVAL;
    data+=PrefixLength;size-=PrefixLength;
    enum Op {Stage,Confirm,Cancel,Status};Op op;
    IdentityBinding expected{};
    if(size==5&&!memcmp(data,"stage",5))op=Stage;
    else if(size==6&&!memcmp(data,"cancel",6))op=Cancel;
    else if(size==6&&!memcmp(data,"status",6))op=Status;
    else if(size==8+8+1+64&&!memcmp(data,"confirm ",8)&&data[16]==' '){
      op=Confirm;expected.generation=1;
      for(unsigned i=0;i<8;++i){const int n=nibble(data[8+i]);if(n<0)return -EINVAL;expected.node=(expected.node<<4)|uint32_t(n);}
      for(unsigned i=0;i<32;++i){const int a=nibble(data[17+2*i]),b=nibble(data[18+2*i]);
        if(a<0||b<0)return -EINVAL;expected.publicKey[i]=uint8_t((a<<4)|b);}
    }else return -EINVAL;
    if(!session_.bind(epoch)||!session_.current(epoch)){disconnect();return -EACCES;}
    IdentityBinding candidate{};int rc=0;bool have=false;
    switch(op){case Stage:rc=core_.stage(candidate);have=rc==0;break;
      case Confirm:rc=core_.confirm(expected);break;
      case Cancel:rc=core_.cancel()?0:-EACCES;break;
      case Status:rc=core_.tick();break;}
    if(!session_.current(epoch)){disconnect();return -EACCES;}
    char publicHex[65]{};constexpr char digits[]="0123456789abcdef";
    if(have){for(unsigned i=0;i<32;++i){publicHex[2*i]=digits[candidate.publicKey[i]>>4];publicHex[2*i+1]=digits[candidate.publicKey[i]&15];}}
    const int n=snprintf(out,capacity,"pki_fresh_v1 rc=%d state=%u error=%d candidate=%u node=%08x gen=%u pub=%s reboot=%u",
      rc,unsigned(core_.state()),core_.lastError(),unsigned(have),unsigned(candidate.node),unsigned(candidate.generation),
      publicHex,unsigned(core_.state()==PkiFreshProvisioning::State::Committed));
    return n>0&&size_t(n)<capacity?n:-ENOSPC;
  }
};
}
