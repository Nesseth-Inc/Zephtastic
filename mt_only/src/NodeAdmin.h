// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Service.h"
#include "NodeJournal.h"

namespace mt {
// Called under serviceLock by authorized NodeDB administration. This helper
// is shared by native firmware and tests so failures exercise the real RF gate.
struct NodeAdmin {
  // The policy write is already verified and live, but RX restoration failed.
  // Callers must report the mutation as committed and the interface as degraded;
  // treating this as an ordinary failed edit can reverse a non-idempotent retry.
  static constexpr int CommittedRxFault=1;
  using Removed=void (*)(uint32_t,void *);
private:
  static int restore(Service &service,bool receiving,bool armed,bool relaying,uint32_t now){
    if(!receiving)return armed?-EIO:0;
    // Normal mode derives relay intent from the persisted role; its boolean
    // start argument is only a TX gate. Legacy mode uses that argument itself.
    const bool relayArgument=service.normalMode()?false:relaying;
    if(service.faulted()||!service.config.provisioned()||!service.start(relayArgument,now)){
      service.stop();return -EIO;
    }
    if(!service.armEndpoint(armed)||service.faulted()||!service.receiving()||
       service.endpointArmed()!=armed||service.relaying()!=relaying){
      service.stop();return -EIO;
    }
    return 0;
  }
public:
  // Admission precedes any withdrawal of the selected archived row. Its journal
  // fence and metadata write share the same storage pause.
  template<class Context,class Commit>
  static int promoteFavorite(Context &context,Commit commit){
    if(!context.pause())return -EBUSY;
    const int rc=commit();
    if(rc){context.hold();return rc;}
    if(!context.resume()){context.hold();return CommittedRxFault;}
    return 0;
  }
  static int edit(Service &service,Radio &radio,NodeJournal &journal,SettingsBackend &settings,
                  uint32_t id,NodeCache::Edit op,uint32_t now,bool blocked=false,Removed removed=nullptr,void *context=nullptr,
                  NodeJournal::BeforeEdit before=nullptr){
    if(!journal.ready()||!service.nodes.ready())return -EACCES;
    if(id<=1||id>=0xfffffffeu||id==service.route.identity())return -EINVAL;
    if(blocked)return -EBUSY;
    if(service.faulted())return -EIO;
    if(!service.storageStopSafe()||service.active()||
       service.endpointFree()!=(service.normalMode()?4u:1u))return -EBUSY;
#if defined(CONFIG_MT_RF_ADMIN_GET)
    // revokePrivatePeer cancels peer GET correlations before its own pause.
    // Do not let local edits/removal cancel accepted work that has not completed.
    if(service.privateAdminQueriesPending()||service.privateAdminMutationQueued())return -EBUSY;
#endif
    if(!radio.ready())return -EIO;
    if(radio.busy())return -EBUSY;
    if((service.receiving()&&radio.health()!=1)||(!service.receiving()&&radio.health()!=0))return -EIO;
    // Ignore is a reversible local filter; it must not destroy the trusted key
    // domain. Remove is the explicit durable trust retirement operation. Its
    // private provider owns its own pause, so revoke before the NodeJournal
    // storage pause rather than nesting private-administration pauses.
    if(op==NodeCache::Edit::Remove&&!service.revokePrivatePeer(id))return -EIO;
    if(service.faulted())return -EIO;
    if(!service.storageStopSafe()||service.active()||radio.busy())return -EBUSY;
    if(!radio.ready()||(service.receiving()?radio.health()!=1:radio.health()!=0))return -EIO;

    // Peer retirement can deliberately leave a legacy one-peer provider
    // dormant and RF-off. Capture the post-retirement state for restoration.
    const bool receiving=service.receiving();
    const bool armed=service.endpointArmed();
    const bool relaying=service.relaying();
    if(armed&&!receiving)return -EIO;
    const int paused=service.pauseForStorage();
    if(paused)return paused==-EBUSY&&!service.faulted()?-EBUSY:-EIO;
    if(!journal.ready()||!service.nodes.ready()||service.faulted()||
       !service.storageQuiescent()||!radio.ready()||radio.busy()||radio.health()!=0){
      service.stop();return -EIO;
    }
    bool committed=false;
    const int ret=journal.edit(settings,service.nodes,id,op,now,true,&committed,before);
    // A valid missing ID is a durable no-op but may still have ephemeral
    // authority/dedup state. Clear only after verified success, before RX.
    if(!ret&&op==NodeCache::Edit::Remove){
      service.signature.forgetPublicKey(id);
      service.route.forgetRoute(id);
      service.forgetReceivedBeaconSource(id);
      if(removed)removed(id,context);
    }
    // Failed or uncertain storage freezes policy, so never resume RF. A
    // verified edit restores the exact relay/receive/arm intent captured after
    // any peer-specific retirement; no-authority dormant providers stay off.
    if(!journal.ready()||!service.nodes.ready())return -EIO;
    if(service.faulted()||!service.storageQuiescent()||!radio.ready()||radio.busy()||radio.health()!=0){
      service.stop();return committed?CommittedRxFault:-EIO;
    }
    if(restore(service,receiving,armed,relaying,now))return committed?CommittedRxFault:-EIO;
    return ret;
  }

  // Each peer retirement uses the same verified transaction as Remove. A
  // busy or uncertain retirement stops the reset before any success reply.
  template<typename Finalize>
  static int reset(Service &service,Radio &radio,NodeJournal &journal,SettingsBackend &settings,
                   bool keepFavorites,uint32_t now,Removed removed,Finalize finalize,
                   NodeJournal::BeforeEdit before=nullptr) {
    if(!journal.ready()||!service.nodes.ready()||service.faulted()||!radio.ready())return -EIO;
    const auto role=service.config.role();
    keepFavorites=keepFavorites||role==DeviceRolePolicy::Role::CLIENT_BASE||
      role==DeviceRolePolicy::Role::ROUTER||role==DeviceRolePolicy::Role::ROUTER_LATE;
    for(unsigned i=service.nodes.count();i>0;--i){
      const auto *entry=service.nodes.get(i-1);
      if(keepFavorites&&entry->favorite())continue;
      const int result=edit(service,radio,journal,settings,entry->id(),NodeCache::Edit::Remove,now,false,removed,nullptr,before);
      if(result)return result;
    }
    if(!service.storageStopSafe()||service.active()||radio.busy()||
       service.endpointFree()!=(service.normalMode()?4u:1u))return -EBUSY;
#if defined(CONFIG_MT_RF_ADMIN_GET)
    if(service.privateAdminQueriesPending()||service.privateAdminMutationQueued())return -EBUSY;
#endif
    const bool receiving=service.receiving(),armed=service.endpointArmed(),relaying=service.relaying();
    if(radio.health()!=(receiving?1:0))return -EIO;
    const int paused=service.pauseForStorage();
    if(paused)return paused;
    if(!service.storageQuiescent()||!radio.ready()||radio.busy()||radio.health()!=0)return -EIO;
    const int flushed=journal.flush(settings,service.nodes,now,true,true);
    if(flushed<0)return flushed;
    const int result=finalize();
    if(result)return result;
    return restore(service,receiving,armed,relaying,now);
  }
};
}
