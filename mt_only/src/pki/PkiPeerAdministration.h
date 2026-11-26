// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "LegacyTailTools.h"
#include "PkiStorage.h"
#include "UsbPeerCommand.h"
#include "UsbStorageCommand.h"
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
#include "UsbTailPrepareCommand.h"
#endif

namespace mt::pki {
struct PeerAdministrationContext {
  virtual ~PeerAdministrationContext()=default;
  virtual bool tryEnter()=0;
  virtual void leave()=0;
  virtual bool current(uint32_t epoch)const=0;
  virtual bool ready()const=0;
  virtual bool peerAllowed(const IdentityBinding &,const PkiPeerPin &)const=0;
  virtual bool pause()=0;
  virtual bool resume()=0;
  virtual void hold()=0;
};

// One exact local command, never fresh identity provisioning. The context owns
// actual service serialization, measured USB epoch and native RF pause/restore.
// A committed pin does not hot-replace runtime sessions; reboot loads the set.
class PkiPeerAdministration {
  PkiStorage &storage_;PeerAdministrationContext &context_;bool busy_=false;
  int enableStorageMode(const UsbStorageCommand &command,uint32_t epoch,unsigned mode){
#if !MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
    // The explicit compact-contact migration remains available in lean
    // profiles; legacy store/lifecycle upgrades are omitted entirely.
    if(mode!=2u)return -ENOTSUP;
#endif
    if(!context_.tryEnter())return -EBUSY;
    struct Leave {PeerAdministrationContext &context;~Leave(){context.leave();}} leave{context_};
    if(busy_)return -EBUSY;
    busy_=true;struct End {bool &busy;~End(){busy=false;}} end{busy_};
    if(!context_.current(epoch)||!context_.ready())return -EACCES;
    IdentityBinding local;
    if(!storage_.preparedBinding(local)||local.node!=command.localNode||
       memcmp(local.publicKey,command.localPublic,32))return -EINVAL;
    if(!context_.pause())return -EBUSY;
    if(!context_.current(epoch)){
      if(!context_.resume())context_.hold();
      return -EACCES;
    }
    int result=-EINVAL;
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
    if(mode==0)result=storage_.enableLongStorage(local);
    else if(mode==1)result=storage_.enableBoundedHistory(local);
#endif
    if(mode==2)result=storage_.enableCompactContacts(local);
    if(!context_.current(epoch)||storage_.state()!=PkiStorage::State::Active){
      context_.hold();return result<0?result:-EIO;
    }
    if(!context_.resume()){context_.hold();return -EIO;}
    if(!context_.current(epoch)){context_.hold();return -EACCES;}
    return result;
  }
public:
  PkiPeerAdministration(PkiStorage &storage,PeerAdministrationContext &context):storage_(storage),context_(context){}
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
  int prepareTail(AppendStore &store,HistoryPageEraser &eraser,PreparationProgress &progress,
                  const UsbTailPrepareCommand &command,uint32_t epoch,TailPreparationResult &out){
    out={};if(!context_.tryEnter())return -EBUSY;
    struct Leave {PeerAdministrationContext &c;~Leave(){c.leave();}} leave{context_};
    if(busy_)return -EBUSY;
    busy_=true;struct End {bool &b;~End(){b=false;}} end{busy_};
    if(!context_.current(epoch)||!context_.ready())return -EACCES;
    IdentityBinding local;
    if(!storage_.preparedBinding(local)||local.node!=command.localNode||memcmp(local.publicKey,command.localPublic,32))return -EINVAL;
    // Any failure after pause is held. No implicit resume or retry on an
    // uncertain result; storage's nonrecursive Lease ends before hold/close.
    struct Paused {PeerAdministrationContext &c;bool restored=false;~Paused(){if(!restored)c.hold();}} paused{context_};
    if(!context_.pause())return -EBUSY;
    int rc=storage_.prepareFormerCode(store,eraser,progress,local,command.generation,command.crc,out);
    if(rc){out.complete=false;return rc;}
    auto failure=[&](int error){out.complete=false;storage_.preparationRestoreFailed(error);return error;};
    if(!context_.current(epoch)||storage_.state()!=PkiStorage::State::Active)return failure(-EACCES);
    if(!context_.resume())return failure(-EIO);
    if(!context_.current(epoch)||!context_.ready())return failure(-EACCES);
    paused.restored=true;return 0;
  }
#endif
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
  int enableStorage(const UsbStorageCommand &command,uint32_t epoch,bool bounded=false){
    return enableStorageMode(command,epoch,bounded?1u:0u);
  }
#else
  int enableStorage(const UsbStorageCommand &,uint32_t,bool=false){return -ENOTSUP;}
#endif
  int enableContacts(const UsbStorageCommand &command,uint32_t epoch){
    return enableStorageMode(command,epoch,2u);
  }
  int add(const UsbPeerCommand &command,uint32_t epoch){
    if(!context_.tryEnter())return -EBUSY;
    struct Leave {PeerAdministrationContext &context;~Leave(){context.leave();}} leave{context_};
    if(busy_)return -EBUSY;
    busy_=true;struct End {bool &busy;~End(){busy=false;}} end{busy_};
    if(!context_.current(epoch)||!context_.ready())return -EACCES;
    IdentityBinding local;PkiPeerPin pin;PkiPeerSet checked;
    pin.node=command.peerNode;memcpy(pin.publicKey,command.peerPublic,32);
    if(!storage_.preparedBinding(local)||local.node!=command.localNode||
       memcmp(local.publicKey,command.localPublic,32)||!checked.assign(local,&pin,1)||
       !context_.peerAllowed(local,pin))return -EINVAL;
    if(!context_.pause())return -EBUSY;
    if(!context_.current(epoch)){
      if(!context_.resume())context_.hold();
      return -EACCES;
    }
    const int result=storage_.addTrustedPeer(local,pin);
    if(!context_.current(epoch)||storage_.state()!=PkiStorage::State::Active){
      context_.hold();return result<0?result:-EIO;
    }
    if(!context_.resume()){context_.hold();return -EIO;}
    if(!context_.current(epoch)){context_.hold();return -EACCES;}
    return result;
  }
};
}
