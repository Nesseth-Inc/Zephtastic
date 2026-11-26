// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "UsbProvisionCommand.h"
#include <stdio.h>

namespace mt::pki {
// The production implementation binds only a measured, connected CDC epoch.
// It shares the provisioning context, actual service mutex and hardware gate.
struct ProvisionUsbSession {
  virtual ~ProvisionUsbSession()=default;
  virtual bool bind(uint32_t epoch)=0;
  virtual bool current(uint32_t epoch)const=0;
  virtual bool current()const=0;
  virtual void clear()=0;
};

// Bounded local command adapter. No radio control, key access, store selection,
// reboot, retries or allocation. The core remains the sole credential writer.
class PkiUsbIngress {
  PkiProvisioning &core_;ProvisionUsbSession &session_;
  static void hex(char out[65],const uint8_t value[32]) {
    constexpr char digits[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){out[2*i]=digits[value[i]>>4];out[2*i+1]=digits[value[i]&15];}
    out[64]=0;
  }
public:
  PkiUsbIngress(PkiProvisioning &core,ProvisionUsbSession &session):core_(core),session_(session){}
  void disconnect(){if(core_.state()==ProvisionState::Staged)(void)core_.cancel();session_.clear();}
  void tick(){
    if(core_.state()!=ProvisionState::Staged)return;
    if(!session_.current())disconnect();else (void)core_.tick();
  }
  // Positive length is a complete numeric/public result, including core error.
  // A negative result publishes no response and must never trigger a retry.
  int execute(const uint8_t *bytes,size_t size,uint32_t epoch,char *out,size_t capacity) {
    UsbProvisionCommand command;
    if(!out||capacity<224||!UsbProvisionParser::parse(bytes,size,command))return -EINVAL;
    if(!session_.bind(epoch)||!session_.current(epoch)){disconnect();return -EACCES;}
    ProvisionStatus status;IdentityBinding candidate;bool staged=false;int result=0;
    switch(command.op) {
    case UsbProvisionCommand::Op::Stage:
      result=core_.stage(candidate);staged=result==0;break;
    case UsbProvisionCommand::Op::Commit:
      result=core_.commit(command.expected,command.peer);break;
    case UsbProvisionCommand::Op::Cancel:
      result=core_.cancel()?0:-EACCES;break;
    case UsbProvisionCommand::Op::Status:break;
    }
    // inspect performs no generation or repair and rechecks current quiescence.
    const int inspected=core_.inspect(status);
    if(!result&&inspected)result=inspected;
    if(!session_.current(epoch)){disconnect();return -EACCES;}
    if(staged&&!result){status.local=candidate;status.hasBinding=true;}
    if(!status.hasBinding)status.local=IdentityBinding{};
    if(!status.hasPeer)status.peer=ProvisionPeer{};
    char local[65]{},peer[65]{};hex(local,status.local.publicKey);hex(peer,status.peer.publicKey);
    const int n=snprintf(out,capacity,"pki_v1 rc=%d state=%u stored=%u local=%u node=%08x pub=%s peer=%u pnode=%08x ppub=%s",
      result,unsigned(core_.state()),unsigned(status.stored),unsigned(status.hasBinding),
      unsigned(status.local.node),local,unsigned(status.hasPeer),unsigned(status.peer.node),peer);
    eraseIdentityBytes(&candidate,sizeof(candidate));
    if(n<=0||size_t(n)>=capacity){out[0]=0;return -ENOSPC;}
    return n;
  }
};
}
