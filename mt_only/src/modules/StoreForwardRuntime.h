// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "StoreForwardMemory.h"
#include "StoreForwardControl.h"
#include "StoreForwardSettings.h"
#include "StoreForwardPacket.h"
#include "../Service.h"

namespace mt::modules {
// Serialized by the application owner. No worker, heap, storage writes, direct
// modem access, host sender override, PKI fallback, or claimed replay receipt.
class StoreForwardRuntime {
  StoreForwardMemory memory_;
  alignas(StoreForwardControl) uint8_t controlStorage_[sizeof(StoreForwardControl)];
  StoreForwardControl *control_=nullptr;
  StoreForwardSettings settings_;
  uint32_t epoch_=0;bool configured_=false,enabled_=false,server_=false,psramWasReady_=false;
  StoreForwardPacket pending_;bool pendingReady_=false,finalReplay_=false,pendingOwnsTransfer_=false;
  // One bounded control response may wait behind the immutable packet already
  // staged for the radio service. It is materialized only after that packet
  // leaves the pending slot.
  StoreForwardControl::Action deferredControl_{};bool deferredControlReady_=false;uint8_t deferredChannel_=0;
  uint32_t pendingAt_=0;uint8_t activeChannel_=0;
  struct Seen {uint32_t from=0,id=0,at=0;uint8_t channel=0;};
  Seen seen_[32]{};uint8_t nextSeen_=0;
  uint32_t admitted_=0,rejected_=0,queued_=0,dropped_=0,heartbeatAt_=0,heartbeatPeriod_=900;
  stock::StoreForwardModule::ClientInfo clientInfo_{};
  void reset(){
    clientInfo_={};heartbeatAt_=0;heartbeatPeriod_=900;
    if(control_){control_->~StoreForwardControl();control_=nullptr;}
    memory_.disable();pendingReady_=false;finalReplay_=false;pendingOwnsTransfer_=false;deferredControlReady_=false;
    for(auto &seen:seen_)seen={};nextSeen_=0;configured_=enabled_=server_=false;
  }
  void stage(const StoreForwardControl::Action &action,uint8_t channel,Service &service,uint32_t now){
    if(action.kind==StoreForwardControl::ActionKind::None||
       action.kind==StoreForwardControl::ActionKind::Cancelled||
       action.kind==StoreForwardControl::ActionKind::ReplayFinished)return;
    bool ok=false;
    if(action.kind==StoreForwardControl::ActionKind::ControlMessage)
      ok=StoreForwardPacket::control(service.route.identity(),action.destination,channel,service.config.hopLimit(),action.control,pending_);
    else if(action.kind==StoreForwardControl::ActionKind::ErrorText)
      ok=StoreForwardPacket::error(service.route.identity(),action.destination,channel,service.config.hopLimit(),
        action.errorText==StoreForwardControl::ErrorTextKind::Busy,pending_);
    else if(action.kind==StoreForwardControl::ActionKind::Replay)
      ok=StoreForwardPacket::replay(action.destination,action.replay,pending_);
    const bool ownsTransfer=action.kind==StoreForwardControl::ActionKind::Replay||
      (action.kind==StoreForwardControl::ActionKind::ControlMessage&&control_&&
       action.control.rr==meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY&&control_->activeRequester()==action.destination);
    if(!ok){
      ++dropped_;
      // An unrepresentable long text consumes one stock cursor selection, but
      // must not suppress later encodable history. Summary counts are planned
      // selections, never delivery receipts.
      if(control_&&ownsTransfer&&(action.kind!=StoreForwardControl::ActionKind::Replay||action.finalReplay))control_->finishReplay();
      return;
    }
    pendingReady_=true;pendingAt_=now;finalReplay_=action.finalReplay;pendingOwnsTransfer_=ownsTransfer;
  }
  void deferControl(const StoreForwardControl::Action &action,uint8_t channel){
    if(action.kind!=StoreForwardControl::ActionKind::ControlMessage&&
       action.kind!=StoreForwardControl::ActionKind::ErrorText)return;
    if(deferredControlReady_){++dropped_;return;}
    deferredControl_=action;deferredChannel_=channel;deferredControlReady_=true;
  }
  static bool shape(Service &service,const uint8_t *frame,size_t size,const bench::RouteMetadata &m){
    if(!m.channelDecoded||m.decodeRejected||m.backendFailed||m.signatureRejected||m.signatureQuarantined||
       m.payloadLength>233||m.request_id||(m.portnum!=1&&m.portnum!=65)||
       (m.signatureLength&&(!m.signatureVerified||m.signatureLength!=64)))return false;
    meshtastic_Data data{};data.portnum=static_cast<meshtastic_PortNum>(m.portnum);
    data.payload.size=m.payloadLength;memcpy(data.payload.bytes,m.payload,m.payloadLength);
    data.want_response=m.wantResponse;data.reply_id=m.reply_id;data.emoji=m.emoji;
    data.has_bitfield=m.hasBitfield;data.bitfield=m.bitfield;
    if(m.signatureLength){data.xeddsa_signature.size=m.signatureLength;memcpy(data.xeddsa_signature.bytes,m.signature,m.signatureLength);}
    uint8_t encoded[239];auto stream=pb_ostream_from_buffer(encoded,sizeof(encoded));
    const bool ok=pb_encode(&stream,meshtastic_Data_fields,&data)&&
      service.codec.validateStoreForwardData(frame,size,encoded,stream.bytes_written);
    bench::wipeKeyBuffer(&data,sizeof(data));bench::wipeKeyBuffer(encoded,sizeof(encoded));return ok;
  }
public:
  ~StoreForwardRuntime(){reset();}
  StoreForwardRuntime()=default;
  StoreForwardRuntime(const StoreForwardRuntime &)=delete;
  StoreForwardRuntime &operator=(const StoreForwardRuntime &)=delete;
  struct Snapshot {bool enabled,server,active,pending;uint32_t admitted,rejected,queued,dropped,heartbeatAt,heartbeatPeriod;StoreForwardMemory::Snapshot memory;stock::StoreForwardModule::ClientInfo clientInfo;};
  Snapshot snapshot()const{return {enabled_,server_,control_&&control_->active(),pendingReady_,admitted_,rejected_,queued_,dropped_,heartbeatAt_,heartbeatPeriod_,memory_.snapshot(),clientInfo_};}
  const StoreForwardArchive *localArchive()const{
    return configured_&&enabled_&&server_?memory_.archive():nullptr;
  }
  bool archiveLocalText(Service &service,const StoreForwardArchive::MessageView &message){
    const auto *archive=localArchive();
    if(!archive||epoch_!=service.storeForwardEpoch()||message.from!=service.route.identity()||
       message.to!=StoreForwardArchive::BroadcastNode||!message.id||
       message.channel>=8||message.hopLimit>7||message.hopStart>7||
       message.payloadSize>StoreForwardArchive::MaxPayload||
       (message.payloadSize&&!message.payload)||
       message.transportMechanism<_meshtastic_MeshPacket_TransportMechanism_MIN||
       message.transportMechanism>_meshtastic_MeshPacket_TransportMechanism_MAX)return false;
    const auto result=memory_.archive()->add(message);
    if(result==StoreForwardArchive::AddResult::InvalidMessage||result==StoreForwardArchive::AddResult::Unavailable){
      ++rejected_;return false;
    }
    ++admitted_;return true;
  }
  void receive(Service &service,const uint8_t *frame,size_t size,const bench::RouteMetadata &m,
               uint8_t channel,uint32_t now,uint32_t utc,uint32_t uptimeSeconds=0){
    if(!configured_||!enabled_||epoch_!=service.storeForwardEpoch()||channel>=8||!frame||size<17)return;
    const uint32_t from=bench::le32(frame+4),to=bench::le32(frame),id=bench::le32(frame+8);
    if(from<=1||from>=0xfffffffeu||from==service.route.identity()||!id||service.nodes.ignored(from)||!shape(service,frame,size,m)){++rejected_;return;}
    for(const auto &seen:seen_)if(seen.from==from&&seen.id==id&&seen.channel==channel&&uint32_t(now-seen.at)<600000)return;
    const auto remember=[&]{seen_[nextSeen_++%32]={from,id,now,channel};};
    if(m.portnum==1){
      if(!server_||!control_||!memory_.archive())return;
      const bool legacy=stock::StoreForwardModule::legacyHistoryRequest(
          to==service.route.identity(),m.payload,m.payloadLength);
      if(!legacy){
        remember();
        StoreForwardArchive::MessageView view;view.from=from;view.to=to;view.id=id;
        view.time=utc?utc:uptimeSeconds;view.hasRxTime=utc!=0;
        view.channel=channel;view.hopLimit=frame[12]&7;view.hopStart=(frame[12]>>5)&7;
        view.viaMqtt=(frame[12]&0x10)!=0;view.transportMechanism=meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
        view.replyId=m.reply_id;view.emoji=m.emoji!=0;
        view.payload=m.payload;view.payloadSize=m.payloadLength;
        if(memory_.archive()->add(view)==StoreForwardArchive::AddResult::InvalidMessage)++rejected_;else ++admitted_;
        return;
      }
    }else {
      // Broadcast router heartbeats are client observations. A server request
      // must be explicitly addressed to this node before any history/control
      // dispatch; receiving a shared-channel broadcast is not such intent.
      if(server_?to!=service.route.identity():(to!=UINT32_MAX&&to!=service.route.identity()))return;
    }
    meshtastic_StoreAndForward message=meshtastic_StoreAndForward_init_zero;
    if(m.portnum==1)message.rr=meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY;
    else if(!store_forward::StoreForwardWire::decode(m.payload,m.payloadLength,&message)){++rejected_;return;}
    if(message.rr==meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY&&
       message.which_variant==meshtastic_StoreAndForward_history_tag&&
       message.variant.history.window>StoreForwardSettings::MaxReturnWindowMinutes){++rejected_;return;}
    const bool abort=message.rr==meshtastic_StoreAndForward_RequestResponse_CLIENT_ABORT||
                     message.rr==meshtastic_StoreAndForward_RequestResponse_CLIENT_ERROR;
    // A transfer belongs to its original requester and channel until it finishes.
    if (abort && control_ && control_->active() && control_->activeRequester() == from) {
      if (channel != activeChannel_) return;
      remember();StoreForwardControl::Context context;context.peer=from;
      (void)control_->receive(message,context);
      if(pendingOwnsTransfer_){pendingReady_=false;pendingOwnsTransfer_=false;}
      if(deferredControlReady_&&deferredControl_.destination==from)deferredControlReady_=false;
      return;
    }
    // Both response slots are occupied. Leave an unaccepted request eligible
    // for retransmission, and avoid advancing the archive or its counters.
    const bool needsResponse = server_
        ? (message.rr == meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY ||
           message.rr == meshtastic_StoreAndForward_RequestResponse_CLIENT_PING ||
           message.rr == meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS)
        : (message.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_PING &&
           to == service.route.identity());
    if (pendingReady_ && deferredControlReady_ && needsResponse) { ++dropped_; return; }
    remember();
    if(!server_){
      if(to==service.route.identity())stock::StoreForwardModule::observeRouterInfo(message,clientInfo_);
      const bool heartbeat = stock::StoreForwardModule::observeRouterHeartbeat(message, now, heartbeatAt_, heartbeatPeriod_);
      if(!heartbeat && message.rr==meshtastic_StoreAndForward_RequestResponse_ROUTER_PING && to==service.route.identity()){
        StoreForwardControl::Action action;action.kind=StoreForwardControl::ActionKind::ControlMessage;action.destination=from;
        action.control.rr=meshtastic_StoreAndForward_RequestResponse_CLIENT_PONG;
        if(pendingReady_)deferControl(action,channel);else stage(action,channel,service,now);
      }
      return;
    }
    if(!control_)return;
    PrimaryChannel::Selection key{};const bool selected=service.config.channelSelection(channel,key);
    const bool publicChannel=!selected||key.publicDefault;PrimaryChannel::clear(key);
    StoreForwardControl::Context context;context.peer=from;context.nowMillis=now;
    context.nowEpochSeconds=utc?utc:uptimeSeconds;
    context.defaultChannel=publicChannel;context.wantResponse=m.wantResponse;
    const auto action=control_->receive(message,context);
    if (action.kind == StoreForwardControl::ActionKind::ControlMessage &&
        action.control.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY &&
        control_->active()) activeChannel_ = channel;
    if(pendingReady_)deferControl(action,channel);else stage(action,channel,service,now);
  }
  void poll(Service &service,const StoreForwardSettings &settings,bool trusted,bool dispatchPaused,bool psramReady,
            uint32_t now,uint32_t random){
    const bool becameReady=psramReady&&!psramWasReady_;psramWasReady_=psramReady;
    if(!trusted||!settings.valid()){if(configured_)reset();return;}
    const bool reconfigure=!configured_||!settings_.same(settings)||epoch_!=service.storeForwardEpoch()||becameReady;
    if(configured_&&server_&&!psramReady){
      // No array access after board readiness is withdrawn. A future verified
      // false-to-true edge permits one fresh canary/initialization attempt.
      (void)memory_.initialize(false,true,settings_.normalized().records);
      enabled_=false;pendingReady_=false;if(!reconfigure)return;
    }
    if(reconfigure){
      reset();settings_=settings;epoch_=service.storeForwardEpoch();configured_=true;
      const auto config=settings.normalized();enabled_=config.enabled;
      server_=stock::StoreForwardModule::serverRole(service.config.role(),config.isServer);
      if(enabled_&&server_){
        if(!memory_.initialize(psramReady,true,config.records)){enabled_=false;return;}
        StoreForwardControl::Config c(true,true,config.heartbeat,900,config.historyReturnMax,config.historyReturnWindow);
        control_=new(controlStorage_) StoreForwardControl(*memory_.archive(),c);
      }
    }
    if(!enabled_||dispatchPaused)return;
    if(pendingReady_){
      uint32_t id=0;const int result=service.submitStoreForward(pending_,now,random,id);
      if(!result){++queued_;pendingReady_=false;if(finalReplay_&&control_)control_->finishReplay();}
      else if((result!=-EAGAIN&&result!=-ENOBUFS&&result!=-EBUSY)||uint32_t(now-pendingAt_)>=5000){
        ++dropped_;pendingReady_=false;if(control_&&pendingOwnsTransfer_)control_->finishReplay();
      }
      if(pendingReady_)return;
    }
    if(deferredControlReady_){
      stage(deferredControl_,deferredChannel_,service,now);deferredControlReady_=false;
      if(pendingReady_)return;
    }
    if(!control_||service.moduleAnnouncementPending())return;
    const auto utilization=service.channelUtilization();const bool allowed=utilization&&isfinite(*utilization)&&*utilization>=0&&*utilization<25;
    const auto action=control_->poll(now,allowed);
    const uint8_t channel=action.kind==StoreForwardControl::ActionKind::ControlMessage ? 0 : activeChannel_;
    stage(action,channel,service,now);
  }
};
}
