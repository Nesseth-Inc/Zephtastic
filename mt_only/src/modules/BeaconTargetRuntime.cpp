// SPDX-License-Identifier: GPL-3.0-only
#include "BeaconTargetRuntime.h"

#include "../Service.h"
#include "../ModuleAnnouncement.h"
#include "../RadioTiming.h"
#include "../RegionRuntimePolicy.h"
#include "../PublicApplication.h"
#include <math.h>
#include <string.h>

namespace mt::modules {
BeaconExcursion::RadioProfile BeaconTargetRuntime::runningProfile(const Config &config)
{
  auto request=config.radioRequest();
  request.overrideMHz=0;
  LoRaPresets::Profile modem{};USChannelSelection selection{};
  BeaconExcursion::RadioProfile profile{};
  profile.preset=request.preset;profile.region=request.regionCode;profile.usePreset=request.usePreset;
  if(RadioSettings::resolve(request,modem,selection,config.effectivePrimaryName(),config.modemName(),
                            config.txIntent()==Config::TxIntent::Disabled))
    profile.slot=selection.slot+1u;
  return profile;
}

BeaconTargetRuntime::Row *BeaconTargetRuntime::row(uint32_t id)
{for(auto &r:rows_)if(r.used&&r.id==id)return &r;return nullptr;}
const BeaconTargetRuntime::Row *BeaconTargetRuntime::row(uint32_t id)const
{for(const auto &r:rows_)if(r.used&&r.id==id)return &r;return nullptr;}

bool BeaconTargetRuntime::targetSupported(const BeaconExcursion::RadioProfile &profile)
{
  Config candidate{};
  if(!configureProfile(profile,candidate))return false;
  PrimaryChannel::Selection key;
  if(!candidate.channelSelection(profile.channelIndex,key))return false;
  const bool cryptoReady=(service_.private_?service_.publicReady():key.publicDefault)&&
      service_.sharedOriginAllowed(candidate,profile.channelIndex);
  PrimaryChannel::clear(key);
  if(!cryptoReady)return false;
  const auto home=runningProfile(service_.config);
  const bool same=profile.sameRadio(home);
  return same||service_.radio_.beaconIsolationSupported();
}

bool BeaconTargetRuntime::configureProfile(const BeaconExcursion::RadioProfile &profile,Config &candidate)
{
  if(!profile.region||profile.channelIndex>=8||radio::RegionPolicy::fixedReceiveOnly(profile.region))return false;
  candidate=service_.config;
  if(profile.sameRadio(runningProfile(candidate)))return candidate.provisioned();
  auto request=candidate.radioRequest();
  request.regionCode=profile.region;request.preset=profile.preset;request.usePreset=profile.usePreset;
  request.channel=profile.slot;request.overrideMHz=0;
  if(!candidate.setRadio(request))return false;
  LoRaPresets::Profile modem{};USChannelSelection channel{};
  return candidate.valid()&&candidate.provisioned()&&candidate.resolveRadio(modem,channel)&&
         channel.regionCode==profile.region;
}

bool BeaconTargetRuntime::encode(const BeaconExcursion::Request &request,const uint8_t *data,
    size_t size,uint32_t random,uint32_t now,bench::Frame &out)
{
  (void)random;(void)now;
  bool telemetry=false;
  if(!ModuleAnnouncement::canonical(data,size,telemetry,true)||!request.packetId||
     request.packetId>=0xfffffffeu)return false;
  Config candidate{};if(!configureProfile(request.target,candidate))return false;
  PrimaryChannel::Selection key;
  if(!candidate.channelSelection(request.target.channelIndex,key))return false;
  if(!service_.private_&&!key.publicDefault){PrimaryChannel::clear(key);return false;}
  bench::MtCodec targetCodec;
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  if(service_.codec.originsRestricted())targetCodec.restrictOrigins(service_);
#endif
  targetCodec.setPublicSigning(candidate.packetSignaturePolicy()!=0);
  const bool keyed=key.licensedPlaintext?targetCodec.setLicensedPlaintext(key.hash):
      targetCodec.setKey(key.hash,key.bytes,key.length,key.aead);
  PrimaryChannel::clear(key);
  if(!keyed)return false;
  bool encoded=false;
  if(service_.private_) {
    if(!service_.publicReady()||!service_.pausePrivate())return false;
    encoded=service_.private_->broadcast(targetCodec,request.packetId,data,size,out);
    const bool resumed=service_.resumePrivate();encoded=encoded&&resumed;
  } else {
    size_t frameSize=0;
    encoded=targetCodec.encodeZeroHopData(service_.route.identity(),UINT32_MAX,request.packetId,
       data,size,out.bytes,sizeof(out.bytes),frameSize);
    if(encoded)out.len=frameSize;
  }
  if(!encoded)return false;
  out.bytes[12]=request.legacyHopOverride?0x20u:0u;return true;
}

bool BeaconTargetRuntime::enqueue(const BeaconExcursion::Request &input,const uint8_t *data,
    size_t size,uint32_t random,uint32_t now,uint32_t *acceptedId)
{
  if(acceptedId)*acceptedId=0;
  if(!service_.normalMode_||!service_.receiving_||!service_.endpointArmed_||
     !service_.txAllowed_||service_.fault_||service_.active_||service_.own_||
     service_.normalActive_||service_.normalTransitActive_||service_.endpointActive_||
     !service_.endpoint_.idle()||service_.route.pending()||service_.route.retransmitting()||
     service_.radio_.busy()||input.identityEpoch!=service_.route.identity()||
     (input.requiresRetune&&!service_.radio_.beaconIsolationSupported()))
    return false;
  size_t free=Rows;for(size_t i=0;i<Rows;++i)if(!rows_[i].used){free=i;break;}
  if(free==Rows)return false;
  auto request=input;
  if(request.target.channelIndex>=8)return false;
  const bool retuneRequired=!request.target.sameRadio(runningProfile(service_.config));
  if(request.requiresRetune!=retuneRequired)return false;
  if(!request.packetId)request.packetId=service_.endpoint_.generatedId(random);
  if(!request.packetId)return false;
  if(!targetSupported(request.target))return false;
  bench::Frame frame{};
  if(!encode(request,data,size,random,now,frame)) {bench::wipeKeyBuffer(&frame,sizeof(frame));return false;}
  if(!service_.endpoint_.reserveExternalId(request.packetId)) {
    bench::wipeKeyBuffer(&frame,sizeof(frame));return false;
  }
  if(!excursion_.enqueue(request)){bench::wipeKeyBuffer(&frame,sizeof(frame));return false;}
  rows_[free].id=request.packetId;rows_[free].frame=frame;rows_[free].used=true;
  rows_[free].enqueuedAt=now;rows_[free].sequence=nextSequence_++;
  if(!nextSequence_)nextSequence_=1;
  rows_[free].request=request;
  bench::wipeKeyBuffer(&frame,sizeof(frame));
  if(acceptedId)*acceptedId=request.packetId;
  return true;
}

void BeaconTargetRuntime::expireQueued(uint32_t now,uint32_t settingsEpoch,uint32_t identityEpoch)
{
  // The core and payload table preserve FIFO order; use the oldest sequence,
  // not the first physical row, because released slots are reused.
  for(;;) {
    Row *oldest=nullptr;
    for(auto &candidate:rows_)if(candidate.used&&candidate.id!=activeId_&&
       (!oldest||int32_t(candidate.sequence-oldest->sequence)<0))oldest=&candidate;
    if(!oldest||!bench::due(now,oldest->enqueuedAt+60000u))return;
    (void)excursion_.cancel(oldest->id,settingsEpoch,identityEpoch);
    release(oldest->id);
  }
}

bool BeaconTargetRuntime::admission(const Config &target,bool polite)
{
  if(!target.regionCode())return false;
  const auto observed=service_.channel_.percent();
  if(!service_.channel_.diagnosticSupported()||!observed||!isfinite(*observed)||*observed<0||*observed>100)
    return false;
  if(TxAdmission::channel(true,*observed,polite)!=TxAdmission::Decision::Allow)return false;
  const auto tx=service_.airtime_.percent();const auto role=target.role();
  const bool fixed=role==meshtastic_Config_DeviceConfig_Role_ROUTER||
      role==meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
  return radio::RegionRuntimePolicy::duty(target.regionCode(),fixed,polite,tx.has_value(),tx?*tx:NAN)==
      TxAdmission::Decision::Allow;
}

bool BeaconTargetRuntime::retune(const BeaconExcursion::RadioProfile &profile)
{
  Config target{};if(!configureProfile(profile,target))return false;
  if(service_.active_||service_.own_||service_.endpointQueued_||service_.endpointActive_||
     service_.normalActive_||service_.normalTransitActive_||!service_.endpoint_.idle()||
     service_.route.pending()||service_.route.retransmitting()||service_.radio_.busy())return false;
  homeArmed_=service_.endpointArmed_;homeRelaying_=service_.relaying_;
  const int stopped=service_.pauseForStorage();if(stopped)return false;
  retuned_=true;activeTarget_=profile;
  service_.receiving_=false;service_.relaying_=false;service_.endpointArmed_=false;
  service_.radio_.beaconRxIsolation(true);
  const int configured=service_.radio_.configure(target);
  if(configured){lastError_=configured;(void)restoreHome();return false;}
  const int started=service_.radio_.start();if(started){lastError_=started;(void)restoreHome();return false;}
  if(service_.radio_.health()!=1){lastError_=-EIO;(void)restoreHome();return false;}
  return true;
}

bool BeaconTargetRuntime::restoreHome()
{
  if(!retuned_)return true;
  if(service_.radio_.busy())return false;
  int result=service_.radio_.stopForStorage();if(result)return false;
  if(service_.radio_.health()!=0)return false;
  result=service_.radio_.configure(service_.config);if(result)return false;
  service_.radio_.beaconRxIsolation(false);
  result=service_.radio_.start();if(result)return false;
  if(service_.radio_.health()!=1)return false;
  service_.receiving_=true;service_.relaying_=homeRelaying_;service_.endpointArmed_=homeArmed_;
  retuned_=false;return true;
}

void BeaconTargetRuntime::release(uint32_t id)
{
  if(auto *r=row(id)){bench::wipeKeyBuffer(&r->frame,sizeof(r->frame));*r=Row{};}
  if(activeId_==id) {
    bench::wipeKeyBuffer(&activeFrame_,sizeof(activeFrame_));activeFrame_={};activeId_=0;
    txActive_=false;deadline_=0;completionAwaitingRestore_=false;pendingTxSuccess_=false;
    waitingForAdmission_=false;activeSince_=0;activeSinceSet_=false;
  }
}

void BeaconTargetRuntime::apply(const BeaconExcursion::Action &action,uint32_t now,
    uint32_t settingsEpoch,uint32_t identityEpoch)
{
  lastError_=0;
  switch(action.kind) {
  case BeaconExcursion::ActionKind::DropPacket:
    release(action.packetId);return;
  case BeaconExcursion::ActionKind::ConfigureTarget: {
    if(!activeSinceSet_){activeSince_=now;activeSinceSet_=true;activeId_=action.packetId;}
    auto gate=excursion_.beginConfigure(action.packetId,settingsEpoch,identityEpoch);
    const bool ok=gate.kind==BeaconExcursion::ActionKind::ConfigureTarget&&retune(gate.profile);
    if(!ok){lastError_=lastError_?lastError_:-EIO;if(retuned_&&!restoreHome())service_.fault_=true;}
    apply(excursion_.targetConfigured(action.packetId,ok,settingsEpoch,identityEpoch),now,settingsEpoch,identityEpoch);
    return;
  }
  case BeaconExcursion::ActionKind::StartTransmission: {
    if(!activeSinceSet_){activeSince_=now;activeSinceSet_=true;activeId_=action.packetId;}
    const auto *r=row(action.packetId);Config txConfig{};
    if(!r||!configureProfile(r->request.target,txConfig)||!admission(txConfig,false)){
      // Stay in ReadyToTransmit and re-evaluate on the next owner poll. This
      // retains the immutable frame while channel/duty measurements recover.
      lastError_=-EAGAIN;waitingForAdmission_=true;pendingAction_=action;pendingActionReady_=true;return;
    }
    waitingForAdmission_=false;
    const auto gate=excursion_.beginTransmit(action.packetId,settingsEpoch,identityEpoch);
    if(gate.kind!=BeaconExcursion::ActionKind::StartTransmission){apply(gate,now,settingsEpoch,identityEpoch);return;}
    if(!service_.sharedOriginAllowed(txConfig,r->request.target.channelIndex)) {
      lastError_=-EACCES;
      apply(excursion_.transmitFailed(action.packetId,settingsEpoch,identityEpoch),now,settingsEpoch,identityEpoch);return;
    }
    activeFrame_=r->frame;activeId_=action.packetId;
    const uint32_t air=service_.radio_.airtime(activeFrame_.len);RadioTiming::Deadline timing{};
    if(!RadioTiming::resolve(air,timing)){lastError_=-EMSGSIZE;apply(excursion_.transmitFailed(activeId_,settingsEpoch,identityEpoch),now,settingsEpoch,identityEpoch);return;}
    const int sent=service_.radio_.send(activeFrame_.bytes,activeFrame_.len);
    if(sent){
      lastError_=sent;pendingTxSuccess_=false;
      const int restored=service_.radio_.restore();
      if(restored||service_.radio_.health()!=1){completionAwaitingRestore_=true;service_.fault_=true;return;}
      apply(excursion_.transmitFailed(activeId_,settingsEpoch,identityEpoch),now,settingsEpoch,identityEpoch);return;
    }
    txActive_=true;deadline_=now+timing.hostMs;service_.noteTxAcceptedStart(air);return;
  }
  case BeaconExcursion::ActionKind::RestoreHome:
  case BeaconExcursion::ActionKind::ApplyCurrentSettings: {
    const bool ok=restoreHome();if(!ok){lastError_=-EIO;return;}
    const auto next=excursion_.restoreCompleted(true,settingsEpoch,identityEpoch);
    if(next.kind==BeaconExcursion::ActionKind::None)release(action.packetId);
    else apply(next,now,settingsEpoch,identityEpoch);return;
  }
  case BeaconExcursion::ActionKind::SendNormally:
  case BeaconExcursion::ActionKind::HoldPacket:
  case BeaconExcursion::ActionKind::None:
  default:return;
  }
}

void BeaconTargetRuntime::poll(uint32_t now,uint32_t settingsEpoch,uint32_t identityEpoch,bool mainReady)
{
  if(!mainReady||identityEpoch!=service_.route.identity()) {
    cancelAll(settingsEpoch,identityEpoch);
    if(!txActive_&&!excursion_.holdsRadio())return;
  }
  expireQueued(now,settingsEpoch,identityEpoch);
  if(excursion_.holdsRadio()&&!txActive_&&waitingForAdmission_&&activeSinceSet_&&
     bench::due(now,activeSince_+5000u)) {
    waitingForAdmission_=false;
    pendingAction_={};pendingActionReady_=false;
    apply(excursion_.cancel(activeId_,settingsEpoch,identityEpoch),now,settingsEpoch,identityEpoch);
    return;
  }
  if(pendingActionReady_) {
    const auto action=pendingAction_;pendingAction_={};pendingActionReady_=false;
    apply(action,now,settingsEpoch,identityEpoch);return;
  }
  if(txActive_) {
    int result=0;
    if(!service_.radio_.completion(result)) {
      if(bench::due(now,deadline_)){service_.noteTxUncertain();lastError_=-ETIMEDOUT;service_.fault_=true;return;}
      return;
    }
    txActive_=false;
    pendingTxSuccess_=result==0;
    if(result){service_.noteTxUncertain();lastError_=result;}
    else service_.creditTxPending();
    const int restored=service_.radio_.restore();
    if(restored||service_.radio_.health()!=1){
      service_.fault_=true;lastError_=restored?restored:-EIO;completionAwaitingRestore_=true;return;
    }
    const uint32_t completedId=activeId_;
    const auto next=pendingTxSuccess_?excursion_.transmitCompleted(completedId,settingsEpoch,identityEpoch):
      excursion_.transmitFailed(completedId,settingsEpoch,identityEpoch);
    completionAwaitingRestore_=false;
    if(next.kind==BeaconExcursion::ActionKind::StartTransmission&&next.packetId!=completedId)
      release(completedId);
    apply(next,now,settingsEpoch,identityEpoch);
    return;
  }
  if(excursion_.holdsRadio()) {
    if(completionAwaitingRestore_&&!service_.radio_.busy()) {
      const int restored=service_.radio_.restore();
      if(!restored&&service_.radio_.health()==1) {
        completionAwaitingRestore_=false;
        const auto next=pendingTxSuccess_?excursion_.transmitCompleted(activeId_,settingsEpoch,identityEpoch):
          excursion_.transmitFailed(activeId_,settingsEpoch,identityEpoch);
        apply(next,now,settingsEpoch,identityEpoch);return;
      }
      lastError_=restored?restored:-EIO;return;
    }
    if(lastError_&&restoreHome()) {
      apply(excursion_.restoreCompleted(true,settingsEpoch,identityEpoch),now,settingsEpoch,identityEpoch);
    }
    return;
  }
  if(!excursion_.queued()||service_.radio_.busy()||!service_.receiving_||service_.fault_)return;
  // Queued rows are offered in the same insertion order as the core queue.
  for(const auto &r:rows_)if(r.used) {
    const auto home=runningProfile(service_.config);
    apply(excursion_.beforeTransmit(r.id,home,settingsEpoch,identityEpoch),now,settingsEpoch,identityEpoch);
    return;
  }
}

void BeaconTargetRuntime::receiveAccepted()
{
  const auto action=excursion_.receiveAccepted();
  if(action.kind!=BeaconExcursion::ActionKind::None&&action.kind!=BeaconExcursion::ActionKind::HoldPacket) {
    pendingAction_=action;pendingActionReady_=true;
  }
}

void BeaconTargetRuntime::cancelAll(uint32_t settingsEpoch,uint32_t identityEpoch)
{
  pendingAction_={};pendingActionReady_=false;
  for(auto &r:rows_)if(r.used) {
    const auto action=excursion_.cancel(r.id,settingsEpoch,identityEpoch);
    if(action.kind==BeaconExcursion::ActionKind::RestoreHome||
       action.kind==BeaconExcursion::ActionKind::ApplyCurrentSettings) {
      if(txActive_)continue;
      if(restoreHome())(void)excursion_.restoreCompleted(true,settingsEpoch,identityEpoch);
    }
  }
  if(!txActive_&&!excursion_.holdsRadio())for(auto &r:rows_)if(r.used)release(r.id);
}

} // namespace mt::modules
