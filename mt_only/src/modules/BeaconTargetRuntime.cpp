// SPDX-License-Identifier: GPL-3.0-only
#include "BeaconTargetRuntime.h"

#include "../Service.h"
#include "../ModuleAnnouncement.h"
#include "../RadioTiming.h"
#include "../RegionRuntimePolicy.h"
#include "../PublicApplication.h"
#include <math.h>
#ifdef __ZEPHYR__
#include <zephyr/random/random.h>
#endif
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
{for(auto &r:rows_)if(id&&r.id==id)return &r;return nullptr;}
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

bool BeaconTargetRuntime::configureProfile(const BeaconExcursion::RadioProfile &profile,Config &candidate,uint32_t *slotMs)
{
  if(!profile.region||profile.channelIndex>=8||radio::RegionPolicy::fixedReceiveOnly(profile.region))return false;
  candidate=service_.config;
  if(!profile.sameRadio(runningProfile(candidate))) {
    auto request=candidate.radioRequest();
    request.regionCode=profile.region;request.preset=profile.preset;request.usePreset=profile.usePreset;
    request.channel=profile.slot;request.overrideMHz=0;
    // The settings editor may normalize an oversized US channel. A resolved
    // beacon target must retain the exact channel chosen by the broadcaster.
    if(!candidate.setRadio(request)||candidate.channelNumber()!=profile.slot)return false;
  } else if(!slotMs) return candidate.provisioned();
  LoRaPresets::Profile modem{};USChannelSelection channel{};
  return candidate.provisioned()&&candidate.resolveRadio(modem,channel)&&channel.regionCode==profile.region&&
         (!slotMs||RadioSlot::resolve(modem.bandwidthHz,modem.spreadingFactor,*slotMs));
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

bool BeaconTargetRuntime::ownerIdle()const
{
  return service_.normalMode_&&service_.receiving_&&service_.endpointArmed_&&service_.txAllowed_&&
         service_.storageStopSafe()&&!service_.normalTransitActive_&&!service_.radio_.busy()&&
         !service_.radio_.receiveEventPending();
}

bool BeaconTargetRuntime::homeRxPending()const
{
  return !retuned_&&!txActive_&&!completionAwaitingRestore_&&service_.radio_.receiveEventPending();
}

bool BeaconTargetRuntime::enqueue(const BeaconExcursion::Request &input,const uint8_t *data,
    size_t size,uint32_t random,uint32_t now,uint32_t *acceptedId)
{
  if(acceptedId)*acceptedId=0;
  if(!ownerIdle()||input.identityEpoch!=service_.route.identity()||
     (input.requiresRetune&&!service_.radio_.beaconIsolationSupported()))
    return false;
  size_t free=Rows;for(size_t i=0;i<Rows;++i)if(!rows_[i].id){free=i;break;}
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
  rows_[free].id=request.packetId;rows_[free].frame=frame;
  rows_[free].enqueuedAt=now;
  rows_[free].target=request.target;
  bench::wipeKeyBuffer(&frame,sizeof(frame));
  if(acceptedId)*acceptedId=request.packetId;
  return true;
}

void BeaconTargetRuntime::expireQueued(uint32_t now,uint32_t settingsEpoch,uint32_t identityEpoch)
{
  while(auto *oldest=row(excursion_.frontId())) {
    if(!bench::due(now,oldest->enqueuedAt+60000u))return;
    (void)excursion_.cancel(oldest->id,settingsEpoch,identityEpoch);
    release(oldest->id);
  }
}

bool BeaconTargetRuntime::admission(uint32_t region)
{
  if(!region)return false;
  const auto observed=service_.channel_.percent();
  if(!service_.channel_.diagnosticSupported()||!observed||!isfinite(*observed)||*observed<0||*observed>100)
    return false;
  if(TxAdmission::channel(true,*observed,false)!=TxAdmission::Decision::Allow)return false;
  const auto tx=service_.airtime_.percent();const auto role=service_.config.role();
  const bool fixed=role==meshtastic_Config_DeviceConfig_Role_ROUTER||
      role==meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
  return radio::RegionRuntimePolicy::duty(region,fixed,false,tx.has_value(),tx?*tx:NAN)==
      TxAdmission::Decision::Allow;
}

bool BeaconTargetRuntime::retune(const BeaconExcursion::RadioProfile &profile)
{
  Config target{};if(!configureProfile(profile,target,&slotMs_))return false;
  homeArmed_=service_.endpointArmed_;homeRelaying_=service_.relaying_;
  const int stopped=service_.pauseForStorage();if(stopped)return false;
  retuned_=true;
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
  if(auto *r=row(id))bench::wipeKeyBuffer(r,sizeof(*r));
  if(activeId_==id) {
    activeId_=0;
    txActive_=false;deadline_=0;completionAwaitingRestore_=false;pendingTxSuccess_=false;
    waitingForAdmission_=false;activeSince_=0;
  }
}

uint32_t BeaconTargetRuntime::defaultDraw()
{
#ifdef __ZEPHYR__
  return sys_rand32_get();
#else
  return 0; // Native fixtures can inject a deterministic sequence.
#endif
}

// RadioInterface::getTxDelayMsec draws a local (unweighted) contention
// window. The hardware send performs a fresh CAD at the expiry on this modem.
void BeaconTargetRuntime::contend(const BeaconExcursion::Action &action,uint32_t now)
{
  const auto utilization=service_.channel_.percent();
  const unsigned percent=utilization&&*utilization>=0&&*utilization<=100 ? unsigned(*utilization) : 100u;
  const unsigned cw=RebroadcastTiming::CWmin+percent*(RebroadcastTiming::CWmax-RebroadcastTiming::CWmin)/100u;
  const uint32_t draw=draw_();
  deadline_=now+(draw&((1u<<cw)-1u))*slotMs_;
  pendingAction_=action;pendingActionReady_=true;
}

void BeaconTargetRuntime::finishTransmit(uint32_t now,uint32_t settingsEpoch,uint32_t identityEpoch)
{
  const int restored=service_.radio_.restore();
  if(restored||service_.radio_.health()!=1){
    completionAwaitingRestore_=true;lastError_=restored?restored:-EIO;service_.fault_=true;return;
  }
  completionAwaitingRestore_=false;
  const uint32_t id=activeId_;
  // A hard fault stays latched. Finish the held packet and restore home
  // without starting the next member of a split group.
  if(service_.fault_)excursion_.cancel(id,settingsEpoch,identityEpoch);
  const auto next=pendingTxSuccess_?excursion_.transmitCompleted(id,settingsEpoch,identityEpoch):
      excursion_.transmitFailed(id,settingsEpoch,identityEpoch);
  apply(next,now,settingsEpoch,identityEpoch);
}

void BeaconTargetRuntime::apply(const BeaconExcursion::Action &action,uint32_t now,
    uint32_t settingsEpoch,uint32_t identityEpoch)
{
  lastError_=0;
  switch(action.kind) {
  case BeaconExcursion::ActionKind::DropPacket:
    release(action.packetId);return;
  case BeaconExcursion::ActionKind::ConfigureTarget: {
    if(!activeId_){activeSince_=now;activeId_=action.packetId;}
    auto gate=excursion_.beginConfigure(action.packetId,settingsEpoch,identityEpoch);
    const bool ok=gate.kind==BeaconExcursion::ActionKind::ConfigureTarget&&retune(gate.profile);
    if(!ok){lastError_=lastError_?lastError_:-EIO;if(retuned_&&!restoreHome())service_.fault_=true;}
    const auto next=excursion_.targetConfigured(action.packetId,ok,settingsEpoch,identityEpoch);
    if(next.kind==BeaconExcursion::ActionKind::StartTransmission)contend(next,now);
    else apply(next,now,settingsEpoch,identityEpoch);
    return;
  }
  case BeaconExcursion::ActionKind::StartTransmission: {
    if(activeId_&&activeId_!=action.packetId)release(activeId_); // completed split-group member
    if(!activeId_){activeSince_=now;activeId_=action.packetId;}
    auto *r=row(action.packetId);Config txConfig{};
    if(!r||!configureProfile(r->target,txConfig,&slotMs_)||!admission(txConfig.regionCode())){
      // Stay in ReadyToTransmit and re-evaluate on the next owner poll. This
      // retains the immutable frame while channel/duty measurements recover.
      lastError_=-EAGAIN;waitingForAdmission_=true;deadline_=now;pendingAction_=action;pendingActionReady_=true;return;
    }
    if(service_.radio_.busy()||homeRxPending()){waitingForAdmission_=true;contend(action,now);return;}
    waitingForAdmission_=false;
    const auto gate=excursion_.beginTransmit(action.packetId,settingsEpoch,identityEpoch);
    if(gate.kind!=BeaconExcursion::ActionKind::StartTransmission){apply(gate,now,settingsEpoch,identityEpoch);return;}
    if(!service_.sharedOriginAllowed(txConfig,r->target.channelIndex)) {
      lastError_=-EACCES;
      apply(excursion_.transmitFailed(action.packetId,settingsEpoch,identityEpoch),now,settingsEpoch,identityEpoch);return;
    }
    activeId_=action.packetId;
    const uint32_t air=service_.radio_.airtime(r->frame.len);RadioTiming::Deadline timing{};
    if(!RadioTiming::resolve(air,timing)){lastError_=-EMSGSIZE;apply(excursion_.transmitFailed(activeId_,settingsEpoch,identityEpoch),now,settingsEpoch,identityEpoch);return;}
    const int sent=service_.radio_.send(r->frame.bytes,r->frame.len);
    if(sent==-EBUSY&&service_.radio_.health()==1){
      excursion_.deferTransmit(activeId_);
      waitingForAdmission_=true;
      contend(action,now);return;
    }
    if(sent){
      lastError_=sent;pendingTxSuccess_=false;
      finishTransmit(now,settingsEpoch,identityEpoch);return;
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
    if(!radioHeld())return;
  }
  expireQueued(now,settingsEpoch,identityEpoch);
  if(pendingActionReady_&&pendingAction_.kind==BeaconExcursion::ActionKind::StartTransmission&&
     bench::due(now,activeSince_+5000u)) {
    const auto *r=row(activeId_);
    if(waitingForAdmission_||!r||!admission(r->target.region)) {
      waitingForAdmission_=false;pendingActionReady_=false;
      apply(excursion_.cancel(activeId_,settingsEpoch,identityEpoch),now,settingsEpoch,identityEpoch);
      return;
    }
  }
  if(pendingActionReady_) {
    if(pendingAction_.kind==BeaconExcursion::ActionKind::StartTransmission&&!bench::due(now,deadline_))return;
    const auto action=pendingAction_;pendingAction_={};pendingActionReady_=false;
    apply(action,now,settingsEpoch,identityEpoch);return;
  }
  if(txActive_||completionAwaitingRestore_) {
    if(txActive_) {
      int result=0;
      if(!service_.radio_.completion(result)) {
        if(bench::due(now,deadline_)){service_.noteTxUncertain();lastError_=-ETIMEDOUT;service_.fault_=true;}
        return;
      }
      txActive_=false;
      pendingTxSuccess_=result==0;
      if(result){service_.noteTxUncertain();lastError_=result;}
      else service_.creditTxPending();
    } else if(service_.radio_.busy())return;
    // A terminal completion releases the driver buffer. Its first RX restore
    // is idempotent even if the modem already reports an active receive.
    finishTransmit(now,settingsEpoch,identityEpoch);return;
  }
  if(excursion_.holdsRadio()||retuned_) {
    apply({BeaconExcursion::ActionKind::RestoreHome,activeId_},now,settingsEpoch,identityEpoch);
    return;
  }
  const auto id=excursion_.frontId();
  if(!id||!ownerIdle())return;
#if defined(CONFIG_MT_RF_ADMIN_GET)
  if(service_.privateAdminWorkQueued()||service_.privateAdminQueriesPending()||service_.legacyAdminWorkQueued())return;
#endif
  apply(excursion_.beforeTransmit(id,runningProfile(service_.config),settingsEpoch,identityEpoch),
        now,settingsEpoch,identityEpoch);
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
  for(auto &r:rows_)if(r.id) {
    const auto action=excursion_.cancel(r.id,settingsEpoch,identityEpoch);
    if(action.kind==BeaconExcursion::ActionKind::None)release(r.id);
    else if(action.kind!=BeaconExcursion::ActionKind::HoldPacket)apply(action,0,settingsEpoch,identityEpoch);
  }
}

} // namespace mt::modules
