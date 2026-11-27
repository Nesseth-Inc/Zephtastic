// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "admin/HamModeProjection.h"
#include "stock/history/TransmitHistory.h"
#include "companion/SatelliteReplay.h"
#include "RoutingCore.h"
#include "MtSignature.h"
#include "MtNodeInfo.h"
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE) || defined(CONFIG_MT_DROPZONE)
#include "stock/optional/ReplyWire.h"
#endif
#include "Endpoint.h"
#include "modules/TrafficNodeInfoResponder.h"
#include "ModuleAnnouncement.h"
#include "stock/modules/NeighborInfoModule.h"
#include "modules/TrafficManagementWindow.h"
#include "modules/MeshBeaconReceivedCache.h"
#if defined(CONFIG_MT_TAK_TRACKER)
#include "modules/TakPliPayload.h"
#include "modules/TakPliReplyPayload.h"
#endif
#if defined(CONFIG_MT_GPIO_MODULES)
#include "modules/GpioModuleChannels.h"
#include "modules/GpioModuleWire.h"
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
#include "modules/StoreForwardArchive.h"
#endif
#include "pki/PrivateRuntime.h"
#include "pki/PrivateApplication.h"
#if defined(CONFIG_MT_RF_ADMIN_GET)
#include "admin/AdminQueryWindow.h"
#include "admin/LegacyAdminProtocol.h"
#include "admin/RemoteAdminMutationProtocol.h"
#endif
#include "Config.h"
#include "CorePorts.h"
#include "Discovery.h"
#include "NodeCache.h"
#include "NodeUser.h"
#include "AirtimeMeter.h"
#include "ChannelMeter.h"
#include "stock/modules/HopScalingModule.h"
#include "TxAdmission.h"
#include "RegionRuntimePolicy.h"
#include <errno.h>
#include "network/MqttDownlinkAdmission.h"
#include "network/MqttClientProxy.h"
#include <pb_decode.h>
#include <pb_encode.h>

#include "RadioDiagnostic.h"
namespace mt {
namespace modules { class BeaconTargetRuntime; }
// Stable new local identity, separate domain from stock MAC/PKI identities.
// Not a cryptographic identifier. No private key is generated or stored.
inline uint32_t identity(const uint8_t *id,size_t len) {
  if(!id||len<6)return 0;
  uint32_t h=2166136261u;
  for(char c:"MT-ZEPHYR-BASELINE-1") {h^=uint8_t(c);h*=16777619u;}
  for(size_t i=0;i<len;++i) {h^=id[i];h*=16777619u;}
  h=(h&0x7fffff00u)|((h&255u)?(h&255u):1u);
  return h>1?h:0x10001u;
}
struct Radio {
  // Pure RAM metadata only; implementations must perform no modem I/O.
  virtual bool diagnostic(NativeStages &out)const{out={};return false;}
  virtual bool ready()=0;
  // A backend must opt into nondefault persisted modulation. Existing test
  // and bench adapters cannot silently accept a setting they do not apply.
  virtual int configure(const Config &c){return c.modemPreset()==0&&c.channelNumber()==20?0:-ENOTSUP;}
  virtual bool beaconIsolationSupported()const{return false;}
  virtual void beaconRxIsolation(bool){}
  virtual int start()=0;
  virtual int restore()=0;
  virtual int stop()=0;
  // Storage/config transitions need a stronger stop than ordinary shutdown:
  // implementations must preserve completed application RX events and return
  // -EAGAIN after restoring RX if an event raced the transition. A hard error
  // cannot authorize stopped service bookkeeping; -ESHUTDOWN is the special
  // fail-closed result for verified physical standby and reconciled events.
  virtual int stopForStorage(){return stop();}
  // Passive application RX backlog. An idle owner must not discard a completion.
  virtual bool receiveEventPending()const{return false;}
  virtual int health()=0;
  virtual bool busy()=0;
  // Passive RAM detail from that exact busy observation, never extra driver I/O.
  // Storage reason2 uses this code; reason1 retains its Service-state bit mask.
  virtual uint16_t busyDiagnostic()const{return 0;}
  virtual uint32_t airtime(size_t n)=0;
  virtual int send(uint8_t *p,size_t n)=0;
  virtual bool completion(int &result)=0;
};
struct StoragePauseDiagnostic {
  uint32_t sequence=0;
  uint16_t mask=0;
  uint8_t reason=0;
  int result=0;
};
using NeighborSnapshotFn=bool(*)(void*,meshtastic_NeighborInfo&);
#if defined(CONFIG_MT_RF_ADMIN_GET)
class Service;
namespace admin {
class LegacyAdminRequest {
  friend class ::mt::Service;
public:
  enum class Kind:uint8_t { Invalid=0, Get, Set };
  LegacyAdminRequest(const LegacyAdminRequest &)=delete;
  LegacyAdminRequest &operator=(const LegacyAdminRequest &)=delete;
  ~LegacyAdminRequest(){wipe(this,sizeof(*this));}
  uint32_t from()const{return from_;}
  uint32_t packetId()const{return packetId_;}
  uint32_t receivedAt()const{return receivedAt_;}
  uint32_t domainEpoch()const{return domainEpoch_;}
  uint8_t channelIndex()const{return channelIndex_;}
  uint8_t channelHash()const{return channelHash_;}
  uint8_t responseHop()const{return responseHop_;}
  Kind kind()const{return kind_;}
  bool reliable()const{return reliable_;}
  bool requestData(const uint8_t *&bytes,size_t &size)const {
    bytes=data_;size=dataSize_;return state_==State::Queued&&dataSize_!=0;
  }
private:
  enum class State:uint8_t { Empty=0,Queued };
  LegacyAdminRequest()=default;
  void clear(){wipe(this,sizeof(*this));}
  void copyFrom(const LegacyAdminRequest &other){
    from_=other.from_;packetId_=other.packetId_;receivedAt_=other.receivedAt_;
    domainEpoch_=other.domainEpoch_;channelIndex_=other.channelIndex_;channelHash_=other.channelHash_;
    responseHop_=other.responseHop_;
    memcpy(key_,other.key_,sizeof(key_));keySize_=other.keySize_;role_=other.role_;
    memcpy(name_,other.name_,sizeof(name_));memcpy(data_,other.data_,sizeof(data_));
    dataSize_=other.dataSize_;kind_=other.kind_;reliable_=other.reliable_;state_=other.state_;
  }
  static void wipe(void *memory,size_t size) {
    volatile uint8_t *p=static_cast<volatile uint8_t *>(memory);while(size--)*p++=0;
  }
  uint32_t from_=0,packetId_=0,receivedAt_=0,domainEpoch_=0;
  uint8_t channelIndex_=0,channelHash_=0,responseHop_=0,key_[32]{},keySize_=0,role_=0;
  char name_[13]{};
  uint8_t data_[remote_protocol::MaxDataBytes]{};
  uint16_t dataSize_=0;
  Kind kind_=Kind::Invalid;
  bool reliable_=false;
  State state_=State::Empty;
};
using LegacyAdminHandler=bool(*)(void*,const LegacyAdminRequest&,uint32_t,
    uint8_t *,size_t,size_t &);
}
using PrivateAdminSnapshotFn=bool(*)(void*,uint32_t,uint32_t,const uint8_t[32],uint32_t,
    const uint8_t *,size_t,uint8_t *,size_t,size_t &);
using PrivateAdminMutationFn=bool(*)(void*,uint32_t,uint32_t,const uint8_t[32],uint32_t,
    const uint8_t *,size_t,uint32_t,uint32_t &);
using AdminResponseQueuedFn=void(*)(void*,bool,uint32_t,uint32_t,uint32_t);
#endif
class Service:private Endpoint::ControlEncoder
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    ,private bench::ChannelOriginPolicy
#endif
{
  friend class modules::BeaconTargetRuntime;
  Radio &radio_;
  modules::TrafficManagementWindow *traffic_=nullptr;
  uint64_t trafficUptime_=0;bool trafficSuppressed_=false;
#if defined(CONFIG_MT_WARM_PEERS)
  bool (*positionRole_)(uint32_t,uint8_t&)=nullptr;
  bool (*coldIdentityVeto_)(uint32_t)=nullptr;
#endif
  void *trafficNodeInfoContext_=nullptr;
  bool (*trafficNodeInfoSink_)(void*,const uint8_t*,size_t,const bench::RouteMetadata&,unsigned,uint32_t,uint32_t)=nullptr;
  struct ProxyNodeInfoBits {uint32_t node=0,at=0;uint8_t channelIndex=0,channelHash=0,okToMqtt=0;};
  ProxyNodeInfoBits proxyNodeInfoBits_[16]{};uint8_t proxyNodeInfoBitsNext_=0;

  void rememberProxyNodeInfoBits(uint32_t node,uint8_t channelIndex,uint8_t channelHash,
                                 const bench::RouteMetadata &metadata,uint32_t now) {
    if(!node||!metadata.hasBitfield||(metadata.bitfield&~3u))return;
    ProxyNodeInfoBits *entry=nullptr;
    for(auto &candidate:proxyNodeInfoBits_)if(candidate.node==node&&
       candidate.channelIndex==channelIndex&&candidate.channelHash==channelHash){entry=&candidate;break;}
    if(!entry)entry=&proxyNodeInfoBits_[proxyNodeInfoBitsNext_++%16u];
    *entry={node,now,channelIndex,channelHash,uint8_t(metadata.bitfield&1u)};
  }
  bool proxyNodeInfoOkToMqtt(uint32_t node,uint8_t channelIndex,uint8_t channelHash,
                             uint32_t now,uint8_t &value)const {
    for(const auto &entry:proxyNodeInfoBits_) {
      if(entry.node!=node||entry.channelIndex!=channelIndex||entry.channelHash!=channelHash||
         uint32_t(now-entry.at)>21600000u)continue;
      value=entry.okToMqtt;return true;
    }
    return false;
  }

  uint8_t selectedChannel_=0;
  uint32_t waypointDomainEpoch_=1;
#if defined(CONFIG_MT_REPLY_BOT)
  stock::optional::ReplyBotModule replyBot_;
#endif
#if defined(CONFIG_MT_DROPZONE)
  stock::optional::DropzoneModule dropzone_;
#endif
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE)
  struct PendingStockReply {
    stock::optional::Reply reply{};
    char text[96]{};
    bool ready=false;
  } stockReply_;
#endif
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE) || defined(CONFIG_MT_DROPZONE)
  struct StockSeen {uint32_t from,id,at;};
  StockSeen stockSeen_[64]{};
  unsigned stockSeenIndex_=0;
  uint32_t optionalEpoch_=0,optionalNode_=0;
  void syncOptionalDomain() {
    if(optionalNode_==route.identity()&&optionalEpoch_==waypointDomainEpoch_)return;
    optionalNode_=route.identity();optionalEpoch_=waypointDomainEpoch_;
    memset(stockSeen_,0,sizeof(stockSeen_));stockSeenIndex_=0;
#if defined(CONFIG_MT_REPLY_BOT)
    replyBot_={};
#endif
#if defined(CONFIG_MT_DROPZONE)
    dropzone_={};
#endif
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE)
    stockReply_={};
#endif
  }
#endif
  bool selectedDefault_=true;
  uint8_t packetSignaturePolicy_=0; // Stock COMPATIBLE default.
  uint32_t refusedIdentity_=0,lastRefusedNotice_=0;
  bool meshBeaconListening_=false;
#if defined(CONFIG_MT_TAK_TRACKER)
  uint8_t takTeam_=0,takRole_=0;
  bool takReady_=false;
  // Complete protobuf scratch before entering the persistent-ID call chain.
  [[gnu::noinline]] static bool takData(const Config &cfg,const meshtastic_Position &position,
                                      uint32_t node,uint8_t battery,uint8_t team,uint8_t role,
                                      uint8_t *data,size_t capacity,size_t &size) {
    const auto owner=cfg.node();char name[25]{};
    memcpy(name,owner.bytes+bench::MtNodeRecord::Name,24);
    tak_pli::Inputs input{&position,name,node,battery,cfg.fixedPosition(),true,
      static_cast<meshtastic_Team>(team),static_cast<meshtastic_MemberRole>(role)};
    uint8_t payload[tak_pli::PayloadLimit]{};size_t length=0;
    return tak_pli::encode(input,payload,sizeof(payload),length)==tak_pli::Result::Ok&&
      PublicApplication::encode(meshtastic_PortNum_ATAK_PLUGIN_V2,payload,length,0,0,data,capacity,size);
  }
  int announceTakPosition(uint32_t now,uint32_t random) {
    if(!takReady_||!normalMode_||!endpointArmed_||!txAllowed_||!receiving_||fault_||
       privatePaused_||(private_&&!publicReady()))return -EACCES;
    if(!endpoint_.moduleControlAvailable())return -EAGAIN;
    const unsigned precision=config.positionPrecision(selectedChannel_);
    meshtastic_Position position{};
    if(!precision||!localPosition(position))return -EACCES;
    PositionPrecision::apply(position,precision);
    if(position.latitude_i < -900000000||position.latitude_i>900000000||
       position.longitude_i < -1800000000||position.longitude_i>1800000000)return -EACCES;
    // Unknown battery remains omitted. USB power is not a measured cell charge.
    const uint8_t battery=hardware_.cellPercent&&*hardware_.cellPercent<=100?*hardware_.cellPercent:0;
    uint8_t data[239]{};size_t size=0;
    if(!takData(config,position,route.identity(),battery,takTeam_,takRole_,data,sizeof(data),size))return -EINVAL;
    const auto id=endpoint_.generatedId(random);if(!id)return -EAGAIN;
    bench::Frame frame;
    if(!encodePublicApplication(UINT32_MAX,id,data,size,frame))return -EPERM;
    frame.bytes[12]=uint8_t(config.hopLimit()|(config.hopLimit()<<5));
    const int result=endpoint_.submitModuleControl(frame,now,private_&&codec.signsPublic(),true,false,false,0,false,true);
    bench::wipeKeyBuffer(&frame,sizeof(frame));return result;
  }
#endif
public:
  using MqttFrameSink=void(*)(void*,const uint8_t*,size_t,const bench::RouteMetadata&,unsigned);
  // Successful local private TX only; frame is borrowed and already encrypted.
  using MqttPrivateTxSink=void(*)(void*,const uint8_t*,size_t,uint32_t);
  // Borrowed MQTT frame admitted for the local phone receive path. Private
  // consumers must still take their immutable authenticated PrivateEvent.
  using MqttLocalRxSink=void(*)(void*,const uint8_t*,size_t,uint32_t);
  // The sink returns true when encrypted MQTT uplinking was enabled for this observation.
  using MqttPkiTransitSink=bool(*)(void*,const uint8_t*,size_t,uint32_t,uint32_t,bool);
  // Authenticated/decoded packet metadata only; no frame, payload or key crosses this callback.
  using HotNodeObservationSink=void(*)(void*,uint32_t,uint32_t,uint32_t,bool);
#if defined(CONFIG_MT_GPIO_MODULES)
  using GpioFrameSink=void(*)(void*,const uint8_t*,size_t,const bench::RouteMetadata&,unsigned,uint32_t,uint32_t);
#endif
private:
  bool actualPublicSignature(const bench::Frame &frame,bool &signedPublic)const {
    signedPublic=false;
    if(frame.len<17||frame.len>255)return false;
    bench::RouteMetadata metadata{};
    // Origin frames are already channel encoded. Use the selected codec to
    // inspect their Data; parsing ciphertext as protobuf would reject normal
    // encrypted channels (and could misclassify an accidental parse).
    const bool valid=codec.decode(frame.bytes,frame.len,metadata)&&
      (!metadata.signatureLength||metadata.signatureLength==64);
    if(valid)signedPublic=metadata.signatureLength==64;
    bench::wipeKeyBuffer(&metadata,sizeof(metadata));return valid;
  }
  bool positionApplicationData(const uint8_t *data,size_t size)const {
    if(!data||!size||size>239)return false;
    bench::RouteMetadata metadata{};
    const bool valid=bench::MtCodec::decodeData(data,size,metadata)&&
      metadata.portnum==meshtastic_PortNum_POSITION_APP;
    bench::wipeKeyBuffer(&metadata,sizeof(metadata));return valid;
  }
  MqttFrameSink mqttFrameSink_=nullptr;void *mqttFrameContext_=nullptr;
#if defined(CONFIG_MT_GPIO_MODULES)
  GpioFrameSink gpioFrameSink_=nullptr;void *gpioFrameContext_=nullptr;
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  MqttFrameSink storeForwardSink_=nullptr;void *storeForwardContext_=nullptr;
  using StoreForwardLocalTextSink=bool(*)(void*,const modules::StoreForwardArchive::MessageView&);
  StoreForwardLocalTextSink storeForwardLocalTextSink_=nullptr;void *storeForwardLocalTextContext_=nullptr;
  using StoreForwardArchiveProvider=const modules::StoreForwardArchive*(*)(void*);
  StoreForwardArchiveProvider storeForwardArchiveProvider_=nullptr;void *storeForwardArchiveContext_=nullptr;
  uint32_t storeForwardEpoch_=0;
#endif
  MqttPrivateTxSink mqttPrivateTxSink_=nullptr;void *mqttPrivateTxContext_=nullptr;
  MqttLocalRxSink mqttLocalRxSink_=nullptr;void *mqttLocalRxContext_=nullptr;
  // Borrowed only while the Service owner dispatches this admitted envelope.
  struct MqttFrameView {const uint8_t *data;size_t size;};
  const MqttFrameView *mqttRadioOffFrame_=nullptr;
  struct OpaqueUplinkId { uint32_t from,id; };
  OpaqueUplinkId opaqueUplinkSeen_[8]{};
  uint8_t opaqueUplinkNext_=0;
  MqttPkiTransitSink mqttPkiTransitSink_=nullptr;void *mqttPkiTransitContext_=nullptr;
  HotNodeObservationSink hotNodeObservationSink_=nullptr;void *hotNodeObservationContext_=nullptr;
  void observeHotNode(uint32_t from,uint32_t utc,uint32_t uptime,bool viaMqtt) {
    if(hotNodeObservationSink_&&from&&from!=route.identity())
      hotNodeObservationSink_(hotNodeObservationContext_,from,utc,uptime,viaMqtt);
  }
  struct MqttPrivateNotice {const Endpoint::Job *job=nullptr;uint32_t id=0;};
  MqttPrivateNotice mqttPrivateNotices_[8]{};
  // Exact last-RX channel classification, retained only for serialized client
  // fanout. It is ciphertext equality, never a new trust/replay authority.
  bench::Frame sharedZeroReceive_{};
  bool firstPrivateTxNotice(const Endpoint::Job *job) {
    if(!job)return false;
    const uint32_t id=bench::le32(job->frame.bytes+8);if(!id)return false;
    for(auto &entry:mqttPrivateNotices_)if(entry.job==job) {
      if(entry.id==id)return false;
      entry.id=id;return true;
    }
    for(auto &entry:mqttPrivateNotices_)if(!entry.job) {entry={job,id};return true;}
    return false;
  }
  bool mqttDownlinkFrame(const meshtastic_MeshPacket &packet,uint8_t hash,
                        bool privateDelivery,uint32_t now,uint32_t random,
                        bool forceOpaqueChannelZero=false,uint32_t utcNow=0,uint32_t uptimeSeconds=0) {
    uint8_t frame[255]{};
    const size_t size=size_t(packet.encrypted.size)+16;
    if(size>sizeof(frame)||!packet.encrypted.size)return false;
    for(unsigned i=0;i<4;++i) {
      frame[i]=uint8_t(packet.to>>(8*i));
      frame[4+i]=uint8_t(packet.from>>(8*i));
      frame[8+i]=uint8_t(packet.id>>(8*i));
    }
    frame[12]=uint8_t((packet.hop_limit&7u)|(packet.want_ack?8u:0u)|0x10u|
                      ((packet.hop_start&7u)<<5));
    frame[13]=hash;frame[14]=uint8_t(packet.next_hop);frame[15]=uint8_t(packet.relay_node);
    memcpy(frame+16,packet.encrypted.bytes,packet.encrypted.size);
    const MqttFrameView view{frame,size};
    if(!receiving_&&!privateDelivery&&!forceOpaqueChannelZero)mqttRadioOffFrame_=&view;
    bool routeAccepted=false,channelDelivered=false;
    rx(frame,size,0,now,random,false,&routeAccepted,forceOpaqueChannelZero,utcNow,uptimeSeconds,
       0,0,&channelDelivered);
    // PrivateRuntime owns authentication, replay and immutable owner-mask
    // delivery. Failed auth/duplicates expose no event to these consumers.
    // Fan out only after rx() and its metadata/scratch have unwound. A channel
    // delivery must have reached the local endpoint after packet policy.
    if((privateDelivery||(!forceOpaqueChannelZero&&channelDelivered))&&mqttLocalRxSink_)
      mqttLocalRxSink_(mqttLocalRxContext_,frame,size,now);
    mqttRadioOffFrame_=nullptr;
    bench::wipeKeyBuffer(frame,sizeof(frame));
    return privateDelivery||!forceOpaqueChannelZero||routeAccepted;
  }
  void mqttFrame(const uint8_t *frame,size_t length) {
    if(!mqttFrameSink_)return;
    bench::RouteMetadata metadata{};unsigned index=0;
    if(decodeChannel(frame,length,metadata,index)){
      ChannelScope scope(*this,index);
      if(scope.valid())mqttFrameSink_(mqttFrameContext_,frame,length,metadata,index);
    }
    bench::wipeKeyBuffer(&metadata,sizeof(metadata));
  }
  void mqttFrame(const uint8_t *frame,size_t length,
                 const bench::RouteMetadata &metadata,unsigned channelIndex) {
    if(!mqttFrameSink_||!frame||length<17||length>255||channelIndex>=8||
       !metadata.channelDecoded||metadata.decodeRejected||metadata.backendFailed||
       metadata.signatureRejected||metadata.signatureQuarantined)return;
    ChannelScope scope(*this,channelIndex);
    if(scope.valid())mqttFrameSink_(mqttFrameContext_,frame,length,metadata,channelIndex);
  }
  // Shared core bridge between the MqttChannelUplink privacy gate and an
  // authorized local Companion. Capacity one avoids a multi-kilobyte queue
  // on constrained T096 targets; producer and consumer run on the Service
  // owner, so this queue is deliberately single-owner rather than concurrent.
  mqtt::client_proxy::Queue<1> mqttProxyPublishQueue_;
  bool mqttProxyBridgeEnabled_=false;
  beacon::MeshBeaconReceivedCache receivedBeacons_{};
  friend struct ReceivedBeaconCacheTestAccess;
  bool txAllowed_,receiving_=false,relaying_=false,fault_=false,active_=false,own_=false;
  uint32_t deadline_=0,quiet_=0,window_=0,budget_=0;
  bench::Frame activeFrame_,endpointFrame_;
  bool endpointArmed_=false,endpointQueued_=false,endpointActive_=false;
  uint8_t endpointOwner_=0,resultOwner_=0;
  uint32_t endpointCount_=0,endpointLimit_=1,endpointId_=0,resultId_=0;
  int resultStatus_=0;
  struct VerifiedPeerCandidate {uint32_t node=0;uint8_t publicKey[32]{};bool pending=false;};
  VerifiedPeerCandidate verifiedPeerCandidate_{};
  bool normalMode_=false,normalDiscovery_=true,privateRequired_=false,configurationTransition_=false;
  uint8_t hardwareModel_=81;
  Endpoint endpoint_{this};
  HardwareSnapshot hardware_;
  pki::PrivateRuntime *private_=nullptr;
  pki::KeyVerificationReceive keyVerificationReceive_{};bool keyVerificationReceiveReady_=false;
  bool privatePaused_=false,privateWasReceiving_=false;
  struct BootstrapRequest {uint32_t to=0,request=0,expires=0;uint8_t hop=0;bool reliable:1,ready:1;uint8_t channel:3;bool verified:1;};
  BootstrapRequest bootstrapRequest_{};
  Endpoint::Job *normalActive_=nullptr;
  bool normalTransitActive_=false;
  StoragePauseDiagnostic storagePauseDiagnostic_{};
  RadioDiagnostic radioDiagnostic_{};
  uint32_t completedPacketsTx_=0,completedRelays_=0;
  void (*localStatsSource_)(const NodeCache &,LocalStatsObservation &)=nullptr;
  NeighborSnapshotFn neighborSnapshotFn_=nullptr;
  bool (*neighborRelayEnabled_)(void *)=nullptr;
  void *neighborSnapshotCtx_=nullptr;
#if defined(CONFIG_MT_RF_ADMIN_GET)
  PrivateAdminSnapshotFn privateAdminSnapshotFn_=nullptr;
  void *privateAdminSnapshotCtx_=nullptr;
  PrivateAdminMutationFn privateAdminMutationFn_=nullptr;
  void *privateAdminMutationCtx_=nullptr;
  bool privateAdminMutationProcessing_=false;
  struct PendingPrivateAdminMutation {
    enum class State:uint8_t {Empty,Queued};
    State state=State::Empty;
    uint32_t local=0,from=0,request=0,receivedAt=0;
    uint8_t key[32]{},data[admin::remote_protocol::MaxDataBytes]{};
    uint16_t size=0;
    uint8_t responseHop=0;
    bool reliable=false;
  } privateAdminMutation_{};
  static constexpr uint32_t PrivateAdminMutationLifetimeMs=30000;
  admin::AdminQueryWindow privateAdminQueries_{};
  AdminResponseQueuedFn adminResponseQueuedFn_=nullptr;
  void *adminResponseQueuedContext_=nullptr;
  admin::LegacyAdminHandler legacyAdminHandler_=nullptr;
  void *legacyAdminContext_=nullptr;
  uint32_t legacyAdminDomainEpoch_=1;
  uint32_t legacyAdminNow_=0;
  bool legacyAdminProcessing_=false;
  bool legacyAdminReplyQueued_=false;
  const admin::LegacyAdminRequest *activeLegacyAdminRequest_=nullptr;
  admin::LegacyAdminRequest legacyAdminRequest_{};
  struct PendingLegacyAdminBusy {
    bool ready=false;
    uint32_t from=0,packetId=0,receivedAt=0,domainEpoch=0;
    uint8_t channelIndex=0,channelHash=0,responseHop=0;
    uint32_t routingError=meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED;
    bool reliable=false;
  } legacyAdminBusy_{};
  static constexpr uint32_t LegacyAdminRequestLifetimeMs=30000;
  static constexpr uint32_t LegacyAdminSetReplayLifetimeMs=300000;
  struct CompletedLegacyAdminSet {
    bool used=false;
    uint32_t from=0,packetId=0,domainEpoch=0,completedAt=0,routingError=0;
  } completedLegacyAdminSets_[32]{};
  uint8_t completedLegacyAdminSetNext_=0;
#endif
  static constexpr unsigned TraceAssociations=16;
  struct TraceAssociation {
    TraceRoute::Discovery incoming{},outgoing{};
    uint32_t from=0,id=0,to=0,request=0,at=0;
    uint8_t channel=0,start=0,next=0,channelSlot=0;
    bool response=false,used=false;
  } traceAssociations_[TraceAssociations]{};
  void clearTraceAssociations() {for(auto &entry:traceAssociations_)entry=TraceAssociation{};}
  void expireTraceAssociations(uint32_t now) {
    for(auto &entry:traceAssociations_)if(entry.used&&uint32_t(now-entry.at)>=bench::RoutingCore::HISTORY_TTL)
      entry=TraceAssociation{};
  }
  TraceAssociation *traceAssociation(uint32_t from,uint32_t id) {
    for(auto &entry:traceAssociations_)if(entry.used&&entry.from==from&&entry.id==id)return &entry;
    return nullptr;
  }
  TraceAssociation *freeTraceAssociation(uint32_t now) {
    for(auto &entry:traceAssociations_)if(!entry.used)return &entry;
    TraceAssociation *oldest=nullptr;uint32_t age=0;
    for(auto &entry:traceAssociations_)if(!route.hasQueued(entry.from,entry.id)&&
      (!oldest||uint32_t(now-entry.at)>age)){oldest=&entry;age=uint32_t(now-entry.at);}
    if(oldest)*oldest=TraceAssociation{};
    return oldest;
  }
  bool matchesTraceAssociation(const TraceAssociation &entry,const uint8_t *p,
                                      const bench::RouteMetadata &metadata,
                                      const TraceRoute::Discovery &semantic,unsigned start) {
    return sameChannel(entry.channelSlot,selectedChannel_)&&entry.to==bench::le32(p)&&entry.channel==p[13]&&entry.start==start&&
      entry.next==p[14]&&entry.response==!metadata.wantResponse&&
      entry.request==metadata.request_id&&TraceRoute::sameSemantic(entry.incoming,semantic);
  }
  bool matchesTraceLoopAssociation(const TraceAssociation &entry,const uint8_t *p,
                                          const bench::RouteMetadata &metadata,
                                          const TraceRoute::Discovery &semantic,unsigned start) {
    return sameChannel(entry.channelSlot,selectedChannel_)&&entry.to==bench::le32(p)&&entry.channel==p[13]&&entry.start==start&&
      entry.next==p[14]&&entry.response==!metadata.wantResponse&&
      entry.request==metadata.request_id&&TraceRoute::sameSemantic(entry.outgoing,semantic);
  }
  void endpointResult(int status) {resultId_=endpointId_;resultOwner_=endpointOwner_;resultStatus_=status;endpointQueued_=endpointActive_=false;}
  void stoppedState() {
    // A stopped or canceled in-flight TX has an unknown outcome: tracked in
    // the bounded quality counter, never credited as airtime or zeroed.
    noteTxUncertain();
    if(endpointQueued_||endpointActive_)endpointResult(-ECANCELED);
    endpointArmed_=false;nodeDiscovery.cancel();bootstrapRequest_={};
    if(private_)private_->clearEvent();
    bench::wipeKeyBuffer(&sharedZeroReceive_,sizeof(sharedZeroReceive_));
    clearVerifiedPeerCandidate();
#if defined(CONFIG_MT_RF_ADMIN_GET)
    clearPrivateAdminMutation();
    privateAdminQueries_.clear();
#endif
    privatePaused_=false;receiving_=relaying_=false;
    endpoint_.cancel();normalActive_=nullptr;normalTransitActive_=false;
#if defined(CONFIG_MT_RF_ADMIN_GET)
    legacyAdminReplyQueued_=false;  // Endpoint::cancel() canceled its queued reply.
#endif
    active_=own_=false;route.clearPending();discovery.cancel();
  }

  static uint32_t airtime(size_t n,void *ctx) {return static_cast<Radio *>(ctx)->airtime(n);}
  void updateRouteUtilization() {
    const auto observed=channel_.percent();
    if(!observed||!isfinite(*observed)||*observed<0)return;
    // The stock logger may exceed100 for a long whole-event credit. CW caps
    // at100; the admission gate still rejects that out-of-range observation.
    route.observeUtilization(uint8_t(*observed>100?100:*observed));
  }
  TxAdmission::Decision normalAdmission(bool polite) {
    if(!config.regionCode())return TxAdmission::Decision::Unavailable;
    const auto observed=channel_.percent();
    // percent() drains the native observer, refreshing support. A bound
    // empty logger alone can return zero even when that observer is absent.
    if(!channel_.diagnosticSupported()||!observed)return TxAdmission::Decision::Unavailable;
    const auto decision=TxAdmission::channel(true,*observed,polite);
    if(decision!=TxAdmission::Decision::Allow)return decision;
    route.observeUtilization(uint8_t(*observed));
    return transmitDutyAdmission(polite);
  }
  TxAdmission::Decision transmitDutyAdmission(bool polite) {
    // Use the same region and role as validation/resolution. Unknown airtime
    // stays unavailable for limited-duty regions; US retains its 100% bypass.
    const auto tx=airtime_.percent();
    const auto role=config.role();
    const bool fixed=role==meshtastic_Config_DeviceConfig_Role_ROUTER||
      role==meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
    if(config.licensedOperation()&&config.overrideDutyCycle()) {
      const auto *region=radio::RegionPolicy::lookup(config.regionCode());
      return region&&radio::RegionPolicy::softwareUsableOnSx126x(*region,false,false)?
        TxAdmission::Decision::Allow:TxAdmission::Decision::Unavailable;
    }
    return radio::RegionRuntimePolicy::duty(config.regionCode(),fixed,polite,
      tx.has_value(),tx?*tx:NAN);
  }

  void noteFault(FaultStage stage,DiagnosticDomain domain,int32_t value) {
    clearVerifiedPeerCandidate();
    if(!fault_)RadioDiagnostic::bump(radioDiagnostic_.faults);
    if(!radioDiagnostic_.faultStage) {
      radioDiagnostic_.faultStage=uint16_t(stage);radioDiagnostic_.faultDomain=domain;
      radioDiagnostic_.faultValue=value;radioDiagnostic_.faultFlags=storagePauseMask();
      radioDiagnostic_.nativeKnown=radio_.diagnostic(radioDiagnostic_.native);
    }
  }
#if defined(CONFIG_MT_RF_ADMIN_GET)
  void clearPrivateAdminMutation() {
    bench::wipeKeyBuffer(&privateAdminMutation_,sizeof(privateAdminMutation_));
  }
  void observeAdminResponseQueued(bool privateMessage,uint32_t peer,uint32_t request) {
    uint32_t id=0;
    if(adminResponseQueuedFn_&&endpoint_.queuedControlResponse(peer,request,privateMessage,id))
      adminResponseQueuedFn_(adminResponseQueuedContext_,privateMessage,peer,request,id);
  }
  void clearLegacyAdminRequest(){legacyAdminRequest_.clear();}
  void clearCompletedLegacyAdminSets(){
    bench::wipeKeyBuffer(completedLegacyAdminSets_,sizeof(completedLegacyAdminSets_));
    completedLegacyAdminSetNext_=0;
  }
  void expireCompletedLegacyAdminSets(uint32_t now) {
    for(auto &entry:completedLegacyAdminSets_)
      if(entry.used&&uint32_t(now-entry.completedAt)>=LegacyAdminSetReplayLifetimeMs)
        bench::wipeKeyBuffer(&entry,sizeof(entry));
  }
  bool completedLegacyAdminSet(uint32_t from,uint32_t packetId,uint32_t epoch,
                               uint32_t now,uint32_t &routingError) {
    routingError=0;expireCompletedLegacyAdminSets(now);
    for(const auto &entry:completedLegacyAdminSets_)
      if(entry.used&&entry.from==from&&entry.packetId==packetId&&entry.domainEpoch==epoch) {
        routingError=entry.routingError;return true;
      }
    return false;
  }
  void rememberCompletedLegacyAdminSet(uint32_t from,uint32_t packetId,uint32_t epoch,
                                       uint32_t now,uint32_t routingError) {
    if(from<=1||from>=0xfffffffeu||!packetId||!epoch||
       !(routingError<=9||(routingError>=32&&routingError<=39)))return;
    expireCompletedLegacyAdminSets(now);
    for(auto &entry:completedLegacyAdminSets_)
      if(entry.used&&entry.from==from&&entry.packetId==packetId&&entry.domainEpoch==epoch) {
        entry.completedAt=now;entry.routingError=routingError;return;
      }
    CompletedLegacyAdminSet *freeEntry=nullptr;
    for(unsigned offset=0;offset<32;++offset) {
      const unsigned index=(unsigned(completedLegacyAdminSetNext_)+offset)%32u;
      if(!completedLegacyAdminSets_[index].used) {
        freeEntry=&completedLegacyAdminSets_[index];
        completedLegacyAdminSetNext_=uint8_t((index+1u)%32u);break;
      }
    }
    // Stock history is bounded, not a mutation quota. Once full, replace the
    // next retained outcome; duplicate suppression lasts only while retained.
    if(!freeEntry) {
      const unsigned index=completedLegacyAdminSetNext_;
      freeEntry=&completedLegacyAdminSets_[index];
      completedLegacyAdminSetNext_=uint8_t((index+1u)%32u);
    }
    auto &entry=*freeEntry;
    entry={true,from,packetId,epoch,now,routingError};
  }
  struct LegacyAdminChannelSnapshot {
    bool active=false;uint8_t role=0,hash=0,keySize=0;char name[13]{};uint8_t key[32]{};
  };
  static bool legacyAdminChannelSnapshot(const Config &candidate,unsigned index,
                                         LegacyAdminChannelSnapshot &out) {
    out={};ChannelRecord record;PrimaryChannel::Selection selection{};
    if(index>=8||!candidate.channelRecord(index,record)||!record.role())return false;
    const char *name=index==0?candidate.effectivePrimaryName():
      (record.name()[0]?record.name():candidate.modemName());
    if(!Config::isAdminChannelName(name)||!candidate.channelSelection(index,selection)||
       selection.licensedPlaintext||
       (selection.length!=16&&selection.length!=32)) {
      PrimaryChannel::clear(selection);bench::wipeKeyBuffer(&record,sizeof(record));return false;
    }
    size_t length=0;while(length<12&&name[length])++length;
    if(length>=12){PrimaryChannel::clear(selection);bench::wipeKeyBuffer(&record,sizeof(record));return false;}
    out.active=true;out.role=uint8_t(record.role());out.hash=selection.hash;
    out.keySize=uint8_t(selection.length);memcpy(out.name,name,length);
    memcpy(out.key,selection.bytes,selection.length);
    PrimaryChannel::clear(selection);bench::wipeKeyBuffer(&record,sizeof(record));return true;
  }
  static bool sameLegacyAdminDomain(const Config &left,const Config &right) {
    if(left.adminChannelEnabled()!=right.adminChannelEnabled()||
       left.licensedOperation()!=right.licensedOperation())return false;
    for(unsigned i=0;i<8;++i) {
      LegacyAdminChannelSnapshot a{},b{};
      const bool activeA=legacyAdminChannelSnapshot(left,i,a);
      const bool activeB=legacyAdminChannelSnapshot(right,i,b);
      const bool same=activeA==activeB&&(!activeA||
        (a.role==b.role&&a.hash==b.hash&&a.keySize==b.keySize&&
         !memcmp(a.name,b.name,sizeof(a.name))&&!memcmp(a.key,b.key,sizeof(a.key))));
      bench::wipeKeyBuffer(&a,sizeof(a));bench::wipeKeyBuffer(&b,sizeof(b));
      if(!same)return false;
    }
    return true;
  }
  bool legacyAdminChannelCurrent(uint8_t index,uint8_t hash,uint32_t epoch,
                                 const uint8_t *key,uint8_t keySize,
                                 uint8_t role,const char name[13],uint32_t peer)const {
    if(epoch!=legacyAdminDomainEpoch_||!config.adminChannelEnabled()||
       config.licensedOperation()||index>=8||!key||!name||peer<=1||
       peer>=0xfffffffeu||peer==route.identity()||nodes.ignored(peer))return false;
    LegacyAdminChannelSnapshot current{};
    const bool active=legacyAdminChannelSnapshot(config,index,current);
    const bool exact=active&&current.hash==hash&&current.keySize==keySize&&
      current.role==role&&!memcmp(current.name,name,sizeof(current.name))&&
      !memcmp(current.key,key,keySize)&&
      config.channelCandidates(hash)==uint8_t(1u<<index);
    bench::wipeKeyBuffer(&current,sizeof(current));return exact;
  }
  bool currentLegacyBusyChannel(const PendingLegacyAdminBusy &busy)const {
    if(!busy.ready||busy.channelIndex>=8||busy.domainEpoch!=legacyAdminDomainEpoch_||
       !config.adminChannelEnabled()||config.licensedOperation()||nodes.ignored(busy.from))return false;
    LegacyAdminChannelSnapshot current{};
    const bool active=legacyAdminChannelSnapshot(config,busy.channelIndex,current);
    const bool exact=active&&current.hash==busy.channelHash&&
      config.channelCandidates(busy.channelHash)==uint8_t(1u<<busy.channelIndex);
    bench::wipeKeyBuffer(&current,sizeof(current));return exact;
  }
  static bool validPrivateAdminRoutingError(uint32_t error) {
    return error<=9||(error>=32&&error<=39);
  }
  static bool legacyAdminRoutingOutcome(const uint8_t *wire,size_t length,
      uint32_t requestPacketId,uint32_t &error) {
    error=0;
    if(!admin::legacy_protocol::canonicalResponse(wire,length,requestPacketId))return false;
    meshtastic_Data data{};admin::remote_protocol::Wipe wipeData{&data,sizeof(data)};
    uint8_t canonical[admin::remote_protocol::MaxDataBytes]{};
    admin::remote_protocol::Wipe wipeCanonical{canonical,sizeof(canonical)};
    if(!admin::remote_protocol::decodeCanonicalData(wire,length,data,canonical))return false;
    uint32_t port=0;
    if(!admin::remote_protocol::enumNumber(data.portnum,port)||port!=meshtastic_PortNum_ROUTING_APP)return false;
    meshtastic_Routing routing{};admin::remote_protocol::Wipe wipeRouting{&routing,sizeof(routing)};
    auto input=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
    if(!pb_decode(&input,meshtastic_Routing_fields,&routing)||input.bytes_left||
       routing.which_variant!=meshtastic_Routing_error_reason_tag||routing.ack_proof.size)return false;
    return admin::remote_protocol::enumNumber(routing.error_reason,error)&&
      (error<=9||(error>=32&&error<=39));
  }
  void stageLegacyAdminRequest(const uint8_t *packet,size_t packetSize,
      const bench::RouteMetadata &metadata,unsigned channelIndex,uint32_t from,
      uint32_t to,uint32_t now,bool radioReceive) {
    if(!packet||packetSize<17||packetSize>255||!radioReceive||!normalMode_||
       !endpointArmed_||!txAllowed_||fault_||!receiving_||privatePaused_||
       !config.adminChannelEnabled()||config.licensedOperation()||
       to!=route.identity()||from<=1||from>=0xfffffffeu||from==route.identity()||
       nodes.ignored(from)||(packet[12]&0x10u)||metadata.signatureLength||
       metadata.signatureVerified||metadata.signatureQuarantined||metadata.signatureRejected||
       metadata.publicKeyLength||metadata.portnum!=meshtastic_PortNum_ADMIN_APP||
       metadata.payloadLength==0||metadata.payloadLength>sizeof(metadata.payload)||
       !receivePortAllowed(metadata.portnum))return;
    const uint8_t candidates=config.channelCandidates(packet[13]);
    if(!candidates||(candidates&(uint8_t(candidates-1u)))||
       candidates!=uint8_t(1u<<channelIndex))return;

    ChannelRecord record;PrimaryChannel::Selection selection{};
    if(!config.channelRecord(channelIndex,record)||!record.role()||
       !Config::isAdminChannelName(channelIndex==0?config.effectivePrimaryName():
         (record.name()[0]?record.name():config.modemName()))||
       !config.channelSelection(channelIndex,selection)||selection.hash!=packet[13]||
       selection.licensedPlaintext||
       (selection.length!=16&&selection.length!=32)) {
      PrimaryChannel::clear(selection);bench::wipeKeyBuffer(&record,sizeof(record));return;
    }
    meshtastic_Data data=meshtastic_Data_init_zero;
    data.portnum=static_cast<meshtastic_PortNum>(metadata.portnum);
    data.payload.size=metadata.payloadLength;
    memcpy(data.payload.bytes,metadata.payload,metadata.payloadLength);
    data.want_response=metadata.wantResponse;data.has_bitfield=metadata.hasBitfield;
    data.bitfield=metadata.bitfield;data.request_id=metadata.request_id;
    data.reply_id=metadata.reply_id;data.emoji=metadata.emoji;
    uint8_t wire[admin::remote_protocol::MaxDataBytes]{};size_t wireSize=0;
    const bool encoded=admin::remote_protocol::encodeCanonical(data,wire,sizeof(wire),wireSize);
    bench::wipeKeyBuffer(&data,sizeof(data));
    if(!encoded||wireSize>sizeof(wire)||
       !codec.matchesEncodedData(packet,packetSize,wire,wireSize,false)) {
      PrimaryChannel::clear(selection);bench::wipeKeyBuffer(&record,sizeof(record));
      bench::wipeKeyBuffer(wire,sizeof(wire));return;
    }
    const auto kind=admin::legacy_protocol::classifyRequest(wire,wireSize);
    if(kind==admin::legacy_protocol::Kind::Invalid) {
      PrimaryChannel::clear(selection);bench::wipeKeyBuffer(&record,sizeof(record));
      bench::wipeKeyBuffer(wire,sizeof(wire));return;
    }
    const uint32_t packetId=bench::le32(packet+8);
    uint32_t cachedError=0;
    if(completedLegacyAdminSet(from,packetId,legacyAdminDomainEpoch_,now,cachedError)) {
      if(kind==admin::legacy_protocol::Kind::Set&&!legacyAdminBusy_.ready) {
        legacyAdminBusy_.ready=true;legacyAdminBusy_.from=from;legacyAdminBusy_.packetId=packetId;
        legacyAdminBusy_.receivedAt=now;legacyAdminBusy_.domainEpoch=legacyAdminDomainEpoch_;
        legacyAdminBusy_.channelIndex=uint8_t(channelIndex);legacyAdminBusy_.channelHash=selection.hash;
        legacyAdminBusy_.responseHop=uint8_t(HopPolicy::response((packet[12]>>5)&7u,
          packet[12]&7u,config.hopLimit()));
        legacyAdminBusy_.routingError=cachedError;legacyAdminBusy_.reliable=(packet[12]&8u)!=0;
      }
      PrimaryChannel::clear(selection);bench::wipeKeyBuffer(&record,sizeof(record));
      bench::wipeKeyBuffer(wire,sizeof(wire));return;
    }
    if(legacyAdminBusy_.ready&&legacyAdminBusy_.from==from&&
       legacyAdminBusy_.packetId==packetId) {
      PrimaryChannel::clear(selection);bench::wipeKeyBuffer(&record,sizeof(record));
      bench::wipeKeyBuffer(wire,sizeof(wire));return;
    }
    if(legacyAdminRequest_.state_!=admin::LegacyAdminRequest::State::Empty) {
      if(legacyAdminRequest_.from_==from&&legacyAdminRequest_.packetId_==packetId) {
        // Flooding duplicates and same-ID altered bodies coalesce to the
        // original immutable request; neither can dispatch a second setter.
        PrimaryChannel::clear(selection);bench::wipeKeyBuffer(&record,sizeof(record));
        bench::wipeKeyBuffer(wire,sizeof(wire));return;
      }
      if(!legacyAdminBusy_.ready) {
        legacyAdminBusy_.ready=true;legacyAdminBusy_.from=from;
        legacyAdminBusy_.packetId=packetId;legacyAdminBusy_.receivedAt=now;
        legacyAdminBusy_.domainEpoch=legacyAdminDomainEpoch_;
        legacyAdminBusy_.channelIndex=uint8_t(channelIndex);legacyAdminBusy_.channelHash=selection.hash;
        legacyAdminBusy_.responseHop=uint8_t(HopPolicy::response((packet[12]>>5)&7u,
          packet[12]&7u,config.hopLimit()));
        legacyAdminBusy_.routingError=meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED;
        legacyAdminBusy_.reliable=(packet[12]&8u)!=0;
      }
      PrimaryChannel::clear(selection);bench::wipeKeyBuffer(&record,sizeof(record));
      bench::wipeKeyBuffer(wire,sizeof(wire));return;
    }
    auto &queued=legacyAdminRequest_;
    queued.from_=from;queued.packetId_=packetId;queued.receivedAt_=now;
    queued.domainEpoch_=legacyAdminDomainEpoch_;queued.channelIndex_=uint8_t(channelIndex);
    queued.channelHash_=selection.hash;queued.keySize_=uint8_t(selection.length);
    queued.responseHop_=uint8_t(HopPolicy::response((packet[12]>>5)&7u,
      packet[12]&7u,config.hopLimit()));
    queued.role_=uint8_t(record.role());memcpy(queued.key_,selection.bytes,selection.length);
    const char *name=channelIndex==0?config.effectivePrimaryName():
      (record.name()[0]?record.name():config.modemName());
    size_t nameLength=0;while(nameLength<12&&name[nameLength])++nameLength;
    if(nameLength>=12) {
      queued.clear();PrimaryChannel::clear(selection);bench::wipeKeyBuffer(&record,sizeof(record));
      bench::wipeKeyBuffer(wire,sizeof(wire));return;
    }
    memcpy(queued.name_,name,nameLength);memcpy(queued.data_,wire,wireSize);
    queued.dataSize_=uint16_t(wireSize);queued.kind_=kind==admin::legacy_protocol::Kind::Get?
      admin::LegacyAdminRequest::Kind::Get:admin::LegacyAdminRequest::Kind::Set;
    queued.reliable_=(packet[12]&8u)!=0;
    queued.state_=admin::LegacyAdminRequest::State::Queued;
    PrimaryChannel::clear(selection);bench::wipeKeyBuffer(&record,sizeof(record));
    bench::wipeKeyBuffer(wire,sizeof(wire));
  }
#endif
  void fail(FaultStage stage=FaultStage::Unknown,DiagnosticDomain domain=DiagnosticDomain::Unknown,int32_t value=0) {
#if defined(CONFIG_MT_RF_ADMIN_GET)
    clearPrivateAdminMutation();
    clearCompletedLegacyAdminSets();
    clearLegacyAdminRequest();bench::wipeKeyBuffer(&legacyAdminBusy_,sizeof(legacyAdminBusy_));
#endif
    noteFault(stage,domain,value);fault_=true;if(private_)private_->freeze();stop();
  }
  int submitOutcome(int result,uint16_t stage) {
    if(result)noteOriginReject(stage,DiagnosticDomain::Errno,result);
    return result;
  }
  int storagePauseOutcome(uint8_t reason,int result,uint16_t mask) {
    storagePauseDiagnostic_.reason=reason;storagePauseDiagnostic_.result=result;storagePauseDiagnostic_.mask=mask;return result;
  }
  uint16_t storagePauseMask()const {
    uint16_t mask=0;
    if(privatePaused_)mask|=1;
    if(fault_)mask|=2;
    if(active_)mask|=4;
    if(own_)mask|=8;
    if(endpointQueued_)mask|=16;
    if(endpointActive_)mask|=32;
    if(normalActive_)mask|=64;
    if(!endpoint_.idle())mask|=128;
    if(discovery.pending())mask|=256;
    if(route.pending())mask|=512;
    if(route.retransmitting())mask|=1024;
    if(bootstrapRequest_.ready)mask|=2048;
    return mask;
  }
  bool privateReady()const {
    return private_&&private_->ready(route.identity());
  }
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  // The stock profile carries no channel nonce history. Compare the effective
  // key, since channel names, indices and the eight-bit hash can all collide.
  bool maySealChannel(const uint8_t *key,size_t length,bool plaintext)const override {
    return publicReady()&&(plaintext||(length==sizeof(bench::MtNodeRecord::PublicKey)&&
      !memcmp(key,bench::MtNodeRecord::PublicKey,length)));
  }
#endif
  bool sharedOriginAllowed(const Config &settings,unsigned slot)const {
    if(!codec.originsRestricted())return true;
    PrimaryChannel::Selection key;
    const bool allowed=settings.channelSelection(slot,key)&&publicReady()&&(key.licensedPlaintext||key.publicDefault);
    PrimaryChannel::clear(key);return allowed;
  }
  bool importedPeerAllowed(uint32_t peer)const {
    if(!codec.originsRestricted())return true;
    uint8_t key[32]{};
    const bool allowed=private_&&private_->peerPublicIdentity(peer,key);
    bench::wipeKeyBuffer(key,sizeof(key));return allowed;
  }
  bool publicReady()const {
    return private_&&private_->publicReady(route.identity());
  }
public:
  // Dedicated key-verification bindings for one serialized device owner.
  bool keyVerificationIdentity(uint32_t &node,uint8_t publicKey[32])const {
    node=0;if(!private_||!publicReady()||!publicKey)return false;
    return private_->keyVerificationLocalIdentity(node,publicKey);
  }
  bool keyVerificationPeerKey(uint32_t peer,uint8_t publicKey[32])const {
    return private_&&private_->keyVerificationPeerKey(peer,publicKey);
  }
  bool keyVerificationPeerName(uint32_t peer,char *out,size_t capacity)const {
    if(!out||!capacity)return false;out[0]='\0';
    const int index=nodes.find(peer);const auto *entry=index<0?nullptr:nodes.get(unsigned(index));
    if(!entry||entry->placeholder())return false;
    const uint8_t *name=entry->bytes+NodeCache::Entry::Name;
    const size_t bound=NodeCache::Entry::Short-NodeCache::Entry::Name;
    const void *end=memchr(name,0,bound);if(!end)return false;
    const size_t length=static_cast<const uint8_t *>(end)-name;
    if(length>=capacity)return false;
    memcpy(out,name,length);out[length]='\0';return true;
  }
  bool keyVerificationRandom(uint8_t *out,size_t length) {
    return private_&&private_->keyVerificationRandom(out,length);
  }
  bool keyVerificationSha256(const uint8_t *data,size_t length,uint8_t out[32])const {
    return private_&&private_->keyVerificationSha256(data,length,out);
  }
  int keyVerificationStageCandidate(uint32_t peer,const uint8_t publicKey[32]) {
    if(codec.originsRestricted()) {
      uint8_t pinned[32]{};
      const bool matches=publicKey&&private_&&private_->peerPublicIdentity(peer,pinned)&&!memcmp(pinned,publicKey,sizeof(pinned));
      bench::wipeKeyBuffer(pinned,sizeof(pinned));if(!matches)return -EPERM;
    }
    return private_?private_->keyVerificationStageCandidate(peer,publicKey):-ENOTSUP;
  }
  void keyVerificationClearCandidate(uint32_t peer,const uint8_t publicKey[32]) {
    if(private_)private_->keyVerificationClearCandidate(peer,publicKey);
  }
  void keyVerificationReleasePeer(uint32_t peer,const uint8_t publicKey[32]) {
    if(private_)private_->keyVerificationReleasePeer(peer,publicKey);
  }
  unsigned keyVerificationHopLimit()const{return config.hopLimit();}
  // Dedicated validated control lane. Ordinary submitPrivate remains unchanged
  // and cannot send KEY_VERIFICATION_APP. Every PKI frame is encoded by the
  // provider's existing PkiSession and shared durable NonceAuthority.
  int submitKeyVerification(uint8_t owner,uint32_t peer,uint32_t requestId,
      const uint8_t *data,size_t length,bool pki,const uint8_t *explicitKey,size_t keyLength,
      unsigned hop,bool reliable,uint32_t now,uint32_t random) {
    if(!normalMode_||!private_||!publicReady()||config.licensedOperation())return -EPERM;
    if((owner>=LocalClientCount&&owner!=Endpoint::LocalUiOwner)||!endpointArmed_||!txAllowed_||
       !receiving_||fault_||privatePaused_)
      return -EACCES;
    // Check the exact destination queue before pausing PKI. This makes the
    // transient backpressure result known-not-admitted, before any durable
    // nonce reservation or ciphertext creation can occur.
    if(pki?!endpoint_.privateControlAvailable():!endpoint_.moduleControlAvailable())return -ENOBUFS;
    uint8_t pinned[32]{};
    const bool pinnedPeer=private_->peerPublicIdentity(peer,pinned);
    bench::wipeKeyBuffer(pinned,sizeof(pinned));
    if(codec.originsRestricted()&&!pinnedPeer)return -EPERM;
    if(!pki&&pinnedPeer)return -EKEYREJECTED;
    if(!pausePrivate())return -EBUSY;
    const int result=private_->keyVerificationSend(endpoint_,codec,peer,requestId,data,length,
      pki,explicitKey,keyLength,hop,reliable,now,random);
    return resumePrivate()?result:-EPERM;
  }
  bool takeKeyVerificationReceive(pki::KeyVerificationReceive &out) {
    out={};if(!keyVerificationReceiveReady_)return false;
    out=keyVerificationReceive_;
    volatile uint8_t *bytes=reinterpret_cast<volatile uint8_t *>(&keyVerificationReceive_);
    for(size_t i=0;i<sizeof(keyVerificationReceive_);++i)bytes[i]=0;
    keyVerificationReceiveReady_=false;return true;
  }
private:
  // A refused channel send has no storage work. Keep the receiver running.
  bool pauseSharedOrigin(){return codec.originAllowed()&&pausePrivate();}
  bool pausePrivate() {
    if(privatePaused_||fault_||active_||own_||normalActive_||!radio_.ready()||radio_.busy())return false;
    privateWasReceiving_=receiving_;
    if(!privateWasReceiving_&&radio_.receiveEventPending())return false;
    // The SX126x RX barrier requires an active asynchronous receiver. On a
    // healthy stopped radio there is no receiver to drain or restore. The
    // readiness/busy checks above and the standby health check below still apply.
    const int stopped=privateWasReceiving_?radio_.stopForStorage():0;
    // A completed RX that raced the native barrier is preserved by the
    // driver, with reception already restored. Never discard it for a write.
    if(stopped==-EAGAIN&&privateWasReceiving_&&radio_.health()==1)return false;
    if(stopped){fail(FaultStage::PrivateStop,DiagnosticDomain::Errno,stopped);return false;}
    const int health=radio_.health();
    if(health!=0){fail(FaultStage::PrivateStopHealth,DiagnosticDomain::Health,health);return false;}
    receiving_=false;privatePaused_=true;return true;
  }
  bool resumePrivate() {
    if(!privatePaused_){fail(FaultStage::PrivateState,DiagnosticDomain::State,0);return false;}
    privatePaused_=false;
    if(privateDormant()&&!publicReady()) {
      // A legacy dormant provider has no RF authority. New providers may keep
      // public identity traffic while their private contact set is empty.
      if(privateWasReceiving_||endpointArmed_){fail(FaultStage::PrivateState,DiagnosticDomain::State,0);return false;}
      const int health=radio_.health();
      if(health!=0){fail(FaultStage::PrivateIdleHealth,DiagnosticDomain::Health,health);return false;}
      return true;
    }
    if(!privateReady()&&!publicReady()){fail(FaultStage::PrivateTrust,DiagnosticDomain::State,0);return false;}
    if(privateWasReceiving_) {
      const int restored=radio_.restore();
      if(restored){fail(FaultStage::PrivateRestore,DiagnosticDomain::Errno,restored);return false;}
      const int health=radio_.health();
      if(health!=1){fail(FaultStage::PrivateRestoreHealth,DiagnosticDomain::Health,health);return false;}
      receiving_=true;
    } else {
      const int health=radio_.health();
      if(health!=0){fail(FaultStage::PrivateIdleHealth,DiagnosticDomain::Health,health);return false;}
    }
    return true;
  }
  bool encodeNodeInfo(uint32_t to,uint32_t id,bool wantResponse,uint32_t request,bench::Frame &frame) {
    const auto record=channelNode();
    if(!private_)return Discovery::encode(codec,record,route.identity(),hardwareModel_,to,id,wantResponse,request,frame,uint8_t(config.role()),config.ownerUnmessagable());
    if(!publicReady()||!pauseSharedOrigin())return false;
    const bool encoded=private_->bootstrap(codec,record,hardwareModel_,to,id,wantResponse,request,frame);
    return resumePrivate()&&encoded;
  }
  // This six-part gate is shared by public and private local-origin handlers.
  // Keep a single copy on the small targets, including link-time optimized builds.
  [[gnu::noinline]] bool localEndpointReady()const{
    return normalMode_&&endpointArmed_&&txAllowed_&&receiving_&&!fault_&&!privatePaused_;
  }
  bool (*nodeInfoInterval_)(Service&,uint32_t&) = nullptr;
  bool nodeInfoChannelAllowed() {
    const auto channel=channelUtilization();
    return channel && TxAdmission::channel(channel_.diagnosticSupported(),*channel,false)==TxAdmission::Decision::Allow;
  }
  int queueNodeInfo(uint32_t to,bool wantResponse,uint32_t request,unsigned hop,
                    bool reliable,uint32_t now,uint32_t random,bool interactive=false) {
    uint32_t floorMs=1800000; // Stock unscaled 30-minute floor before the interval owner binds.
    if(endpoint_.nodeInfoPending() || (nodeInfoInterval_ && !nodeInfoInterval_(*this,floorMs)))return -EAGAIN;
    const bool channelAllowed=nodeInfoChannelAllowed();
    const auto licensed=config.licensedOperation();const auto scheduled=bench::MtNodeRecord::intervalMs(config.node().interval());
    const uint32_t historyMs=interactive?60000u:licensed&&scheduled<floorMs?scheduled:floorMs;
    if(!broadcastDue(history::Kind::NodeInfo,historyMs)||!(interactive ? nodeDiscovery.allowed(now,channelAllowed) :
         nodeDiscovery.automaticReply(now,channelAllowed,floorMs,licensed,scheduled)))
      return -EAGAIN;
    wantResponse=stock::nodeinfo::sendOurNodeInfo(wantResponse,config.role());
    const uint32_t id=endpoint_.generatedId(random);bench::Frame f;
    if(!id||!encodeNodeInfo(to,id,wantResponse,request,f))return -EINVAL;
    hop=hopScaling_.limit(hop,to,meshtastic_PortNum_NODEINFO_APP);
    f.bytes[12]=uint8_t(hop|(hop<<5)|((reliable&&to!=0xffffffffu)?8:0));
    const int ret=endpoint_.submitNodeInfoControl(f,reliable,now,private_&&codec.signsPublic(),interactive);
    if(!ret){nodeDiscovery.admitted(now,config.node().interval());broadcastAdmitted(history::Kind::NodeInfo);}
    return ret;
  }
  bool knownUser(uint32_t node)const {
    if(node==route.identity())return true;
    const int i=nodes.find(node);const auto *e=i<0?nullptr:nodes.get(unsigned(i));
    return e&&!e->placeholder()&&!e->ignored();
  }
  bool favoriteIdentity(uint32_t node)const {
    const int i=nodes.find(node);const auto *e=i<0?nullptr:nodes.get(unsigned(i));
    // Pinned NodeDB::isFavorite reads the favorite bit directly. Keep this
    // predicate separate from stricter user/relay eligibility below.
    return e&&e->favorite();
  }
  bool uniqueFavoriteRouterRelay(uint8_t relayByte,uint32_t utcNow)const {
    if(!relayByte)return false; // Stock treats NO_RELAY_NODE as unresolved.
    const uint8_t wanted=relayByte;
    const NodeCache::Entry *resolved=nullptr;
    for(unsigned i=0;i<nodes.count();++i) {
      const auto *e=nodes.get(i);
      // Mirror resolveLastByte's candidate gate and ambiguity order: ignored
      // entries and self are excluded, while a favorite placeholder can still
      // make a matching last byte ambiguous before user-bearing is checked.
      if(!e||e->id()==route.identity()||uint8_t(e->id())!=wanted||e->ignored())continue;
      const bool router=e->role()==2u||e->role()==11u||e->role()==DeviceRolePolicy::Role::CLIENT_BASE;
      int32_t age=int32_t(utcNow-e->lastHeard());if(age<0)age=0;
      const bool direct=e->heard&&e->hops==0&&uint32_t(age)<7200u;
      if(!direct&&!e->favorite()&&!router)continue;
      if(resolved)return false;
      resolved=e;
    }
    return resolved&&!resolved->placeholder()&&!resolved->ignored()&&resolved->favorite()&&
      (resolved->role()==2u||resolved->role()==11u||resolved->role()==DeviceRolePolicy::Role::CLIENT_BASE);
  }
  bool transitAllowed(const uint8_t *p,const bench::RouteMetadata &m)const {
    const auto from=bench::le32(p+4),to=bench::le32(p);
    if(!relaying_||!endpointArmed_||!txAllowed_||from==route.identity()||to==route.identity()||
       m.backendFailed||m.channelAmbiguous||m.signatureRejected)return false;
    const auto mode=config.rebroadcastMode();
    const bool opaque=!m.channelDecoded||m.decodeRejected||(m.signatureLength&&!m.signatureVerified);
    if(!routingPacketAllowed(opaque,p[13],from,to))return false;
    if(opaque)return mode==0||mode==2||mode==3||mode==5;
    if(mode==3&&!knownUser(from))return false;
    if(mode==5&&!corePort(m.portnum))return false;
    if (m.portnum == meshtastic_PortNum_NEIGHBORINFO_APP) {
      // The payload stays untouched, including encrypted or signed announcements.
      // Apply the same module consent and RF-only boundary as other neighbor observations.
      return to==UINT32_MAX && !(p[12]&0x10u) && !m.wantResponse && !m.request_id && !m.reply_id &&
             m.payloadLength && neighborRelayEnabled_ && neighborRelayEnabled_(neighborSnapshotCtx_);
    }
    return true;
  }
  void uplinkOpaquePki(const uint8_t *p,size_t n,uint32_t from,uint32_t to,
                       const bench::RouteMetadata &m,uint32_t utc,uint32_t uptime) {
    if(!mqttPkiTransitSink_||p[13]||to==UINT32_MAX||to==route.identity()||from==route.identity()||
       (p[12]&0x10u)||n<29||n-16>mqtt::MqttDownlinkAdmission::kMaxPrivateCiphertextBytes||
       config.channelCandidates(0)||config.licensedOperation()||nodes.ignored(to)||
       m.channelDecoded||m.channelAmbiguous||m.backendFailed||m.signatureLength||m.signatureRejected||
       m.signatureQuarantined||(config.rebroadcastMode()==3&&(!knownUser(from)||!knownUser(to))))return;
    const uint32_t id=bench::le32(p+8);
    for(const auto &seen:opaqueUplinkSeen_)if(seen.from==from&&seen.id==id)return;
    if(mqttPkiTransitSink_(mqttPkiTransitContext_,p,n,route.identity(),utc?utc:uptime,utc!=0)) {
      opaqueUplinkSeen_[opaqueUplinkNext_]={from,id};
      opaqueUplinkNext_=(opaqueUplinkNext_+1u)%8u;
    }
  }
  bool positionRelayAllowed(unsigned channel, uint8_t hash, const bench::RouteMetadata &m) const {
    unsigned precision = config.relayPositionPrecision(channel);
    // Identical on-air channels can occupy multiple settings slots. Their
    // wire identity cannot distinguish which slot the originator selected.
    const auto candidates = config.channelCandidates(hash);
    for (unsigned i = 0; i < 8; ++i) {
      if (i == channel || !(candidates & (1u << i)) || !sameChannel(channel, i)) continue;
      const unsigned other = config.relayPositionPrecision(i);
      if (other < precision) precision = other;
    }
    return stock::TrafficManagementModule::allowsPositionRelay(m.payload, m.payloadLength, precision);
  }
  bool receiveTransit(const uint8_t *p,size_t n,bench::RouteMetadata &m,int8_t snr,uint32_t now,uint32_t random,
                      bool radioReceive=true,uint32_t utcNow=0) {
    m.mqttIngress=!radioReceive;
    if(!relaying_||!endpointArmed_||!txAllowed_)return false;
    if(m.channelDecoded&&!m.decodeRejected) {
      if(m.signatureLength&&!m.signatureVerified&&!m.signatureRejected&&!m.signatureQuarantined) {
        applyPublicSignature(p,n,m,now);
      }
      if(!packetSignatureAllowed(p,m,now))return false;
    }
    if(!transitAllowed(p,m))return false;
    if (traffic_ && traffic_->configured() && m.channelDecoded && !m.decodeRejected &&
        m.portnum == meshtastic_PortNum_POSITION_APP &&
        bench::le32(p) == UINT32_MAX) {
      const unsigned channel = m.channelContext ? unsigned(m.channelContext - 1) : selectedChannel_;
      if (!positionRelayAllowed(channel, p[13], m)) {
        trafficSuppressed_ = true;
        return false;
      }
    }
    if(radioReceive&&config.role()==DeviceRolePolicy::Role::CLIENT_BASE) {
      const uint32_t from=bench::le32(p+4),to=bench::le32(p);
      const uint8_t hop=p[12]&7,start=p[12]>>5;
      m.clientBaseRole=true;
      m.clientBaseFavoriteTraffic=favoriteIdentity(from)||favoriteIdentity(to);
      // Stock always decrements the first hop. Later-hop preservation requires
      // an unambiguous, user-bearing favorite router-like previous relay.
      m.preserveHopLimit=start&&hop<start&&uniqueFavoriteRouterRelay(p[15],utcNow);
    }
    if(radioReceive&&config.role()==DeviceRolePolicy::Role::ROUTER_LATE) {
      const uint8_t hop=p[12]&7,start=p[12]>>5;
      m.routerLateRole=true;
      // Router-like hop preservation is only for a uniquely resolved,
      // user-bearing favorite relay; the first hop still decrements.
      m.preserveHopLimit=start&&hop<start&&uniqueFavoriteRouterRelay(p[15],utcNow);
    }
    const auto from=bench::le32(p+4),id=bench::le32(p+8);
    const bool decoded=m.channelDecoded&&!m.decodeRejected&&(!m.signatureLength||m.signatureVerified);
    // Keep the shared queue's two history domains separate on an outer-ID
    // collision. Neither kind may overwrite/cancel the other's borrowed job.
    if((decoded&&route.hasOpaqueHistory(from,id))||(!decoded&&route.hasHistory(from,id)))return false;
    if(m.channelDecoded&&m.portnum==70&&!m.signatureLength&&bench::le32(p)!=0xffffffffu) {
      receiveTraceTransit(p,n,m,snr,now,random);return false;
    }
    route.setRxSnr(snr);
    if (!radioReceive && decoded && m.portnum == meshtastic_PortNum_NEIGHBORINFO_APP) return false;
    if(!route.ingestWithMetadata(bench::Protocol::Meshtastic,p,n,m,now,random))return false;
    const auto to=bench::le32(p);
    if(radioReceive&&decoded&&mqttFrameSink_&&from!=route.identity()&&to!=route.identity()&&
       to!=UINT32_MAX&&!(p[12]&0x10u)&&!nodes.ignored(from)&&!nodes.ignored(to)) {
      const unsigned channelIndex=m.channelContext?unsigned(m.channelContext-1):selectedChannel_;
      mqttFrame(p,n,m,channelIndex);
    }
    return true;
  }
  void receiveTraceTransit(const uint8_t *p,size_t n,const bench::RouteMetadata &metadata,
                           int8_t snr,uint32_t now,uint32_t random) {
    const auto from=bench::le32(p+4),to=bench::le32(p),packet=bench::le32(p+8);
    const unsigned hop=p[12]&7,start=(p[12]>>5)&7;
    if(!relaying_||!endpointArmed_||!txAllowed_||p[13]!=codec.channelHash()||!codec.usableConfigured()||
       (p[12]&0x18)||to==0xffffffffu||from==route.identity()||to==route.identity()||
       (start&&p[14]&&p[14]!=uint8_t(route.identity())))return;
    TraceRoute::Discovery semantic{};
    uint8_t payload[TraceRoute::MaxPayload]{},oldData[239]{},newData[239]{},rewritten[255]{};
    size_t payloadSize=0,oldSize=0,newSize=0,rewrittenSize=0;
    if(!TraceRoute::semantic(metadata,semantic)||
       !TraceRoute::data(metadata,metadata.payload,metadata.payloadLength,oldData,sizeof(oldData),oldSize))return;
    const auto result=TraceRoute::transit(metadata,route.identity(),hop,start,snr,
                                          payload,sizeof(payload),payloadSize);
    if(result==TraceRoute::TransitResult::AlreadyVisited) {
      // A loop observation is never routed, even if a prior history entry and
      // origin retry interval exist. Still prove exact canonical Data so
      // unsupported outer fields cannot hide behind the local route ID.
      expireTraceAssociations(now);route.expire(now);
      const auto *association=traceAssociation(from,packet);
      if(!association||!matchesTraceLoopAssociation(*association,p,metadata,semantic,start)||
         !codec.validateTraceRouteData(p,n,oldData,oldSize)) {
        bench::wipeKeyBuffer(oldData,sizeof(oldData));return;
      }
      ++traceLoopObservations;
      bench::wipeKeyBuffer(oldData,sizeof(oldData));return;
    }
    if(result!=TraceRoute::TransitResult::Mutated)return;
    bench::RouteMetadata rewrittenMetadata=metadata;
    TraceRoute::Discovery outgoing{};
    if(!TraceRoute::data(metadata,payload,payloadSize,newData,sizeof(newData),newSize)||
       !codec.rewriteTraceRouteData(p,n,oldData,oldSize,newData,newSize,
                                    rewritten,sizeof(rewritten),rewrittenSize)||
       !codec.decode(rewritten,rewrittenSize,rewrittenMetadata)||
       !rewrittenMetadata.channelDecoded||!rewrittenMetadata.traceRouteValid||
       !TraceRoute::semantic(rewrittenMetadata,outgoing))return;
    expireTraceAssociations(now);route.expire(now);
    rewrittenMetadata.channelContext=metadata.channelContext;
    auto *association=traceAssociation(from,packet);
    if(association&&!matchesTraceAssociation(*association,p,metadata,semantic,start))return;
    if(!association&&route.hasHistory(from,packet))return;
    auto *slot=association?association:freeTraceAssociation(now);
    if(!slot)return;
    // Traceroute payloads are unauthenticated. Preserve duplicate/correlation
    // history but suppress generic request_id route learning/cancellation.
    rewrittenMetadata.request_id=0;
    route.setRxSnr(snr);
    bool admitted=false;
    if(association)admitted=route.upgradeTraceVariant(rewritten,rewrittenSize,rewrittenMetadata,now);
    if(!admitted)admitted=route.ingestWithMetadata(bench::Protocol::Meshtastic,rewritten,rewrittenSize,
                                                   rewrittenMetadata,now,random);
    if(admitted&&!association) {
      slot->incoming=semantic;slot->outgoing=outgoing;slot->from=from;slot->id=packet;slot->to=to;
      slot->request=metadata.request_id;slot->at=now;slot->channel=p[13];slot->channelSlot=selectedChannel_;
      slot->start=uint8_t(start);slot->next=p[14];slot->response=!metadata.wantResponse;slot->used=true;
    } else if(admitted){association->outgoing=outgoing;association->at=now;}
    bench::wipeKeyBuffer(payload,sizeof(payload));bench::wipeKeyBuffer(oldData,sizeof(oldData));
    bench::wipeKeyBuffer(newData,sizeof(newData));bench::wipeKeyBuffer(rewritten,sizeof(rewritten));
    bench::wipeKeyBuffer(&rewrittenMetadata,sizeof(rewrittenMetadata));
  }
#if defined(CONFIG_MT_RF_ADMIN_GET)
  void processPrivateAdminMutation(uint32_t now,uint32_t random) {
    auto &slot=privateAdminMutation_;
    if(slot.state!=PendingPrivateAdminMutation::State::Queued||privateAdminMutationProcessing_)return;
    if(uint32_t(now-slot.receivedAt)>=PrivateAdminMutationLifetimeMs||
       !privateAdminMutationFn_||!privateAdminPeerCurrent(slot.from,slot.key)) {
      clearPrivateAdminMutation();return;
    }
    if(!receiving_||privatePaused_||fault_||!endpointArmed_||!txAllowed_||
       active_||own_||normalActive_||endpointActive_||!privateReady()||
       radio_.health()!=1||radio_.busy())return;

    // Move the bounded request to a wiped stack copy and clear the shared slot
    // before entering Companion. A typed setter may stop/restart Service;
    // stop must not erase memory currently borrowed by the callback.
    PendingPrivateAdminMutation request=slot;
    clearPrivateAdminMutation();
    privateAdminMutationProcessing_=true;
    struct ProcessingGuard {bool &active;~ProcessingGuard(){active=false;}} processing{privateAdminMutationProcessing_};
    struct RequestWipe {PendingPrivateAdminMutation &value;~RequestWipe(){bench::wipeKeyBuffer(&value,sizeof(value));}} wipe{request};
    uint32_t routingError=0;
    const bool handled=privateAdminMutationFn_(privateAdminMutationCtx_,request.local,
        request.from,request.key,request.request,request.data,request.size,now,routingError);
    if(!handled||!validPrivateAdminRoutingError(routingError)||
       !privateAdminPeerCurrent(request.from,request.key)||!receiving_||privatePaused_||
       fault_||!endpointArmed_||!txAllowed_||active_||own_||normalActive_||
       endpointActive_||radio_.health()!=1||radio_.busy()) {
      return;
    }

    // The callback has returned before this separate pause. Fresh durable TX
    // identity reservation and exact-peer PKI response encoding happen here.
    const bool paused=pausePrivate();
    bool queued=false;
    if(paused&&privateReady()&&privateAdminPeerCurrent(request.from,request.key))
      queued=private_->respondPrivateAdminMutation(endpoint_,request.local,request.from,request.key,
          request.request,request.responseHop,request.reliable,routingError,now,random);
    const bool resumed=paused&&resumePrivate();
    if(queued&&resumed)observeAdminResponseQueued(true,request.from,request.request);
  }
  bool legacyAdminRequestCurrent(const admin::LegacyAdminRequest &request,
                                 uint32_t now)const {
    return normalMode_&&receiving_&&!privatePaused_&&!fault_&&endpointArmed_&&txAllowed_&&
      request.state_==admin::LegacyAdminRequest::State::Queued&&
      uint32_t(now-request.receivedAt_)<LegacyAdminRequestLifetimeMs&&
      legacyAdminChannelCurrent(request.channelIndex_,request.channelHash_,
        request.domainEpoch_,request.key_,request.keySize_,request.role_,
        request.name_,request.from_);
  }
  bool sendLegacyAdminResponse(uint32_t to,uint32_t requestPacketId,
      uint8_t channelIndex,uint8_t channelHash,uint32_t domainEpoch,
      uint8_t responseHop,bool reliable,
      const uint8_t *response,size_t responseSize,uint32_t now,uint32_t random) {
    if(!private_||!publicReady()||!endpointArmed_||!txAllowed_||fault_||
       !receiving_||privatePaused_||active_||own_||normalActive_||endpointActive_||
       !storageStopSafe()||radio_.health()!=1||radio_.busy()||
       !endpoint_.moduleControlAvailable()||
       !admin::legacy_protocol::canonicalResponse(response,responseSize,requestPacketId))return false;
    LegacyAdminChannelSnapshot before{};
    const bool selected=legacyAdminChannelSnapshot(config,channelIndex,before)&&
      before.hash==channelHash&&config.adminChannelEnabled()&&!config.licensedOperation()&&
      domainEpoch==legacyAdminDomainEpoch_;
    bench::wipeKeyBuffer(&before,sizeof(before));if(!selected)return false;
    ChannelScope channel(*this,channelIndex);if(!channel.valid())return false;
    uint32_t id=endpoint_.generatedId(random);if(!id)return false;
    bench::Frame frame{};
    const bool paused=pauseSharedOrigin();
    bool encoded=false;
    if(paused&&publicReady())
      encoded=private_->encodeChannelControl(codec,to,id,response,responseSize,frame);
    const bool resumed=paused&&resumePrivate();
    if(!encoded||!resumed||!receiving_||fault_||!endpointArmed_||!txAllowed_||
       !endpoint_.moduleControlAvailable()) {
      bench::wipeKeyBuffer(&frame,sizeof(frame));return false;
    }
    if(responseHop>HopPolicy::Maximum) {
      bench::wipeKeyBuffer(&frame,sizeof(frame));return false;
    }
    frame.bytes[12]=uint8_t(responseHop|(responseHop<<5)|(reliable?8u:0u));
    const int submitted=endpoint_.submitModuleControl(frame,now,false,false,false,
                                                       reliable,requestPacketId);
    if(submitted==0){legacyAdminReplyQueued_=true;
      observeAdminResponseQueued(false,to,requestPacketId);}
    bench::wipeKeyBuffer(&frame,sizeof(frame));return submitted==0;
  }
  static bool encodeLegacyAdminRoutingError(uint32_t requestPacketId,uint32_t error,
      uint8_t *out,size_t capacity,size_t &size) {
    size=0;if(!requestPacketId||!out||
      !(error<=9||(error>=32&&error<=39)))return false;
    meshtastic_Routing routing=meshtastic_Routing_init_zero;
    routing.which_variant=meshtastic_Routing_error_reason_tag;
    routing.error_reason=static_cast<meshtastic_Routing_Error>(error);
    uint8_t payload[admin::remote_protocol::MaxPayloadBytes]{};
    auto routingOut=pb_ostream_from_buffer(payload,sizeof(payload));
    const bool routingOk=pb_encode(&routingOut,meshtastic_Routing_fields,&routing);
    bench::wipeKeyBuffer(&routing,sizeof(routing));
    if(!routingOk||routingOut.bytes_written>sizeof(payload)) {
      bench::wipeKeyBuffer(payload,sizeof(payload));return false;
    }
    meshtastic_Data data=meshtastic_Data_init_zero;
    data.portnum=meshtastic_PortNum_ROUTING_APP;
    data.payload.size=pb_size_t(routingOut.bytes_written);
    memcpy(data.payload.bytes,payload,routingOut.bytes_written);
    data.request_id=requestPacketId;data.has_bitfield=true;data.bitfield=0;
    auto dataOut=pb_ostream_from_buffer(out,capacity);
    const bool encoded=pb_encode(&dataOut,meshtastic_Data_fields,&data);
    bench::wipeKeyBuffer(&data,sizeof(data));bench::wipeKeyBuffer(payload,sizeof(payload));
    if(!encoded)return false;
    size=dataOut.bytes_written;
    return admin::legacy_protocol::canonicalResponse(out,size,requestPacketId);
  }
  void processLegacyAdmin(uint32_t now,uint32_t random) {
    if(legacyAdminRequest_.state_==admin::LegacyAdminRequest::State::Queued) {
      if(uint32_t(now-legacyAdminRequest_.receivedAt_)>=LegacyAdminRequestLifetimeMs||
         !legacyAdminHandler_||!legacyAdminRequestCurrent(legacyAdminRequest_,now)) {
        clearLegacyAdminRequest();return;
      }
      if(!endpointArmed_||!txAllowed_||!receiving_||privatePaused_||fault_||
         active_||own_||normalActive_||endpointActive_||!storageStopSafe()||
         radio_.health()!=1||radio_.busy()||!private_||!publicReady()||
         !endpoint_.moduleControlAvailable()||privateAdminMutationQueued()||
         privateAdminQueries_.size()!=0)return;
      admin::LegacyAdminRequest request;request.copyFrom(legacyAdminRequest_);
      clearLegacyAdminRequest();legacyAdminProcessing_=true;activeLegacyAdminRequest_=&request;
      uint8_t response[admin::remote_protocol::MaxDataBytes]{};size_t responseSize=0;
      const bool handled=legacyAdminHandler_(legacyAdminContext_,request,now,
          response,sizeof(response),responseSize);
      const bool contextStillCurrent=currentLegacyAdminRequest(request)&&
        legacyAdminRequestCurrent(request,now);
      activeLegacyAdminRequest_=nullptr;legacyAdminProcessing_=false;
      bool replyValid=handled&&contextStillCurrent&&responseSize<=sizeof(response)&&
        admin::legacy_protocol::canonicalResponse(response,responseSize,request.packetId_);
      if(replyValid&&request.kind_==admin::LegacyAdminRequest::Kind::Set) {
        uint32_t routingError=0;
        replyValid=legacyAdminRoutingOutcome(response,responseSize,request.packetId_,routingError);
        if(replyValid)rememberCompletedLegacyAdminSet(request.from_,request.packetId_,
          request.domainEpoch_,now,routingError);
      }
      if(replyValid)
        (void)sendLegacyAdminResponse(request.from_,request.packetId_,request.channelIndex_,
          request.channelHash_,request.domainEpoch_,request.responseHop_,request.reliable_,
          response,responseSize,now,random);
      bench::wipeKeyBuffer(response,sizeof(response));
      return;
    }
    if(!legacyAdminBusy_.ready)return;
    if(uint32_t(now-legacyAdminBusy_.receivedAt)>=LegacyAdminRequestLifetimeMs||
       !currentLegacyBusyChannel(legacyAdminBusy_)) {
      bench::wipeKeyBuffer(&legacyAdminBusy_,sizeof(legacyAdminBusy_));return;
    }
    if(!endpointArmed_||!txAllowed_||!receiving_||privatePaused_||fault_||
       active_||own_||normalActive_||endpointActive_||!storageStopSafe()||
       radio_.health()!=1||radio_.busy()||!private_||!publicReady()||
       !endpoint_.moduleControlAvailable()||privateAdminMutationQueued()||
       privateAdminQueries_.size()!=0)return;
    auto request=legacyAdminBusy_;bench::wipeKeyBuffer(&legacyAdminBusy_,sizeof(legacyAdminBusy_));
    uint32_t completedError=0;
    if(completedLegacyAdminSet(request.from,request.packetId,request.domainEpoch,now,completedError))
      request.routingError=completedError;
    uint8_t response[admin::remote_protocol::MaxDataBytes]{};size_t responseSize=0;
    if(encodeLegacyAdminRoutingError(request.packetId,request.routingError,
          response,sizeof(response),responseSize))
      (void)sendLegacyAdminResponse(request.from,request.packetId,request.channelIndex,
        request.channelHash,request.domainEpoch,request.responseHop,request.reliable,
        response,responseSize,now,random);
    bench::wipeKeyBuffer(response,sizeof(response));
  }
#endif
  void pollNormal(uint32_t now,uint32_t random) {
    if(hopScaling_.due(now)) {
      hopScaling_.seed(uint16_t(random));
      const auto utilization=channel_.smoothedPercent();
      hopScaling_.runOnce(now,utilization?*utilization:0.0f,config.role());
    }
#if defined(CONFIG_MT_RF_ADMIN_GET)
    legacyAdminNow_=now;
    privateAdminQueries_.expire(now);
    if(!privateReady()){privateAdminQueries_.clear();clearPrivateAdminMutation();}
    if(!publicReady()) {
      clearLegacyAdminRequest();bench::wipeKeyBuffer(&legacyAdminBusy_,sizeof(legacyAdminBusy_));
      clearCompletedLegacyAdminSets();
    }
#endif
    if(private_) {
      if(!privateReady())private_->clearEvent();
      if(!publicReady()){endpoint_.cancelPrivate();bootstrapRequest_={};}
      else {private_->sweep(endpoint_);
#if defined(CONFIG_MT_RF_ADMIN_GET)
        if(!privateReady())clearPrivateAdminMutation();
#endif
      }
    }
    // Signed replies run after RX returns, so its decoded metadata/scratch does
    // not remain on the 8KiB main stack across the signer/curve call chain.
    if(bootstrapRequest_.ready) {
      if(bench::due(now,bootstrapRequest_.expires)||!endpointArmed_||!txAllowed_)bootstrapRequest_={};
      else if(!active_&&!radio_.busy()) {
        const auto request=bootstrapRequest_;bootstrapRequest_={};
        ChannelScope channel(*this,request.channel);
        if(channel.valid()&&!nodes.ignored(request.to)&&(request.verified||nodeDiscovery.replyAllowed(request.to,now))&&
          !queueNodeInfo(request.to,false,request.request,request.hop,request.reliable,now,random))nodeDiscovery.replied(request.to,now,request.verified);
        else ++nodeDiscovery.limited;
      }
    }
    nodeDiscovery.observeArm(config.role()!=DeviceRolePolicy::Role::CLIENT_HIDDEN&&!configurationTransition_&&normalDiscovery_&&endpointArmed_&&txAllowed_,now);
    if(!configurationTransition_ && nodeDiscovery.periodic(now) &&
       (transmitDutyAdmission(false)!=TxAdmission::Decision::Allow ||
        queueNodeInfo(0xffffffffu,false,0,config.hopLimit(),false,now,random)))nodeDiscovery.defer(now);
    endpoint_.tick(now,random);
    if(active_) {
      int result=0;
      if(!radio_.completion(result)){if(bench::due(now,deadline_)){++failed;noteTxUncertain();fail(FaultStage::CompletionTimeout,DiagnosticDomain::Errno,-ETIMEDOUT);}return;}
      active_=false;quiet_=now+250;
      // Known terminal completion: a clean result is credited exactly once
      // before the RX-restore check, so a known duration survives a failed
      // restore. An error status is an uncertain outcome (possible early
      // abort), tracked and never credited.
      if(result)noteTxUncertain();else creditTxPending(bench::le32(activeFrame_.bytes+4)!=route.identity());
      FaultStage failureStage=FaultStage::CompletionStatus;DiagnosticDomain failureDomain=DiagnosticDomain::Errno;
      int failureValue=result;
      if(!failureValue){failureValue=radio_.restore();failureStage=FaultStage::Restore;}
      if(!failureValue){const int health=radio_.health();if(health!=1){failureValue=health;failureStage=FaultStage::RestoreHealth;failureDomain=DiagnosticDomain::Health;}}
      if(result||failureStage==FaultStage::RestoreHealth||failureValue){
        if(normalTransitActive_)route.complete(activeFrame_,now,false);
        normalTransitActive_=false;++failed;fail(failureStage,failureDomain,failureValue);return;
      }
      ++sent;
      // Only locally originated successful TX completion is an uplink event.
      // Relays have their RX event; retries are deduplicated by the bridge.
      if(!normalTransitActive_&&normalActive_&&
         bench::le32(activeFrame_.bytes+4)==route.identity()) {
        if(normalActive_->privateMessage&&!normalActive_->signedPublic) {
          // Endpoint owns four jobs plus four private-control ACK slots.
          if(mqttPrivateTxSink_&&firstPrivateTxNotice(normalActive_))
            mqttPrivateTxSink_(mqttPrivateTxContext_,activeFrame_.bytes,
                               activeFrame_.len,route.identity());
        } else {
          mqttFrame(activeFrame_.bytes,activeFrame_.len);
        }
      }
      if(normalTransitActive_)route.complete(activeFrame_,now,true);
      else endpoint_.complete(*normalActive_,now,random);
      normalActive_=nullptr;normalTransitActive_=false;
    }
    const int idleHealth=radio_.health();
    if(idleHealth!=1){fail(FaultStage::IdleHealth,DiagnosticDomain::Health,idleHealth);return;}
#if defined(CONFIG_MT_RF_ADMIN_GET)
    if(!configurationTransition_)processPrivateAdminMutation(now,random);
    if(!configurationTransition_)processLegacyAdmin(now,random);
    if(fault_||!receiving_)return;
#endif
    if(!endpointArmed_||!txAllowed_||!bench::due(now,quiet_)||radio_.busy())return;
    auto *j=endpoint_.next(now);bench::Frame *transit=j?nullptr:route.next(now);
    if(!j&&!transit)return;
    if(j&&codec.originsRestricted()&&
       !(j->privateMessage?importedPeerAllowed(bench::le32(j->frame.bytes)):sharedOriginAllowed(config,j->channelSlot))) {
      endpoint_.refused(*j,4);return;
    }
    if(j&&((j->privateMessage&&!privateReady())||(private_&&(!publicReady()||nodes.ignored(bench::le32(j->frame.bytes)))))) {
      noteOriginReject(5,DiagnosticDomain::Routing,4);endpoint_.refused(*j,4);return;
    }
    const auto air=radio_.airtime(j?j->frame.len:transit->len);
    RadioTiming::Deadline timing;
    if(!RadioTiming::resolve(air,timing)){
      if(j)endpoint_.refused(*j,7);else {++failed;route.complete(*transit,now,false);}
      return;
    }
    // Stock NodeInfo and common send paths use the maximum threshold; position
    // responses are polite except for trackers. Unknown observations keep work queued.
    const bool tracker=config.role()==meshtastic_Config_DeviceConfig_Role_TRACKER||config.role()==meshtastic_Config_DeviceConfig_Role_TAK_TRACKER;
    const bool polite=j&&j->positionResponse&&!tracker;
    if(normalAdmission(polite)!=TxAdmission::Decision::Allow){
      ++throttled;quiet_=now+250;if(transit)route.defer(*transit,now,random);return;
    }
    if(j) {
      // Canonicalize equal channel slots exactly as decodeChannel does so
      // origin-response evidence cannot cross colliding channel contexts.
      unsigned equivalent=j->channelSlot;
      for(unsigned i=0;i<j->channelSlot;++i)if(sameChannel(i,j->channelSlot)){equivalent=i;break;}
      route.prepareOrigin(j->frame,uint8_t(equivalent+1),now);
      activeFrame_=j->frame;
    }
    else {route.noteTransmitStart(*transit);activeFrame_=*transit;transit->used=false;++activeFrame_.attempts;}
    // Stable buffer until native completion.
    const int ret=radio_.send(activeFrame_.bytes,activeFrame_.len);
    quiet_=now+250;
    if(j)endpoint_.started(*j,ret,now,air,random);
    if(!ret){
      if(j)route.noteOriginTransmit(activeFrame_,now);
      active_=true;normalActive_=j;normalTransitActive_=transit!=nullptr;deadline_=now+timing.hostMs;
      if(j)endpoint_.blocked(air+250);else route.noteBlockedAirtime(air+250);
      noteTxAcceptedStart(air);
      return;
    }
    if(transit) {
      if(ret==-EBUSY) {
        const int restored=radio_.restore();
        const int health=restored?0:radio_.health();
        if(restored||health!=1){++failed;fail(restored?FaultStage::BusyRestore:FaultStage::BusyRestoreHealth,restored?DiagnosticDomain::Errno:DiagnosticDomain::Health,restored?restored:health);return;}
        if(route.retry(activeFrame_,now))return;
        route.retireRetryRefusal(activeFrame_);
        normalActive_=nullptr;normalTransitActive_=false;++failed;return;
      }
      route.complete(activeFrame_,now,false);++failed;fail(FaultStage::Send,DiagnosticDomain::Errno,ret);return;
    }
    if(ret!=-EBUSY){++failed;fail(FaultStage::Send,DiagnosticDomain::Errno,ret);return;}
    const int restored=radio_.restore();
    if(restored){++failed;fail(FaultStage::BusyRestore,DiagnosticDomain::Errno,restored);return;}
    const int health=radio_.health();
    if(health!=1){++failed;fail(FaultStage::BusyRestoreHealth,DiagnosticDomain::Health,health);}
  }
public:
  // Values match stock COMPATIBLE=0, BALANCED=1, STRICT=2. The licensed
  // input is the generated local User.is_licensed value, not licensed RF mode.
  int setPacketSignaturePolicy(uint32_t raw,bool /*licensed*/) {
    if(raw>2)return -EINVAL;
    if(receiving_||active_)return -EBUSY;
    packetSignaturePolicy_=uint8_t(raw);codec.setPublicSigning(raw!=0);return 0;
  }
  Config config;
  NodeCache nodes;
  companion::SatelliteReplay satellites;
  bench::RoutingCore route;
  bench::MtCodec codec;
  bench::MtSignature signature;
  bench::MtNodeInfo discovery;
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
  history::TransmitHistory *broadcastHistory=nullptr;
  int (*saveBeaconHistory)()=nullptr;
#endif
  bool broadcastDue(history::Kind kind,uint64_t interval)const{
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
    if(broadcastHistory)return broadcastHistory->allows(kind,interval);
#else
    (void)kind;(void)interval;
#endif
    return true;
  }
  void broadcastAdmitted(history::Kind kind){
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
    if(broadcastHistory)broadcastHistory->record(kind);
#else
    (void)kind;
#endif
  }
  Discovery nodeDiscovery;
  void nodeInfoIntervalProvider(bool (*provider)(Service&,uint32_t&)){nodeInfoInterval_=provider;}
  uint32_t received=0,rejected=0,failed=0,sent=0,throttled=0,traceLoopObservations=0;
  AirtimeMeter airtime_;
  ChannelMeter channel_;
private:
  stock::HopScalingModule hopScaling_;
  void sampleHopDistance(bool rf,uint32_t source,uint8_t flags,uint32_t random) {
    if(!rf || (flags&0x10u) || source==route.identity())return;
    hopScaling_.seed(uint16_t(random));
    hopScaling_.samplePacketForHistogram(source,(flags>>5)-(flags&7u));
  }
public:
#if defined(CONFIG_MT_HOP_HISTORY)
  stock::HopScalingModule &hopHistory(){return hopScaling_;}
#endif
  void noteTxUncertain(){airtime_.noteUncertain();channel_.noteUncertain();}
  void creditTxPending(bool relay=false){
    // A known clean terminal completion is counted once, even if clock or
    // subsequent RX restoration fails. Canceled/uncertain TX is never credited.
    if(airtime_.pending()){
      if(completedPacketsTx_<UINT32_MAX)++completedPacketsTx_;
      if(relay&&completedRelays_<UINT32_MAX)++completedRelays_;
    }
    airtime_.creditPending();channel_.creditPending();
    if(normalMode_)updateRouteUtilization();
  }
  void noteTxAcceptedStart(uint32_t air){airtime_.noteAcceptedStart(air);channel_.noteAcceptedStart(air);}

  unsigned channelIndex()const override{return selectedChannel_;}
  bool positionSharingAllowed()const override{return config.positionPrecision(selectedChannel_)!=0;}
  bool positionReplyData(const Config &settings,const meshtastic_Position &live,
                         unsigned channel,uint32_t request,uint8_t *out,
                         size_t capacity,size_t &size)const override {
    size=0;
    if(settings.role()==10) {
#if defined(CONFIG_MT_TAK_TRACKER)
      // The caller supplies the actual disclosure channel: public receive
      // scope for channel replies, channel zero for authenticated PKI replies.
      // Encryption and durable IDs remain in their existing transport owners.
      if(!takReady_||!request||!out||!capacity||!settings.positionSharing())return false;
      const unsigned precision=settings.positionPrecision(channel);
      meshtastic_Position position{};
      if(!precision||!PositionQuery::localPosition(settings,&live,position))return false;
      PositionPrecision::apply(position,precision);
      if(position.latitude_i < -900000000||position.latitude_i>900000000||
         position.longitude_i < -1800000000||position.longitude_i>1800000000)return false;
      const auto owner=settings.node();char name[25]{};
      memcpy(name,owner.bytes+bench::MtNodeRecord::Name,24);
      const uint8_t battery=hardware_.cellPercent&&*hardware_.cellPercent<=100?*hardware_.cellPercent:0;
      const tak_pli::Inputs inputs{&position,name,route.identity(),battery,settings.fixedPosition(),true,
        static_cast<meshtastic_Team>(takTeam_),static_cast<meshtastic_MemberRole>(takRole_)};
      return tak_pli_reply::encode(inputs,request,out,capacity,size)==tak_pli_reply::Result::Ok;
#else
      return false;
#endif
    }
    return PositionQuery::fixedResponse(settings,request,out,capacity,size,channel,&live);
  }
  bool sameChannel(unsigned a,unsigned b)const override {
    PrimaryChannel::Selection left,right;
    const bool equal=config.channelSelection(a,left)&&config.channelSelection(b,right)&&
      left.hash==right.hash&&left.aead==right.aead&&left.licensedPlaintext==right.licensedPlaintext&&
      left.length==right.length&&!memcmp(left.bytes,right.bytes,left.length);
    PrimaryChannel::clear(left);PrimaryChannel::clear(right);return equal;
  }
  unsigned selectedChannel()const{return selectedChannel_;}
  uint32_t waypointDomainEpoch()const{return waypointDomainEpoch_;}
#if defined(CONFIG_MT_GPIO_MODULES)
  bool bindGpioFrameSink(GpioFrameSink sink,void *context) {
    if(!sink||gpioFrameSink_||receiving_||active_)return false;
    gpioFrameSink_=sink;gpioFrameContext_=context;return true;
  }
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  uint32_t storeForwardEpoch()const{return storeForwardEpoch_;}
  bool bindStoreForwardSink(MqttFrameSink sink,void *context){
    if(!sink||storeForwardSink_||receiving_||active_)return false;
    storeForwardSink_=sink;storeForwardContext_=context;return true;
  }
  bool bindStoreForwardArchiveProvider(StoreForwardArchiveProvider provider,void *context){
    if(!provider||storeForwardArchiveProvider_)return false;
    storeForwardArchiveProvider_=provider;storeForwardArchiveContext_=context;return true;
  }
  bool bindStoreForwardLocalTextSink(StoreForwardLocalTextSink sink,void *context){
    if(!sink||storeForwardLocalTextSink_)return false;
    storeForwardLocalTextSink_=sink;storeForwardLocalTextContext_=context;return true;
  }
  bool archiveStoreForwardLocalText(const modules::StoreForwardArchive::MessageView &view){
    return storeForwardLocalTextSink_&&storeForwardLocalTextSink_(storeForwardLocalTextContext_,view);
  }
  bool storeForwardLocalDispatchReady()const{
    return localEndpointReady();
  }
  const modules::StoreForwardArchive *storeForwardArchive()const{
    return storeForwardArchiveProvider_?storeForwardArchiveProvider_(storeForwardArchiveContext_):nullptr;
  }
#endif
  bool bindMqttFrameSink(MqttFrameSink sink,void *context) {
    if(!sink||mqttFrameSink_||receiving_||active_)return false;
    mqttFrameSink_=sink;mqttFrameContext_=context;return true;
  }
  bool bindMqttPrivateTxSink(MqttPrivateTxSink sink,void *context) {
    if(!sink||mqttPrivateTxSink_||receiving_||active_)return false;
    mqttPrivateTxSink_=sink;mqttPrivateTxContext_=context;return true;
  }
  bool bindMqttLocalRxSink(MqttLocalRxSink sink,void *context) {
    if(!sink||mqttLocalRxSink_||receiving_||active_)return false;
    mqttLocalRxSink_=sink;mqttLocalRxContext_=context;return true;
  }
  bool bindMqttPkiTransitSink(MqttPkiTransitSink sink,void *context) {
    if(!sink||mqttPkiTransitSink_||receiving_||active_)return false;
    mqttPkiTransitSink_=sink;mqttPkiTransitContext_=context;return true;
  }
  bool bindHotNodeObservationSink(HotNodeObservationSink sink,void *context) {
    if(!sink||hotNodeObservationSink_||receiving_||active_)return false;
    hotNodeObservationSink_=sink;hotNodeObservationContext_=context;return true;
  }
#if defined(CONFIG_MT_PAXCOUNTER)
  bool localPaxSnapshot(meshtastic_Paxcount &out){return endpoint_.localPaxSnapshot(out);}
  bool bindPaxSnapshot(bool (*provider)(void *,meshtastic_Paxcount &),void *ctx) {
    return !receiving_ && !privatePaused_ && endpoint_.bindPaxSnapshot(provider,ctx);
  }
#endif
  bool bindNeighborSnapshot(NeighborSnapshotFn fn,void *ctx,bool (*relayEnabled)(void *)=nullptr) {
    if(!fn||neighborSnapshotFn_||receiving_||privatePaused_)return false;
    neighborSnapshotFn_=fn;neighborSnapshotCtx_=ctx;neighborRelayEnabled_=relayEnabled;return true;
  }
  // The provider reads RAM while the caller already owns Service serialization.
  bool neighborSnapshot(meshtastic_NeighborInfo &out) override {
    out=meshtastic_NeighborInfo_init_zero;
    if(!neighborSnapshotFn_)return false;
    if(neighborSnapshotFn_(neighborSnapshotCtx_,out))return true;
    out=meshtastic_NeighborInfo_init_zero;return false;
  }
#if defined(CONFIG_MT_RF_ADMIN_GET)
  // Boot-bound, single-assignment provider. The callback is invoked only
  // synchronously by PkiRuntime on authenticated, Fresh, replay-committed RF.
  bool bindPrivateAdminSnapshot(PrivateAdminSnapshotFn fn,void *ctx) {
    if(!fn||privateAdminSnapshotFn_||receiving_||privatePaused_)return false;
    privateAdminSnapshotFn_=fn;privateAdminSnapshotCtx_=ctx;return true;
  }
  // The owner callback is boot-bound and never replaced after RX starts.
  bool bindPrivateAdminMutation(PrivateAdminMutationFn fn,void *ctx) {
    if(!fn||privateAdminMutationFn_||receiving_||privatePaused_)return false;
    privateAdminMutationFn_=fn;privateAdminMutationCtx_=ctx;return true;
  }
  // Boot-bound owner for the separately opt-in named shared-PSK Admin lane.
  // Companion callbacks run only from pollNormal, never from RF receive.
  bool setLegacyAdminHandler(admin::LegacyAdminHandler handler,void *context) {
    if(!handler||legacyAdminHandler_||receiving_||privatePaused_||active_||fault_)return false;
    legacyAdminHandler_=handler;legacyAdminContext_=context;return true;
  }
  bool bindAdminResponseQueued(AdminResponseQueuedFn observer,void *context) {
    if(!observer||adminResponseQueuedFn_||receiving_||privatePaused_)return false;
    adminResponseQueuedFn_=observer;adminResponseQueuedContext_=context;return true;
  }
  bool privateAdminWorkQueued()const {
    return privateAdminMutationQueued()||endpoint_.privateControlPending();
  }
  // Excludes the callback's own stack copy, which was removed from its slot
  // before dispatch. A core-profile transition must not strand other owners.
  bool adminRequestsQueued()const {
    return privateAdminMutationQueued()||
      legacyAdminRequest_.state_==admin::LegacyAdminRequest::State::Queued||legacyAdminBusy_.ready;
  }
  uint32_t legacyAdminDomainEpoch()const{return legacyAdminDomainEpoch_;}
  bool legacyAdminWorkQueued()const {
    return legacyAdminProcessing_||legacyAdminRequest_.state_==admin::LegacyAdminRequest::State::Queued||
           legacyAdminBusy_.ready||(legacyAdminReplyQueued_&&endpoint_.modulePending());
  }
  // This unforgeable request-context proof succeeds only while Service is
  // synchronously dispatching the exact queue-owned object into its handler.
  bool currentLegacyAdminRequest(const admin::LegacyAdminRequest &request)const {
    return legacyAdminProcessing_&&activeLegacyAdminRequest_==&request&&
      legacyAdminRequestCurrent(request,legacyAdminNow_);
  }
  // This is a read-only current-pin check for the owner callback immediately
  // before authorization/application. It does not acquire the RX storage pause.
  bool privateAdminPeerCurrent(uint32_t peer,const uint8_t key[32])const {
    if(!privateReady()||peer<=1||peer>=0xfffffffeu||!key||nodes.ignored(peer)||
       !private_->matchesPeer(peer))return false;
    uint8_t current[32]{};
    const bool exact=private_->peerPublicIdentity(peer,current)&&!memcmp(current,key,32);
    bench::wipeKeyBuffer(current,sizeof(current));return exact;
  }
  // PkiRuntime calls only after authenticated Fresh replay commit while the
  // radio/storage pause is held. This stores one canonical bounded Admin SET;
  // no callback, setter, or RF response is invoked here.
  bool privateAdminMutationRequest(uint32_t local,uint32_t from,const uint8_t key[32],
      uint32_t request,uint8_t responseHop,bool reliable,const uint8_t *data,
      size_t size,uint32_t now)override {
    if(!privateAdminMutationFn_||local!=route.identity()||from<=1||from>=0xfffffffeu||
       from==local||!key||!request||responseHop>HopPolicy::Maximum||!privatePaused_||
       receiving_||fault_||privateAdminMutationProcessing_||!privateReady()||nodes.ignored(from)||!data||
       size==0||size>admin::remote_protocol::MaxDataBytes)return false;
    admin::mutation_protocol::Mutation mutation{};
    if(!admin::mutation_protocol::canonicalSetRequest(data,size,mutation))return false;
    if(privateAdminMutation_.state==PendingPrivateAdminMutation::State::Queued) {
      if(uint32_t(now-privateAdminMutation_.receivedAt)<PrivateAdminMutationLifetimeMs)return false;
      clearPrivateAdminMutation();
    }
    if(!privateAdminPeerCurrent(from,key))return false;
    privateAdminMutation_.local=local;privateAdminMutation_.from=from;
    privateAdminMutation_.request=request;privateAdminMutation_.receivedAt=now;
    privateAdminMutation_.size=uint16_t(size);privateAdminMutation_.responseHop=responseHop;
    privateAdminMutation_.reliable=reliable;memcpy(privateAdminMutation_.key,key,32);
    memcpy(privateAdminMutation_.data,data,size);
    privateAdminMutation_.state=PendingPrivateAdminMutation::State::Queued;
    return true;
  }
  // Both calls occur synchronously inside authenticated PKI ingress while the
  // radio/storage pause and service lock are held. Probe precedes replay commit;
  // consuming is permitted only after the runtime persisted a Fresh replay ID.
  uint32_t privateAdminResponseOwners(uint32_t local,uint32_t from,const uint8_t key[32],
      uint32_t packetId,const uint8_t *data,size_t size,uint32_t now,bool consume) override {
    if(local!=route.identity()||!privatePaused_||receiving_||fault_||!privateReady()||
       !key||nodes.ignored(from))return 0;
    uint8_t current[32]{};
    const bool exact=private_->peerPublicIdentity(from,current)&&!memcmp(current,key,32);
    bench::wipeKeyBuffer(current,sizeof(current));
    return exact?privateAdminQueries_.responseOwners(local,from,key,packetId,data,size,now,consume)&LocalClientMask:0;
  }
  void privateAdminFailure(uint32_t from,const uint8_t key[32],uint32_t requestId) override {
    // Routing proofs arrive through the public-channel RX path, without a
    // storage pause. This exact cancellation changes only the RAM query table.
    if(privatePaused_||!receiving_||fault_||!privateReady()||!key||nodes.ignored(from))return;
    uint8_t current[32]{};
    const bool exact=private_->peerPublicIdentity(from,current)&&!memcmp(current,key,32);
    bench::wipeKeyBuffer(current,sizeof(current));
    if(exact)privateAdminQueries_.cancelRequest(from,key,requestId);
  }
  bool privateAdminReplyData(uint32_t local,uint32_t from,const uint8_t key[32],
      uint32_t requestId,const uint8_t *request,size_t requestSize,uint8_t *out,
      size_t capacity,size_t &size)const override {
    size=0;
    if(!privateAdminSnapshotFn_||local!=route.identity()||from<=1||from>=0xfffffffeu||
       from==local||!requestId||!privatePaused_||receiving_||fault_||!privateReady()||
       !key||!request||!requestSize||requestSize>227||
       !out||!capacity||capacity>227)return false;
    uint8_t pinned[32]{};
    const bool exactPeer=private_->matchesPeer(from)&&private_->peerPublicIdentity(from,pinned)&&
        !memcmp(pinned,key,sizeof(pinned));
    bench::wipeKeyBuffer(pinned,sizeof(pinned));
    if(!exactPeer)return false;
    if(privateAdminSnapshotFn_(privateAdminSnapshotCtx_,local,from,key,requestId,
        request,requestSize,out,capacity,size)&&size&&size<=capacity)return true;
    memset(out,0,capacity);size=0;return false;
  }
#endif
  bool channelMuted()const {ChannelRecord channel;return config.channelRecord(selectedChannel_,channel)&&channel.muted();}
  bool selectChannel(unsigned index) {
    PrimaryChannel::Selection key;
    if(!config.channelSelection(index,key))return false;
    const bool ok=key.licensedPlaintext?codec.setLicensedPlaintext(key.hash):codec.setKey(key.hash,key.bytes,key.length,key.aead);
    if(ok){selectedChannel_=uint8_t(index);selectedDefault_=key.publicDefault;}
    PrimaryChannel::clear(key);return ok;
  }
  bench::MtNodeRecord channelNode()const {
    auto record=config.node();PrimaryChannel::Selection key;
    if(config.channelSelection(selectedChannel_,key)) {
      if(key.licensedPlaintext)record.setLicensedPlaintext(key.hash);
      else record.setKey(key.hash,key.bytes,key.length);
    }
    else record.setKey(0,nullptr,0);
    PrimaryChannel::clear(key);return record;
  }
  class ChannelScope {
    Service &service_;unsigned previous_;bool selected_;
  public:
    ChannelScope(Service &service,unsigned index):service_(service),previous_(service.selectedChannel_),selected_(service.selectChannel(index)){}
    ChannelScope(const ChannelScope &)=delete;
    ChannelScope &operator=(const ChannelScope &)=delete;
    ~ChannelScope(){if(selected_&&!service_.selectChannel(previous_)){service_.codec.clearKey();service_.selectedChannel_=0;service_.selectedDefault_=false;}}
    bool valid()const{return selected_;}
  };
  // CTR parsing is only a wrong-key heuristic; CCM verifies its channel tag.
  // Equal key/mode contexts coalesce; distinct successful contexts are rejected.
  // A local zero-hash unicast needs the PKI-first decision from rx(), or its
  // exact retained shared result for subsequent serialized client fanout.
  bool sharedChannelZeroReceive(const uint8_t *p,size_t n)const {
    return receiving_&&!fault_&&!privatePaused_&&p&&n>=17&&n<=255&&p[13]==0&&
      sharedZeroReceive_.len==n&&!memcmp(sharedZeroReceive_.bytes,p,n);
  }
  bool decodeChannel(const uint8_t *p,size_t n,bench::RouteMetadata &out,unsigned &index,
                     bool allowKeyedZeroUnicast=false) {
    out={};index=0;if(!p||n<17||n>255)return false;
    const unsigned previous=selectedChannel_;const auto candidates=config.channelCandidates(p[13]);
    const bool allowZero=allowKeyedZeroUnicast||bench::le32(p)!=route.identity()||
      sharedChannelZeroReceive(p,n);
    bool ok=false;
    for(unsigned i=0;i<8;++i)if(candidates&(1u<<i)) {
      if(ok&&sameChannel(i,index))continue;
      if(!selectChannel(i)) {
        out={};out.decodeRejected=true;out.backendFailed=true;ok=false;break;
      }
      bench::RouteMetadata decoded{};
      if(codec.decode(p,n,decoded,allowZero)&&decoded.channelDecoded) {
        if(ok) {
          // Neither key order nor byte-zero classification may choose between
          // distinct keys which both pass CTR's weak parse discriminator.
          out={};out.decodeRejected=true;out.channelAmbiguous=true;ok=false;
          bench::wipeKeyBuffer(&decoded,sizeof(decoded));break;
        }
        unsigned equivalent=i;
        for(unsigned j=0;j<i;++j)if(sameChannel(i,j)){equivalent=j;break;}
        decoded.channelContext=uint8_t(equivalent+1);out=decoded;index=i;ok=true;
      } else if(decoded.backendFailed) {
        out={};out.decodeRejected=true;out.backendFailed=true;ok=false;
        bench::wipeKeyBuffer(&decoded,sizeof(decoded));break;
      } else if(!ok)out=decoded;
      bench::wipeKeyBuffer(&decoded,sizeof(decoded));
    }
    if(!selectChannel(previous)) {
      codec.clearKey();selectedChannel_=0;selectedDefault_=false;
      out={};out.decodeRejected=true;out.backendFailed=true;ok=false;
    }
    return ok;
  }
  Service(Radio &r,bool allowTx,uint32_t node,uint8_t model,bool normalMode=false,bool normalDiscovery=true,pki::PrivateRuntime *privateRuntime=nullptr):radio_(r),txAllowed_(allowTx),normalMode_(normalMode),normalDiscovery_(normalDiscovery),hardwareModel_(model),private_(privateRuntime) {
    endpoint_.localStatsSource(this,[](const void *owner){return static_cast<const Service*>(owner)->localStats();});
    route.configureContinuous(uint8_t(node),8);route.configureIdentity(node);
    route.setAirtimeProvider(airtime,&radio_);
    route.setRouteHintStore(&nodes,
      [](void *cache,uint32_t id){return static_cast<NodeCache *>(cache)->routeHint(id);},
      [](void *cache,uint32_t id,uint8_t hop){static_cast<NodeCache *>(cache)->setRouteHint(id,hop);});
    if(normalMode_&&(!endpoint_.setLifetimeAirtimeProvider(airtime,&radio_)||!route.enableDynamicLifetimes()))txAllowed_=false;
    discovery.identity(node);discovery.hardwareModel(model);
    apply(config);
  }
  bool apply(const Config &c) {
    if(privatePaused_||receiving_||active_||!c.valid())return false;
    bench::wipeKeyBuffer(&sharedZeroReceive_,sizeof(sharedZeroReceive_));
    auto record=c.node();
    // Legacy bench TX has only boot-local nonce bookkeeping. Custom keys are
    // limited to normal mode, whose startup/start gates require the provider.
    if(record.bytes[3]&&!PrimaryChannel::defaultKey(record)&&(!normalMode_||!c.channelConfigured()))return false;
    for(unsigned i=1;i<8;++i){PrimaryChannel::Selection key;const bool enabled=c.channelSelection(i,key);const bool unsafe=enabled&&!normalMode_;PrimaryChannel::clear(key);if(unsafe)return false;}
    uint32_t slotMilliseconds=28;
    if(normalMode_) {
      LoRaPresets::Profile profile{};USChannelSelection channel{};
      if(!c.resolveRadio(profile,channel)||!RadioSlot::resolve(profile.bandwidthHz,profile.spreadingFactor,slotMilliseconds))return false;
    }
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    if(memcmp(config.bytes,c.bytes,Config::Size))++storeForwardEpoch_;
#endif
#if defined(CONFIG_MT_RF_ADMIN_GET)
    if(!sameLegacyAdminDomain(config,c)) {
      if(++legacyAdminDomainEpoch_==0)++legacyAdminDomainEpoch_;
      clearLegacyAdminRequest();bench::wipeKeyBuffer(&legacyAdminBusy_,sizeof(legacyAdminBusy_));
      clearCompletedLegacyAdminSets();
    }
#endif
    if(!config.sameChannelDomain(c)){++waypointDomainEpoch_;satellites.clear();nodes.clearReplay();}
    const auto oldInterval=config.node().interval();
    endpoint_.positionConfig(c);config=c;codec.clearKey();signature.clear();route.clearRoutingState();clearTraceAssociations();receivedBeacons_.clear();
    // Keep signature policy aligned with the generated security setting and
    // the local owner's explicit license bit. Durable signer history lives in
    // NodeCache and is deliberately not reset with the verifier key cache.
    if(setPacketSignaturePolicy(c.packetSignaturePolicy(),c.licensedOperation()))return false;
    discovery.configure(record,c.primaryAead());if(private_){private_->deviceRole(c.role());private_->ownerUnmessagable(c.ownerUnmessagable());private_->rebroadcastMode(c.rebroadcastMode());private_->hopLimit(c.hopLimit());}
    if(record.interval()!=oldInterval)nodeDiscovery.reschedule(record.interval());
    route.configureContinuous(uint8_t(route.identity()),record.bytes[4]);
    if(normalMode_&&!route.configureSlotTime(slotMilliseconds))return false;
    route.setAlwaysRelay(!normalMode_||c.role()==2);
    route.setOtherOpaqueChannels(normalMode_&&(c.rebroadcastMode()==0||c.rebroadcastMode()==5));
    selectedChannel_=0;selectedDefault_=PrimaryChannel::defaultKey(record);
    uint8_t hashes[8]{};unsigned count=0;
    for(unsigned i=0;i<8;++i){PrimaryChannel::Selection key;if(c.channelSelection(i,key))hashes[count++]=key.hash;PrimaryChannel::clear(key);}
    route.setChannelHashes(hashes,count);
    if(c.channelConfigured())selectChannel(0);
    return true;
  }
  // ExtendedModuleEdit supplies the trusted stock listener flag. Disabling
  // immediately invalidates every retained advisory offer.
  bool moduleAirtimeAllowed() { return transmitDutyAdmission(true)==TxAdmission::Decision::Allow; }
  void meshBeaconListening(bool enabled) {
    meshBeaconListening_=enabled;
    if(!enabled)receivedBeacons_.clear();
  }
  void forgetReceivedBeaconSource(uint32_t source){(void)receivedBeacons_.forgetSource(source);}
  // Fixed position is local companion metadata. Publishing a validated
  // position-only Config must not reset unrelated crypto, duplicate/routing,
  // discovery, endpoint or private-session state. SettingsEdit proves the
  // whole transaction differs from its durable base only in fixed-position/GNSS settings bytes.
#if defined(CONFIG_MT_LOCAL_CLOCK)
  bool applyLocalClock(const Config &c) {
    if(privatePaused_||receiving_||active_||!c.valid()||!config.valid()||
       !config.sameExceptClock(c))return false;
    config=c;return true;
  }
#endif
  bool applyPosition(const Config &c) {
    if(privatePaused_||receiving_||active_||!config.valid()||!c.valid()||
       !config.sameExceptPosition(c))return false;
    endpoint_.positionConfig(c);config=c;return true;
  }
  // Primary-channel mute is local attention metadata. SettingsEdit proves the
  // durable candidate differs only in byte 101, then publishes it without
  // clearing codec/signature/routing/duplicate/discovery/private/endpoint state.
  bool applyChannelMute(const Config &c) {
    if(privatePaused_||receiving_||active_||!config.valid()||!c.valid()||
       !config.sameExceptChannelMute(c))return false;
    endpoint_.positionConfig(c);config=c;return true;
  }
  bool start(bool relay,uint32_t now) {
    if(relay&&radio::RegionPolicy::fixedReceiveOnly(config.regionCode()))return false;
    if(!DeviceRolePolicy::supportedRole(config.role()))return false;
    if(private_&&!publicReady())return false;
    if(!PrimaryChannel::defaultKey(config.node())&&(!normalMode_||!publicReady()))return false;
    for(unsigned i=1;i<8;++i){PrimaryChannel::Selection key;const bool unsafe=config.channelSelection(i,key)&&!key.publicDefault&&!publicReady();PrimaryChannel::clear(key);if(unsafe)return false;}
    if(privateRequired_&&!publicReady())return false;
    if(privatePaused_||fault_||receiving_||!nodes.ready()||!config.provisioned()||!route.identity()||!radio_.ready()||(relay&&!txAllowed_))return false;
    const int configured=radio_.configure(config);
    if(configured){fail(FaultStage::Configure,DiagnosticDomain::Errno,configured);return false;}
    const int started=radio_.start();
    if(started){fail(FaultStage::Start,DiagnosticDomain::Errno,started);return false;}
    const int health=radio_.health();
    if(health!=1){fail(FaultStage::StartHealth,DiagnosticDomain::Health,health);return false;}
    receiving_=true;relaying_=!radio::RegionPolicy::fixedReceiveOnly(config.regionCode())&&
      (normalMode_?DeviceRolePolicy::relays(config.role(),config.rebroadcastMode()):relay);quiet_=now+250;return true;
  }
  void stop() {
    const int ret=radio_.stop();if(ret)noteFault(FaultStage::Stop,DiagnosticDomain::Errno,ret);stoppedState();if(ret)fault_=true;
  }
  bool privatePeerMetadata(uint32_t from,const bench::RouteMetadata &m)const {
    return !private_||m.portnum!=4||private_->acceptsPeerKey(from,m.publicKey,m.publicKeyLength);
  }
  void applyPublicSignature(const uint8_t *p,size_t n,bench::RouteMetadata &m,uint32_t now) {
    // Stock traceroutes can change at every hop. An incidental signature
    // provides no signer identity and must never seed a verifier or contact.
    if(m.portnum==meshtastic_PortNum_TRACEROUTE_APP&&m.channelDecoded&&!m.decodeRejected&&!m.backendFailed&&p&&n>=17&&n<=255){
      m.signatureVerified=m.signatureQuarantined=m.signatureRejected=false;return;
    }
    if(p&&n>=17&&m.channelDecoded&&!m.decodeRejected&&!m.backendFailed&&m.signatureLength==64&&
       private_&&private_->matchesPeer(bench::le32(p+4))) {
      const uint32_t from=bench::le32(p+4);uint8_t key[32]{};
      // A durable explicit pin remains authoritative when the opportunistic
      // NodeInfo cache is absent/expired. It never substitutes for verifying
      // this packet's signature, or permits a conflicting live cached key.
      const bool valid=!nodes.ignored(from)&&privateReady()&&
        private_->peerPublicIdentity(from,key)&&bench::MtSignature::nodeId(key)==from&&
        signature.trustPublicKey(from,key,now);
      bench::wipeKeyBuffer(key,sizeof(key));
      if(!valid){m.signatureVerified=m.signatureQuarantined=false;m.signatureRejected=true;return;}
    }
    // decodeChannel assigns a context only after a unique configured-key
    // decode. Local encrypted hash-zero unicast has already passed rx()'s
    // PKI-first negative classification; licensed plaintext is explicit too.
    const bool resolvedZero=p&&n>=17&&p[13]==0&&bench::le32(p)!=0xffffffffu&&
      m.channelDecoded&&!m.decodeRejected&&!m.backendFailed&&m.channelContext&&
      m.channelContext<=8&&codec.usableConfigured()&&
      (config.channelCandidates(0)&(1u<<(m.channelContext-1)))&&
      sameChannel(selectedChannel_,m.channelContext-1);
    signature.apply(p,n,m,now,resolvedZero);
  }
  bool previouslyVerifiedSigner(uint32_t node)const{return nodes.verifiedSigner(node);}
  bool refusesUnsignedIdentity(uint32_t from,unsigned channel,bool verified)const {
    if(verified||!packetSignaturePolicy_||from==route.identity())return false;
    PrimaryChannel::Selection key{};
    const bool aead=config.channelSelection(channel,key)&&key.aead;
    PrimaryChannel::clear(key);if(aead)return false;
    if(previouslyVerifiedSigner(from))return true;
#if defined(CONFIG_MT_WARM_PEERS)
    if(coldIdentityVeto_)return coldIdentityVeto_(from);
#endif
    return false;
  }
  void noteRefusedIdentity(uint32_t id,const bench::RouteMetadata &m,uint32_t now){
    if(refusedIdentity_||(lastRefusedNotice_&&uint32_t(now-lastRefusedNotice_)<3600000u))return;
    const int i=nodes.find(id);const auto *trusted=i<0?nullptr:nodes.get(unsigned(i));
    if(!trusted||trusted->placeholder()||!trusted->verifiedSigner())return;
    NodeCache::Entry claimed;
    if(!NodeUser::decode(claimed,id,m.payload,m.payloadLength))return;
    using E=NodeCache::Entry;
    if(trusted->role()==claimed.role()&&!memcmp(trusted->bytes+E::Name,claimed.bytes+E::Name,E::KeyLength-E::Name))return;
    refusedIdentity_=id;lastRefusedNotice_=now?now:1;
  }
  bool takeRefusedIdentityNotice(meshtastic_ClientNotification &out){
    if(!refusedIdentity_)return false;
    const uint32_t id=refusedIdentity_;refusedIdentity_=0;
    const int i=nodes.find(id);const auto *trusted=i<0?nullptr:nodes.get(unsigned(i));
    if(!trusted||trusted->placeholder()||!trusted->verifiedSigner()||!packetSignaturePolicy_)return false;
    out={};out.level=meshtastic_LogRecord_Level_WARNING;
    char name[40]{};
    admin::HamModeProjection::copyStockName(name,sizeof(name),
        reinterpret_cast<const char *>(trusted->bytes+NodeCache::Entry::Name),sizeof(name),sizeof(name)-1);
    snprintf(out.message,sizeof(out.message),"Unsigned update from %s%s!%08x%s ignored, due to %s security setting. Remove the node to accept its unsigned updates.",
      name,*name?" (":"",unsigned(id),*name?")":"",packetSignaturePolicy_==2?"Strict":"Balanced");
    return true;
  }
  static bool canonicalSignableDataSize(const bench::RouteMetadata &m,size_t &size) {
    size=0;if(m.payloadLength>sizeof(m.payload))return false;
    const pb_msgdesc_t *innerFields=nullptr;
    union InnerMessage {
      meshtastic_Position position;
      meshtastic_Telemetry telemetry;
      meshtastic_Waypoint waypoint;
      meshtastic_User user;
    } inner{};
    switch(m.portnum) {
      case meshtastic_PortNum_POSITION_APP:innerFields=&meshtastic_Position_msg;break;
      case meshtastic_PortNum_TELEMETRY_APP:innerFields=&meshtastic_Telemetry_msg;break;
      case meshtastic_PortNum_WAYPOINT_APP:innerFields=&meshtastic_Waypoint_msg;break;
      case meshtastic_PortNum_NODEINFO_APP:innerFields=&meshtastic_User_msg;break;
      default:break;
    }
    meshtastic_Data data=meshtastic_Data_init_zero;
    data.portnum=static_cast<meshtastic_PortNum>(m.portnum);
    data.payload.size=pb_size_t(m.payloadLength);
    data.want_response=m.wantResponse;data.has_bitfield=m.hasBitfield;data.bitfield=m.bitfield;
    data.reply_id=m.reply_id;data.emoji=m.emoji;
    size_t rawDataSize=0;
    // RouteMetadata intentionally omits some outer Data fields. If rebuilding
    // the raw envelope does not exactly match its measured wire size, sizing
    // is incomplete and Balanced must fail open as stock does on sizing error.
    if(!m.encodedDataSize||!pb_get_encoded_size(&rawDataSize,&meshtastic_Data_msg,&data)||
       rawDataSize!=m.encodedDataSize){bench::wipeKeyBuffer(&data,sizeof(data));return false;}
    size_t payloadSize=m.payloadLength;
    if(innerFields) {
      auto stream=pb_istream_from_buffer(m.payload,m.payloadLength);
      size_t canonicalPayload=0;
      if(pb_decode(&stream,innerFields,&inner)&&
         pb_get_encoded_size(&canonicalPayload,innerFields,&inner)&&canonicalPayload<=m.payloadLength)
        payloadSize=canonicalPayload;
      bench::wipeKeyBuffer(&inner,sizeof(inner));
    }
    data.payload.size=pb_size_t(payloadSize);
    const bool ok=pb_get_encoded_size(&size,&meshtastic_Data_msg,&data);
    bench::wipeKeyBuffer(&data,sizeof(data));
    return ok;
  }
  bool packetSignatureAllowed(const uint8_t *p,const bench::RouteMetadata &m,uint32_t now) {
    (void)now; // Prior signer evidence is durable, not coupled to verifier-key TTL.
    if(!p||m.signatureRejected)return false;
    if(m.portnum==meshtastic_PortNum_TRACEROUTE_APP)return true;
    if(m.signatureLength&&m.signatureLength!=64)return false;
    if(m.signatureVerified) {
      if(bench::le32(p+4)==route.identity())return true;
      // Verification itself is authoritative; record durable anti-downgrade
      // evidence before any owner-facing or relay side effect. Capacity or
      // journal-readiness failure fails closed, like stock NodeDB exhaustion.
      return nodes.rememberVerifiedSigner(bench::le32(p+4),route.identity());
    }
    const bool strict=packetSignaturePolicy_==2&&!codec.aead();
    if(m.signatureLength)return !strict; // Unknown signer remains in opaque quarantine.
    return !strict||m.portnum==meshtastic_PortNum_ROUTING_APP||
      m.portnum==meshtastic_PortNum_KEY_VERIFICATION_APP;
  }
  void stageVerifiedPeerCandidate(uint32_t from,const bench::RouteMetadata &m,
                                  bool radioReceive,uint32_t now) {
    if(codec.originsRestricted()||!radioReceive||from==route.identity()||nodes.ignored(from)||
       !m.channelDecoded||m.portnum!=meshtastic_PortNum_NODEINFO_APP||!m.nodeInfoValid||
       m.signatureLength!=64||!m.signatureVerified||m.signatureRejected||m.signatureQuarantined||
       m.publicKeyLength!=32||bench::MtSignature::nodeId(m.publicKey)!=from||
       (private_&&private_->matchesPeer(from)))return;
    uint8_t verified[32]{};
    const bool exact=signature.publicIdentity(from,now,verified)&&!memcmp(verified,m.publicKey,32);
    bench::wipeKeyBuffer(verified,sizeof(verified));
    if(!exact)return;
    if(verifiedPeerCandidate_.pending) {
      // Repeat observations coalesce; a conflicting first candidate is kept
      // intact for the owner-loop consumer to inspect and resolve explicitly.
      if(verifiedPeerCandidate_.node!=from||memcmp(verifiedPeerCandidate_.publicKey,m.publicKey,32))return;
      return;
    }
    verifiedPeerCandidate_.node=from;
    memcpy(verifiedPeerCandidate_.publicKey,m.publicKey,32);
    verifiedPeerCandidate_.pending=true;
  }
  bool stockChannelExcludesAutoPki(unsigned channel)const {
    ChannelRecord record;
    if(channel>=8||!config.channelRecord(channel,record)||!record.role())return true;
    const char *name=record.name()[0]?record.name():config.modemName();
    auto equals=[](const char *a,const char *b) {
      if(!a)return false;
      while(*a&&*b) {
        unsigned char x=static_cast<unsigned char>(*a++),y=static_cast<unsigned char>(*b++);
        if(x>='A'&&x<='Z')x=uint8_t(x+'a'-'A');
        if(y>='A'&&y<='Z')y=uint8_t(y+'a'-'A');
        if(x!=y)return false;
      }
      return !*a&&!*b;
    };
    return equals(name,"serial")||equals(name,"gpio");
  }
  bool privateConfigured()const{return private_!=nullptr;}
  pki::DestinationDisposition privateDestinationDisposition(uint32_t peer,
      const uint8_t *key=nullptr,size_t keySize=0)const {
    if(peer<=1||peer>=0xfffffffeu||peer==route.identity()||
       (keySize&&(!key||keySize!=32)))return pki::DestinationDisposition::KeyMismatch;
    auto disposition=private_?private_->destinationDisposition(peer,key,keySize):
      pki::DestinationDisposition::UnknownNoKey;
    if(disposition!=pki::DestinationDisposition::UnknownNoKey)return disposition;
    const int slot=nodes.find(peer);const auto *entry=slot<0?nullptr:nodes.get(unsigned(slot));
    if(entry&&(entry->bytes[NodeCache::Entry::KeyLength]||entry->verifiedSigner()||entry->manuallyVerified())) {
      const auto length=entry->bytes[NodeCache::Entry::KeyLength];
      const bool mismatch=keySize&&length&&(length!=32||memcmp(key,entry->bytes+NodeCache::Entry::Key,32));
      if(keySize&&!length)return pki::DestinationDisposition::KeyMismatch;
      return mismatch?pki::DestinationDisposition::KeyMismatch:pki::DestinationDisposition::KnownUnpinned;
    }
    return pki::DestinationDisposition::UnknownNoKey;
  }
  // Owner-loop-only proof handoff. Caller must serialize with the Service
  // lock; these methods expose no NodeCache-derived trust and perform no I/O.
  bool peekVerifiedPeerCandidate(uint32_t &node,uint8_t publicKey[32])const {
    node=0;if(publicKey)memset(publicKey,0,32);
    if(!publicKey||fault_||!verifiedPeerCandidate_.pending)return false;
    node=verifiedPeerCandidate_.node;
    memcpy(publicKey,verifiedPeerCandidate_.publicKey,32);return true;
  }
  bool takeVerifiedPeerCandidate(uint32_t &node,uint8_t publicKey[32]) {
    if(fault_){node=0;if(publicKey)memset(publicKey,0,32);clearVerifiedPeerCandidate();return false;}
    if(!peekVerifiedPeerCandidate(node,publicKey))return false;
    clearVerifiedPeerCandidate();return true;
  }
  void clearVerifiedPeerCandidate() {
    bench::wipeKeyBuffer(&verifiedPeerCandidate_,sizeof(verifiedPeerCandidate_));
  }
  // Main always fans a physical RX out to both companion transports. They
  // must apply these same gates before signer, duplicate, cache or UI effects.
  // Called under the application's shared lock at the physical RX boundary.
  // Every local transport fans out only after this one traffic decision.
#if defined(CONFIG_MT_WARM_PEERS)
  void warmIdentityVeto(bool (*lookup)(uint32_t)){coldIdentityVeto_=lookup;}
  void warmPositionRole(bool (*lookup)(uint32_t,uint8_t&)){positionRole_=lookup;}
#endif
  void trafficManagement(modules::TrafficManagementWindow *gate,uint64_t uptime){traffic_=gate;trafficUptime_=uptime;}
  bool trafficSuppressed()const{return trafficSuppressed_;}
  bool bindTrafficNodeInfoSink(bool (*sink)(void*,const uint8_t*,size_t,const bench::RouteMetadata&,unsigned,uint32_t,uint32_t),void *context){
    if(trafficNodeInfoSink_&&sink&&trafficNodeInfoSink_!=sink)return false;trafficNodeInfoSink_=sink;trafficNodeInfoContext_=context;return true;
  }
  bool publicProxyAvailable()const{return localEndpointReady()&&codec.configured()&&!endpoint_.modulePending();}
  uint32_t publicProxyPacketId(uint32_t random){return publicProxyAvailable()?endpoint_.generatedId(random):0;}
  int submitPublicNodeInfoProxy(const modules::TrafficNodeInfoReply &reply,uint32_t now){
    if(!publicProxyAvailable())return -EAGAIN;
    if(selectedChannel_!=reply.channelIndex||codec.channelHash()!=reply.channelHash)return -EINVAL;
    if(!reply.unsignedPublicProxy||!reply.proxyAuthor||reply.proxyAuthor==route.identity()||
       !reply.destination||reply.destination==UINT32_MAX||!reply.packetId||!reply.requestId||
       reply.hopLimit||reply.hopStart||reply.userPayloadSize>233||!reply.userPayloadSize)return -EINVAL;
    uint8_t okToMqtt=0;
    if(!proxyNodeInfoOkToMqtt(reply.proxyAuthor,reply.channelIndex,reply.channelHash,now,okToMqtt))return -ESTALE;
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_NODEINFO_APP;d.request_id=reply.requestId;d.has_bitfield=true;
    // Keep only the target author's public MQTT-consent bit. WANT_RESPONSE is
    // not copied into a proxy reply; the proxy must not assert local consent
    // on behalf of the claimed author.
    d.bitfield=okToMqtt;
    d.payload.size=reply.userPayloadSize;memcpy(d.payload.bytes,reply.userPayload,d.payload.size);
    uint8_t wire[239]{};auto stream=pb_ostream_from_buffer(wire,sizeof(wire));if(!pb_encode(&stream,meshtastic_Data_fields,&d))return -EMSGSIZE;
    // Stock cached reply is an explicitly unsigned PUBLIC proxy. Its claimed
    // original author is never passed to the PKI signer or key pinning path.
    bench::Frame frame;size_t length=0;
    if(!codec.encodeZeroHopData(reply.proxyAuthor,reply.destination,reply.packetId,wire,stream.bytes_written,frame.bytes,sizeof(frame.bytes),length))return -EPERM;
    frame.len=length;frame.bytes[12]=0;frame.bytes[15]=uint8_t(route.identity());
    return endpoint_.submitModuleControl(frame,now,false);
  }

  bool routingPacketAllowed(bool opaque,uint8_t channel,uint32_t from,uint32_t to)const {
    return stock::routing::accepts(opaque,channel,from,to,config.rebroadcastMode(),config.licensedOperation(),
      [this](uint32_t id){return knownUser(id);},
      [this](uint32_t id){
        const int slot=nodes.find(id);
        const auto *entry=slot<0?nullptr:nodes.get(unsigned(slot));
        return entry&&!entry->placeholder()&&!entry->licensed();
      });
  }
  bool receiveSourceAllowed(uint32_t from)const{return config.rebroadcastMode()!=3||knownUser(from);}
  bool receivePortAllowed(uint32_t port)const{return config.rebroadcastMode()!=5||corePort(port);}
  pki::PrivateReplyDiagnostics privateReplyDiagnostics()const{return private_?private_->replyDiagnostics():pki::PrivateReplyDiagnostics{};}
  bool customPrimaryAvailable()const{return normalMode_&&publicReady();}
  bool privateDormant()const{return private_&&private_->dormant(route.identity());}
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  // Trusted boot composition calls this before any RF or client publication.
  // Key selection and settings reloads preserve both encoder restrictions.
  void restrictImportedChannelOrigins(){codec.restrictOrigins(*this);discovery.restrictOrigins(*this);}
#endif
  // Opt-in identity qualification profiles cannot regain Public RF while their
  // private domain is absent/held, including through later settings setters.
  bool requirePrivateRuntime() {
    if(receiving_||active_||own_||normalActive_||endpointArmed_)return false;
    privateRequired_=true;return true;
  }
  // Trusted boot composition only, before publishing any client. A provider
  // cannot be replaced, attached to a running radio, or change the local ID.
  bool bindPrivateRuntime(pki::PrivateRuntime &runtime) {
    if(private_||!normalMode_||!storageQuiescent()||!radio_.ready()||radio_.busy()||
       radio_.health()!=0||!nodes.ready()||
       (!runtime.ready(route.identity())&&!runtime.dormant(route.identity())&&
        !runtime.publicReady(route.identity())))return false;
    private_=&runtime;private_->deviceRole(config.role());private_->ownerUnmessagable(config.ownerUnmessagable());private_->rebroadcastMode(config.rebroadcastMode());private_->hopLimit(config.hopLimit());return true;
  }
  // A configured private startup failure cannot silently regain Public RF.
  // There is no companion/admin operation that clears this terminal latch.
  void holdPrivateStartup(){fail();}
  bool privatePublicIdentity(uint8_t out[32])const{return publicReady()&&private_->publicIdentity(out);}
  // Actual native RF-off proof for the injected ledger's storage guard. Queue
  // and arm are intentionally retained; all entry points reject reentry.
  bool privateStoragePaused() {return privatePaused_&&!fault_&&!receiving_&&!active_&&!own_&&!normalActive_&&radio_.ready()&&radio_.health()==0;}
  // Explicit local peer administration shares the runtime storage barrier;
  // pending user/control/discovery work cannot be discarded to admit a pin.
  bool beginPrivateAdministration(){return (privateReady()||privateDormant())&&storageStopSafe()&&pausePrivate();}
  bool endPrivateAdministration(){return resumePrivate();}
  bool takePrivateEvent(uint8_t owner,const uint8_t *p,size_t n,pki::PrivateEvent &out) {
    return receiving_&&!privatePaused_&&!fault_&&privateReady()&&private_->event(owner,p,n,out);
  }
  bool revokePrivatePeer(uint32_t peer) {
#if defined(CONFIG_MT_RF_ADMIN_GET)
    clearPrivateAdminMutation();
    privateAdminQueries_.cancelPeer(peer);
#endif
    if(!private_||!private_->matchesPeer(peer))return true;
    if(!storageStopSafe()||!pausePrivate())return false;
    const bool persisted=private_->revoke(peer);
    if(!persisted){noteFault(FaultStage::Unknown,DiagnosticDomain::State,0);stop();fault_=true;return false;}
    if(!endpoint_.invalidatePrivatePeer(peer)){noteFault(FaultStage::Unknown,DiagnosticDomain::State,0);stop();fault_=true;return false;}
    signature.forgetPublicKey(peer);
    if(privateDormant()&&!publicReady()) {
      endpointArmed_=false;privateWasReceiving_=false;
      return resumePrivate();
    }
    // A multi-peer provider keeps the other trusted sessions and prior RX/arm.
    // Legacy one-peer providers retain their conservative terminal stop.
    if(privateReady()||publicReady())return resumePrivate();
    stop();return true;
  }
  bool receiving() const {return receiving_;}
  bool receivingFrame(const uint8_t *data,size_t size,bool radioReceive)const {
    return receiving_||(!radioReceive&&mqttRadioOffFrame_&&
      mqttRadioOffFrame_->data==data&&mqttRadioOffFrame_->size==size);
  }
  bool relaying() const {return relaying_;}
  bool faulted() const {return fault_;}
  bool active() const {return active_;}
  bool txAllowed() const {return txAllowed_;}
  bool normalMode()const {return normalMode_;}
  // A durable remote Config is awaiting its old-profile response. The owner
  // keeps polling only the existing response job; no new discovery/Admin work.
  void configurationTransition(bool active) {
    configurationTransition_=active;
    if(active){nodeDiscovery.cancel();bootstrapRequest_={};}
  }
  StoragePauseDiagnostic storagePauseDiagnostic()const{return storagePauseDiagnostic_;}
  RadioDiagnostic radioDiagnostic()const {
    auto out=radioDiagnostic_;
    if(!out.faultStage)out.nativeKnown=radio_.diagnostic(out.native);
    return out;
  }
  void noteOriginReject(uint16_t stage,DiagnosticDomain domain,int32_t value) {
    RadioDiagnostic::bump(radioDiagnostic_.rejected);radioDiagnostic_.rejectStage=stage;
    radioDiagnostic_.rejectDomain=domain;radioDiagnostic_.rejectValue=value;radioDiagnostic_.rejectFlags=storagePauseMask();
  }
  // Pre-stop admission for settings/storage operations. Healthy RX and the
  // durable endpoint arm may remain live, but no accepted user, control,
  // discovery, routing or native work may be discarded to obtain quiescence.
  // Callers still prove native ready/not-busy separately before stopping RX.
#if defined(CONFIG_MT_RF_ADMIN_GET)
  bool privateAdminQueriesPending()const {return privateAdminQueries_.size()!=0;}
  // A queued authenticated SET is accepted work that stoppedState() would
  // erase. Processing clears the slot before entering its owner callback, so
  // a storage mutation invoked by that callback is not rejected here.
  bool privateAdminMutationQueued()const {
    return privateAdminMutation_.state==PendingPrivateAdminMutation::State::Queued;
  }
#endif
  bool storageStopSafe()const {
    return !privatePaused_&&!fault_&&!active_&&!own_&&!endpointQueued_&&
      !endpointActive_&&!normalActive_&&endpoint_.idle()&&
      !discovery.pending()&&!route.pending()&&!route.retransmitting()&&
      !bootstrapRequest_.ready;
  }
  // Race-safe transition used by every settings/storage owner. A clean native
  // -EAGAIN maps to -EBUSY only after reception and its completed event were
  // restored. Other hard failures fault with live bookkeeping; only verified
  // standby (-ESHUTDOWN) authorizes stopped service state.
  int pauseForStorage() {
    ++storagePauseDiagnostic_.sequence;
    if(!storageStopSafe())return storagePauseOutcome(1,-EBUSY,storagePauseMask());
    if(radio_.busy())return storagePauseOutcome(2,-EBUSY,radio_.busyDiagnostic());
    if(!receiving_)
      return radio_.ready()&&!radio_.busy()&&radio_.health()==0?storagePauseOutcome(0,0,0):storagePauseOutcome(3,-EIO,0);
    const int ret=radio_.stopForStorage();
    if(ret==-EAGAIN)return storagePauseOutcome(4,-EBUSY,0);
    if(ret){
      noteFault(FaultStage::StorageStop,DiagnosticDomain::Errno,ret);
      if(ret==-ESHUTDOWN)stoppedState();
      fault_=true;return storagePauseOutcome(5,ret,0);
    }
    const int health=radio_.health();
    if(health!=0){noteFault(FaultStage::StorageHealth,DiagnosticDomain::Health,health);fault_=true;return storagePauseOutcome(6,-EIO,0);}
    stoppedState();return storagePauseOutcome(0,0,0);
  }
  // State-only eligibility: callers must additionally prove the native radio
  // is ready and fully off (RX health is not storage quiescence).
  bool storageQuiescent()const {
    return !receiving_&&!relaying_&&!endpointArmed_&&storageStopSafe();
  }
  // Read-only proof for explicit restoration of an absent identity from its
  // intact, already-bound history. Unlike ordinary storage operations this
  // may be true after a latched startup fault, but never while private runtime
  // or settings authority exists, any work is pending, or native RF is active.
  bool identityRecoveryQuiescent()const {
    return !private_&&!privatePaused_&&!receiving_&&!relaying_&&
      !endpointArmed_&&!active_&&!own_&&!endpointQueued_&&!endpointActive_&&
      !normalActive_&&endpoint_.idle()&&!discovery.pending()&&!route.pending()&&
      !route.retransmitting()&&!bootstrapRequest_.ready&&radio_.ready()&&
      !radio_.busy()&&radio_.health()==0;
  }
  unsigned endpointControls()const {return normalMode_?endpoint_.pendingControls():0;}
  unsigned endpointFree()const {return normalMode_?endpoint_.free():unsigned(!endpointQueued_&&!endpointActive_);}
  bool takeMessageResult(uint8_t owner,Endpoint::Result &r){return endpoint_.take(owner,r);}
  bool takeLocalInputResult(Endpoint::Result &result){return endpoint_.takeLocalInput(result);}
  bool takeLocalUiResult(Endpoint::Result &result){return endpoint_.takeLocalUiResult(result);}
  void disconnect(uint8_t owner){
#if defined(CONFIG_MT_RF_ADMIN_GET)
    if(owner<LocalClientCount)privateAdminQueries_.cancelOwners(1u<<owner);
#endif
    if(normalMode_)endpoint_.disconnect(owner);
  }
  void rx(const uint8_t *p,size_t n,int8_t snr,uint32_t now,uint32_t random,bool radioReceive=true,
          bool *routeAccepted=nullptr,bool forceOpaqueChannelZero=false,uint32_t utcNow=0,uint32_t uptimeSeconds=0,int16_t receivedRssi=0,float receivedSnr=0,
          bool *channelDelivered=nullptr) {
    endpoint_.telemetryUtc(utcNow);
#if defined(CONFIG_MT_RF_ADMIN_GET)
    legacyAdminNow_=now;
#endif
    if(routeAccepted)*routeAccepted=false;
    if(channelDelivered)*channelDelivered=false;
    trafficSuppressed_=false;
    bench::wipeKeyBuffer(&sharedZeroReceive_,sizeof(sharedZeroReceive_));
    if(private_)private_->clearEvent();
    if(!receivingFrame(p,n,radioReceive)||fault_||privatePaused_)return;
    if(!p&&!n){const int health=radio_.health();if(health==1)++rejected;else fail(FaultStage::ReceiveHealth,DiagnosticDomain::Health,health);return;}
    if(!p||n<17||n>255){++rejected;return;}
    // Local ignored-node policy precedes ACK/retry completion, discovery,
    // cache admission and routing. Header IDs are not authenticated identities.
    if(config.ignoresIncoming(bench::le32(p+4))||nodes.ignored(bench::le32(p+4))){++rejected;return;}
    if(radioReceive)++received;
    if(normalMode_) {
      const auto from=bench::le32(p+4),to=bench::le32(p);const unsigned hop=p[12]&7,start=(p[12]>>5)&7;
      const bool endpointPacket=to==0xffffffffu||to==route.identity()||from==route.identity();
      if(from<=1||from>=0xfffffffeu||to<=1||!bench::le32(p+8)||hop>start)return;
      if(radioReceive&&from==route.identity()&&p[13]==0)
        (void)endpoint_.observePrivateRelayEcho(p,n,now);
      // An operator-pinned private peer is authenticated by the private path
      // below, even when ordinary KnownOnly NodeDB admission has no user row.
      uint8_t verificationGateKey[32]{};
      const bool verificationPinned=private_&&private_->keyVerificationPeerKey(from,verificationGateKey);
      bench::wipeKeyBuffer(verificationGateKey,sizeof(verificationGateKey));
      const bool verificationCandidate=private_&&private_->keyVerificationCandidateMatches(from);
      const bool privateVerificationSource=p[13]==0&&to==route.identity()&&private_&&
        ((privateReady()&&(private_->matchesPeer(from)||verificationCandidate||verificationPinned))||
         (publicReady()&&verificationCandidate));
      const bool candidateVerificationSource=to==route.identity()&&
        publicReady()&&verificationCandidate;
      if(!receiveSourceAllowed(from)&&
         !privateVerificationSource&&!candidateVerificationSource) {
        bench::RouteMetadata opaque;
        const bool admitted=receiveTransit(p,n,opaque,snr,now,random,radioReceive,utcNow);
        if(routeAccepted)*routeAccepted=admitted;bench::wipeKeyBuffer(&opaque,sizeof(opaque));return;
      }
      // Own-source echoes must reach the endpoint even for unicast control
      // responses. Endpoint requires exact ID/destination/ciphertext and lower
      // hops; an ordinary direct-message echo is never recipient delivery.
      PrimaryChannel::Selection channelZero{};
      const bool publicPlaintextZero=config.licensedOperation()&&config.channelSelection(0,channelZero)&&channelZero.licensedPlaintext;
      PrimaryChannel::clear(channelZero);
      bool sharedZeroCandidate=false;
      if(p[13]==0&&to==route.identity()&&!publicPlaintextZero) {
        // PKI and encrypted channels share this wire byte. Only an explicit
        // negative authentication outcome can enter channel decoding; held,
        // authenticated, replayed or quarantined private work is consumed.
        if(private_) {
          if(!publicReady()||!pausePrivate())return;
          endpoint_.blocked(radio_.airtime(n));
          endpoint_.measuredAirUtilTx(airtime_.percent());
          endpoint_.measuredChannelUtilization(channel_.percent());
          const auto outcome=private_->receiveClassified(endpoint_,codec,p,n,now,random,
            endpointArmed_&&txAllowed_&&importedPeerAllowed(from));
          if(!keyVerificationReceiveReady_) {
            pki::KeyVerificationReceive keyVerification{};
            if(private_->takeKeyVerificationReceive(keyVerification)) {
              keyVerificationReceive_=keyVerification;keyVerificationReceiveReady_=true;
              bench::wipeKeyBuffer(&keyVerification,sizeof(keyVerification));
            }
          }
          uint32_t authenticatedFrom=0;
          if(private_->takeAuthenticatedReceive(authenticatedFrom)&&authenticatedFrom==from) {
            sampleHopDistance(radioReceive,from,p[12],random);
            observeHotNode(authenticatedFrom,utcNow,uptimeSeconds,(p[12]&0x10u)!=0);
          }
          if(!resumePrivate())return;
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE) || defined(CONFIG_MT_DROPZONE)
          if(radioReceive && !(p[12]&0x10u) && outcome==pki::ReceiveClassification::AuthenticatedConsumed){
            const auto *event=private_->moduleEvent(p,n);
            if(event){
              meshtastic_Data d{};auto input=pb_istream_from_buffer(event->data,event->size);
              if(pb_decode(&input,meshtastic_Data_fields,&d) && !input.bytes_left && receivePortAllowed(d.portnum))
                observeStockOptional({from,to,event->id,unsigned(d.portnum),0,start,hop,bool(p[12]&8),
                  d.want_response,d.has_bitfield,true,receivedRssi,receivedSnr,d.payload.bytes,d.payload.size},now);
              bench::wipeKeyBuffer(&d,sizeof(d));
            }
          }
#endif
          if(outcome!=pki::ReceiveClassification::Unauthenticated&&
             outcome!=pki::ReceiveClassification::NotPki)return;
        }
        if(forceOpaqueChannelZero||!config.channelCandidates(0))return;
        sharedZeroCandidate=true;
      }
      if(forceOpaqueChannelZero&&!publicPlaintextZero) {
        if(p[13]!=0)return;
        bench::RouteMetadata opaque{};
        const bool admitted=receiveTransit(p,n,opaque,snr,now,random,radioReceive,utcNow);
        if(routeAccepted)*routeAccepted=admitted;bench::wipeKeyBuffer(&opaque,sizeof(opaque));return;
      }
      bench::RouteMetadata m;
      unsigned decodedSlot=0;const bool decoded=decodeChannel(p,n,m,decodedSlot,sharedZeroCandidate);
      if(!decoded) {
        if(from==route.identity()){bench::wipeKeyBuffer(&m,sizeof(m));return;}
        // In stock licensed mode hash-zero is a configured public plaintext
        // channel. A failed decode is a rejected licensed candidate, never an
        // opaque PKI fallback that could be relayed as if it were a DM.
        if(p[13]==0&&(publicPlaintextZero||sharedZeroCandidate)){++rejected;bench::wipeKeyBuffer(&m,sizeof(m));return;}
        if(radioReceive)uplinkOpaquePki(p,n,from,to,m,utcNow,uptimeSeconds);
        if(traffic_&&traffic_->unknown(from,trafficUptime_)){trafficSuppressed_=true;bench::wipeKeyBuffer(&m,sizeof(m));return;}
        const bool admitted=receiveTransit(p,n,m,snr,now,random,radioReceive,utcNow);
        if(routeAccepted)*routeAccepted=admitted;bench::wipeKeyBuffer(&m,sizeof(m));return;
      }
      ChannelScope channel(*this,decodedSlot);if(!channel.valid()){bench::wipeKeyBuffer(&m,sizeof(m));return;}
      // Verify and enforce signature policy before NodeCache, module, MQTT,
      // discovery, duplicate, or relay side effects can observe this packet.
      applyPublicSignature(p,n,m,now);
      if(!packetSignatureAllowed(p,m,now)) {
        ++rejected;bench::wipeKeyBuffer(&m,sizeof(m));return;
      }
      if(from==route.identity()) {
        // Overheard local packets can complete an existing exact-byte implicit ACK.
        // They never represent another sender to the modules or a phone.
        if(radioReceive&&!m.signatureRejected&&!m.signatureQuarantined&&(!m.signatureLength||m.signatureVerified)) {
          if(to!=UINT32_MAX)route.observeOriginRelay(p,n,m,now);
          endpoint_.receive(codec,route,from,p,n,m,snr,now,random,false);
        }
        bench::wipeKeyBuffer(&m,sizeof(m));return;
      }
      if(m.signatureQuarantined) {
        // Unknown signed payloads remain in the isolated opaque transit path;
        // they cannot update peer/module/owner state or answer requests.
        const bool admitted=receiveTransit(p,n,m,snr,now,random,radioReceive,utcNow);
        if(routeAccepted)*routeAccepted=admitted;
        bench::wipeKeyBuffer(&m,sizeof(m));return;
      }
      if(candidateVerificationSource) {
        if(m.portnum==meshtastic_PortNum_ROUTING_APP&&m.routingPresent&&
           m.routingErrorPresent&&m.request_id&&private_&&
           private_->keyVerificationProof(endpoint_,p,n,m,now)) {
          if(routeAccepted)*routeAccepted=true;
          bench::wipeKeyBuffer(&m,sizeof(m));return;
        }
        // While a first-contact session is active, the candidate's shared-
        // channel surface is limited to canonical KVA and correlated ACK
        // proof. It never falls through into normal application delivery.
        if(m.portnum!=meshtastic_PortNum_KEY_VERIFICATION_APP) {
          bench::wipeKeyBuffer(&m,sizeof(m));return;
        }
      }
#if defined(CONFIG_MT_RF_ADMIN_GET)
      if(endpointPacket&&to==route.identity())
        stageLegacyAdminRequest(p,n,m,decodedSlot,from,to,now,radioReceive);
#endif
      if(endpointPacket&&to==route.identity()&&
         m.portnum==meshtastic_PortNum_KEY_VERIFICATION_APP) {
        uint8_t pinnedKey[32]{};
        const bool alreadyPinned=private_&&private_->keyVerificationPeerKey(from,pinnedKey);
        bench::wipeKeyBuffer(pinnedKey,sizeof(pinnedKey));
        bool admitted=false;
        if(!codec.originsRestricted()&&radioReceive&&!config.licensedOperation()&&!keyVerificationReceiveReady_&&!alreadyPinned&&publicReady()&&
           pki::PrivateApplication::keyVerificationPayload(m.payload,m.payloadLength)&&
           m.publicKeyLength==0&&!m.signatureLength&&!m.signatureRejected&&!m.signatureQuarantined) {
          keyVerificationReceive_={};keyVerificationReceive_.from=from;
          keyVerificationReceive_.id=bench::le32(p+8);keyVerificationReceive_.pki=false;
          keyVerificationReceive_.size=m.payloadLength;
          memcpy(keyVerificationReceive_.payload,m.payload,m.payloadLength);
          keyVerificationReceiveReady_=true;admitted=true;
        }
        // Header want_ack requests the ordinary network Routing ACK. The
        // KeyVerification Data.want_response bit is an independent protocol
        // signal and must not suppress or synthesize that transport ACK.
        if(admitted&&endpointArmed_&&txAllowed_&&(p[12]&8u))
          endpoint_.acknowledgeKeyVerification(codec,route.identity(),from,bench::le32(p+8),
            p[12]&7u,(p[12]>>5)&7u,now,random,true);
        bench::wipeKeyBuffer(&m,sizeof(m));return;
      }
      if(!start&&!m.hasBitfield) {
        bench::wipeKeyBuffer(&m,sizeof(m));return;
      }
      if(p[13]==0) {
        memcpy(sharedZeroReceive_.bytes,p,n);sharedZeroReceive_.len=uint16_t(n);
      }
      // Metadata reflects a successfully decoded/validated source, not whether
      // traffic policy, local relay role, endpoint arm, TX permission, or route
      // queue capacity later permits forwarding. Signature verification above
      // may refresh public verifier/signer evidence; private peer enrollment
      // is staged only in the local endpoint path below.
      const bool refusedIdentity=m.portnum==meshtastic_PortNum_NODEINFO_APP&&
        refusesUnsignedIdentity(from,decodedSlot,m.signatureVerified);
      if(refusedIdentity&&radioReceive)noteRefusedIdentity(from,m,now);
      if(refusedIdentity&&to==UINT32_MAX){++rejected;bench::wipeKeyBuffer(&m,sizeof(m));return;}
      const bool nodeInfoPermitted=!refusedIdentity&&(!(endpointPacket&&from!=route.identity()&&m.portnum==4&&m.nodeInfoValid)||
        NodeUser::permits(nodes,route.identity(),from,m.payload,m.payloadLength,start-hop)>=0);
      if(!m.backendFailed&&!m.decodeRejected&&!m.signatureRejected&&nodeInfoPermitted&&privatePeerMetadata(from,m)&&
         from&&from!=route.identity()) {
        sampleHopDistance(radioReceive,from,p[12],random);
        observeHotNode(from,utcNow,uptimeSeconds,(p[12]&0x10u)!=0);
      }
      if(!receivePortAllowed(m.portnum)) {bench::wipeKeyBuffer(&m,sizeof(m));return;}
      if(traffic_){
        const int slot=nodes.find(from);const auto *peer=slot>=0?nodes.get(unsigned(slot)):nullptr;
        const bool roleKnown=peer&&!peer->placeholder();
        uint8_t role=roleKnown?uint8_t(peer->role()):0;
        bool advisory=false;
#if defined(CONFIG_MT_WARM_PEERS)
        if(!roleKnown&&radioReceive&&to==UINT32_MAX&&m.portnum==meshtastic_PortNum_POSITION_APP&&
           !m.backendFailed&&!m.decodeRejected&&!m.signatureRejected&&!m.signatureQuarantined&&
           (!m.signatureLength||m.signatureVerified)&&positionRole_)
            advisory=positionRole_(from,role);
#endif
        unsigned precision=0;
        // An over-precise broadcast that cannot be relayed must not consume the
        // dedup window for the next compliant report in its cell.
        const bool dedupChannel=m.portnum==meshtastic_PortNum_POSITION_APP &&
          traffic_->deduplicatesPosition() && config.trafficPositionChannel(decodedSlot,precision) &&
          (to!=UINT32_MAX || positionRelayAllowed(decodedSlot,p[13],m));
        if(traffic_->decoded(from,to,route.identity(),m.portnum,m.payload,m.payloadLength,
            dedupChannel,precision,role,trafficUptime_,roleKnown,advisory)){
          trafficSuppressed_=true;bench::wipeKeyBuffer(&m,sizeof(m));return;
        }
      }
      if(radioReceive&&trafficNodeInfoSink_&&from!=route.identity()&&m.portnum==meshtastic_PortNum_NODEINFO_APP&&
         m.nodeInfoValid&&!m.backendFailed&&!m.decodeRejected&&!m.signatureRejected&&!m.signatureQuarantined&&
         nodeInfoPermitted&&privatePeerMetadata(from,m)&&
         (rememberProxyNodeInfoBits(from,uint8_t(decodedSlot),p[13],m,now),
          trafficNodeInfoSink_(trafficNodeInfoContext_,p,n,m,decodedSlot,now,random))){
        if(routeAccepted)*routeAccepted=true;bench::wipeKeyBuffer(&m,sizeof(m));return;
      }
      if(!endpointPacket) {
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
        if(radioReceive&&storeForwardSink_&&from!=route.identity()){
          // Advisory history never upgrades a shared-channel origin to trust.
          // Signed records require the normal verifier before archive admission.
          if(m.signatureLength)applyPublicSignature(p,n,m,now);
          if(!m.signatureRejected&&!m.signatureQuarantined)
            storeForwardSink_(storeForwardContext_,p,n,m,decodedSlot);
        }
#endif
        const bool admitted=receiveTransit(p,n,m,snr,now,random,radioReceive,utcNow);
        if(routeAccepted)*routeAccepted=admitted;
        bench::wipeKeyBuffer(&m,sizeof(m));return;
      }
      if(!refusedIdentity&&!privatePeerMetadata(from,m)){bench::wipeKeyBuffer(&m,sizeof(m));return;}
      if(m.signatureRejected){bench::wipeKeyBuffer(&m,sizeof(m));return;}
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE) || defined(CONFIG_MT_DROPZONE)
      if(radioReceive && !(p[12]&0x10u) && from!=route.identity() && (start || m.hasBitfield) &&
         routingPacketAllowed(false,p[13],from,to) &&
         (m.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP
#if defined(CONFIG_MT_REPLY_EXAMPLE)
          || stockExampleRequest(codec,p,n,m)
#endif
         ))
        observeStockOptional({from,to,bench::le32(p+8),m.portnum,decodedSlot,start,hop,bool(p[12]&8),
          m.wantResponse,m.hasBitfield,false,receivedRssi,receivedSnr,m.payload,m.payloadLength},now);
#else
      (void)receivedRssi;(void)receivedSnr;
#endif
#if defined(CONFIG_MT_GPIO_MODULES)
      // Command side effects are confined to exact decoded local/broadcast RF
      // requests. Runtime owns duplicate suppression and the explicit board
      // capability check. This callback never changes routing admission.
      if(radioReceive&&gpioFrameSink_&&from!=route.identity()&&
         m.portnum==meshtastic_PortNum_REMOTE_HARDWARE_APP) {
        uint8_t expected[239]{};size_t size=0;
        if(modules::GpioModuleWire::requestData(m,expected,sizeof(expected),size)&&
           codec.validateGpioModuleData(p,n,expected,size))
          gpioFrameSink_(gpioFrameContext_,p,n,m,decodedSlot,now,random);
        bench::wipeKeyBuffer(expected,sizeof(expected));
      }
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
      if(radioReceive&&storeForwardSink_&&from!=route.identity())
        storeForwardSink_(storeForwardContext_,p,n,m,decodedSlot);
#endif
      // Cache admission must match the complete phone-facing public Data
      // gate, including canonical outer protobuf shape and covered metadata.
      // This is advisory storage only; it does not change routing or delivery.
      if(endpointPacket&&stock::MeshBeaconListenerModule::wantPacket(meshBeaconListening_,m.portnum)&&
         m.payloadLength&&PublicApplication::received(codec,p,n,m)) {
        PrimaryChannel::Selection key{};
        if(config.channelSelection(decodedSlot,key)){
          (void)receivedBeacons_.remember(from,uint8_t(decodedSlot),key.bytes,key.length,m.payload,m.payloadLength,
                                          trafficUptime_,trafficUptime_);
          PrimaryChannel::clear(key);
        }
      }
      if(to==0xffffffffu) {
        const bool admitted=receiveTransit(p,n,m,snr,now,random,radioReceive,utcNow);
        if(routeAccepted)*routeAccepted=admitted;
      }
      if(receiving_)endpoint_.blocked(radio_.airtime(n));
      if(!refusedIdentity&&from!=route.identity()&&m.portnum==4&&m.nodeInfoValid&&
          NodeUser::permits(nodes,route.identity(),from,m.payload,m.payloadLength,start-hop)<0) {
        bench::wipeKeyBuffer(&m,sizeof(m));return;
      }
      stageVerifiedPeerCandidate(from,m,radioReceive,now);
      if(!refusedIdentity&&mqttFrameSink_&&from!=route.identity()&&!(p[12]&0x10u))
        mqttFrameSink_(mqttFrameContext_,p,n,m,decodedSlot);
      if(normalDiscovery_&&endpointArmed_&&txAllowed_&&from!=route.identity()&&Discovery::request(m)) {
        const bool verified=radioReceive&&m.signatureVerified&&!m.signatureQuarantined;
        const bool allowed=nodeDiscovery.observeRequest(from,now,verified);
        if(private_) {
          if(publicReady()&&allowed&&!bootstrapRequest_.ready)
            bootstrapRequest_={from,bench::le32(p+8),now+5000,uint8_t(Discovery::responseHop(p,config.hopLimit())),(p[12]&8)!=0,true,uint8_t(decodedSlot),verified};
          else ++nodeDiscovery.limited;
        } else if(allowed&&!queueNodeInfo(from,false,bench::le32(p+8),Discovery::responseHop(p,config.hopLimit()),(p[12]&8)!=0,now,random))
          nodeDiscovery.replied(from,now,verified);
        else ++nodeDiscovery.limited;
      }
      if(privateReady())private_->proof(endpoint_,p,n,m,now);
      // Sample current accounting at this receive/response boundary; uptime
      // availability remains a separate observation supplied by main.
      endpoint_.measuredAirUtilTx(airtime_.percent());
      endpoint_.measuredChannelUtilization(channel_.percent());
      endpoint_.receive(codec,route,route.identity(),p,n,m,snr,now,random,endpointArmed_&&txAllowed_);
      if(channelDelivered&&endpointPacket&&m.channelDecoded)*channelDelivered=true;
      bench::wipeKeyBuffer(&m,sizeof(m));return;
    }
    if(!relaying_)return;
    bench::RouteMetadata metadata;codec.decode(p,n,metadata);
    if(metadata.channelDecoded) {
      applyPublicSignature(p,n,metadata,now);
      if(!packetSignatureAllowed(p,metadata,now)){bench::wipeKeyBuffer(&metadata,sizeof(metadata));return;}
    }
    route.setRxSnr(snr);
    route.ingestWithMetadata(bench::Protocol::Meshtastic,p,n,metadata,now,random);
    bench::wipeKeyBuffer(&metadata,sizeof(metadata));
  }
  mqtt::MqttDownlinkAdmission::Decision processMqttDownlink(
      const mqtt::MqttServiceEnvelopeDecoder::Decoded &envelope,
      mqtt::MqttDownlinkAdmission::Context context,const char *gateway,
      uint32_t now,uint32_t random,uint32_t utcNow=0,uint32_t uptimeSeconds=0) {
    if(!normalMode_||mqttRadioOffFrame_)return mqtt::MqttDownlinkAdmission::Decision::ReceiveUnavailable;
    if(envelope.kind!=mqtt::MqttServiceEnvelopeDecoder::PacketKind::kEncrypted)
      return mqtt::MqttDownlinkAdmission::Decision::UnsupportedPacket;
    context.localNode=route.identity();context.localGatewayId=gateway;
    context.localGatewayIdLength=0;
    if(gateway)while(context.localGatewayIdLength<10&&gateway[context.localGatewayIdLength])++context.localGatewayIdLength;
    context.ignoreMqtt=config.ignoreMqtt();
    context.ignoreSender=config.ignoresIncoming(envelope.packet.from);
    context.receivePathReady=receiving_&&!fault_&&!privatePaused_;
    if(envelope.channelIdLength==3&&!memcmp(envelope.channelId,"PKI",3)) {
      // Stock PKI permission is any existing channel downlink grant. Channel
      // PSKs and envelope key/flag claims never establish private identity.
      context.channelDownlinkEnabled=config.anyChannelDownlinkEnabled();
      context.privateRuntimeReady=privateReady();
      context.pinnedPrivatePeer=private_&&private_->matchesPeer(envelope.packet.from);
      context.privateTransitReady=context.receivePathReady&&relaying_&&endpointArmed_&&txAllowed_&&
        config.rebroadcastMode()!=1&&config.rebroadcastMode()!=4;
      context.privateTransitSourceKnown=knownUser(envelope.packet.from);
      context.privateTransitDestinationKnown=knownUser(envelope.packet.to);
      const auto decision=mqtt::MqttDownlinkAdmission::evaluate(context,envelope);
      if(decision!=mqtt::MqttDownlinkAdmission::Decision::Allow)return decision;
      const bool localDelivery=envelope.packet.to==context.localNode;
      return mqttDownlinkFrame(envelope.packet,0,localDelivery,now,random,!localDelivery,utcNow,uptimeSeconds)?
        decision:mqtt::MqttDownlinkAdmission::Decision::InvalidPacket;
    }
    unsigned index=8;bool sawNamedChannel=false,ambiguous=false;
    ChannelRecord selectedRecord;PrimaryChannel::Selection key{};
    for(unsigned i=0;i<8;++i) {
      ChannelRecord record;if(!config.channelRecord(i,record)||record.role()==0)continue;
      const char *name=record.name()[0]?record.name():config.modemName();
      size_t n=0;while(n<=mqtt::MqttServiceEnvelopeDecoder::kMaxStringBytes&&name[n])++n;
      if(n==envelope.channelIdLength&&n<=mqtt::MqttServiceEnvelopeDecoder::kMaxStringBytes&&
         !memcmp(name,envelope.channelId,n)) {
        sawNamedChannel=true;PrimaryChannel::Selection candidate{};
        if(!config.channelSelection(i,candidate))continue;
        const bool packetMatches=envelope.packet.channel==candidate.hash;
        if(packetMatches) {
          if(index<8){ambiguous=true;PrimaryChannel::clear(candidate);continue;}
          index=i;selectedRecord=record;key=candidate;PrimaryChannel::clear(candidate);
        } else PrimaryChannel::clear(candidate);
      }
    }
    if(index>=8)return sawNamedChannel?mqtt::MqttDownlinkAdmission::Decision::KeyMismatch:
      mqtt::MqttDownlinkAdmission::Decision::ChannelMismatch;
    if(ambiguous){PrimaryChannel::clear(key);bench::wipeKeyBuffer(&selectedRecord,sizeof(selectedRecord));
      return mqtt::MqttDownlinkAdmission::Decision::KeyMismatch;}
    // A stock MQTT channel downlink is local input even before a region is
    // selected. Leave radio faults, storage holds, PKI and transit on their
    // existing paths. Local hash-zero unicast requires the radio's PKI-first
    // classification and must never fall back to a channel key here.
    if(!receiving_&&!fault_&&!privatePaused_&&!endpointArmed_&&!relaying_&&
       config.regionCode()==0&&nodes.ready()&&
       ((private_||privateRequired_)?publicReady():key.publicDefault)&&
       (envelope.packet.to==UINT32_MAX||(envelope.packet.to==context.localNode&&key.hash!=0)))
      context.receivePathReady=true;
    context.channelActive=selectedRecord.role()!=0;
    context.channelDownlinkEnabled=selectedRecord.downlink();
    context.channelIndex=uint8_t(index);context.channelHash=key.hash;
    context.channelKeyBound=key.length!=0;
    context.localChannelId=selectedRecord.name()[0]?
      selectedRecord.name():config.modemName();
    const auto decision=mqtt::MqttDownlinkAdmission::evaluate(context,envelope);
    PrimaryChannel::clear(key);
    if(decision==mqtt::MqttDownlinkAdmission::Decision::GatewayEcho) {
      const auto &packet=envelope.packet;
      // Stock only acknowledges a self-gateway echo whose packet source is
      // this node. Same-gateway foreign sources remain ignored.
      if(packet.from==route.identity()) {
        if(!packet.id||packet.from<=1||packet.from==mqtt::MqttDownlinkAdmission::kBroadcast||
           packet.from==mqtt::MqttDownlinkAdmission::kInvalidNode||packet.to<=1||
           packet.to==mqtt::MqttDownlinkAdmission::kInvalidNode||
           packet.hop_limit>mqtt::MqttDownlinkAdmission::kMaxHop||
           packet.hop_start>mqtt::MqttDownlinkAdmission::kMaxHop||
           packet.hop_limit>packet.hop_start||packet.next_hop>255||packet.relay_node>255||
           packet.public_key.size>sizeof(packet.public_key.bytes)) {
          bench::wipeKeyBuffer(&selectedRecord,sizeof(selectedRecord));
          return mqtt::MqttDownlinkAdmission::Decision::InvalidPacket;
        }
        if(!context.encryptionEnabled||
           packet.which_payload_variant!=meshtastic_MeshPacket_encrypted_tag||
           packet.pki_encrypted||packet.via_mqtt||!packet.encrypted.size||
           packet.encrypted.size>sizeof(packet.encrypted.bytes)) {
          bench::wipeKeyBuffer(&selectedRecord,sizeof(selectedRecord));
          return mqtt::MqttDownlinkAdmission::Decision::UnsupportedPacket;
        }
        if(packet.channel!=context.channelHash) {
          bench::wipeKeyBuffer(&selectedRecord,sizeof(selectedRecord));
          return mqtt::MqttDownlinkAdmission::Decision::KeyMismatch;
        }
        uint8_t echoFrame[255]{};
        const size_t echoSize=size_t(packet.encrypted.size)+16;
        if(echoSize>sizeof(echoFrame)) {
          bench::wipeKeyBuffer(&selectedRecord,sizeof(selectedRecord));
          return mqtt::MqttDownlinkAdmission::Decision::InvalidPacket;
        }
        const uint8_t flags=uint8_t((packet.hop_limit&7u)|(packet.want_ack?8u:0u)|0x10u|
                                    ((packet.hop_start&7u)<<5));
        for(unsigned i=0;i<4;++i) {
          echoFrame[i]=uint8_t(packet.to>>(8*i));
          echoFrame[4+i]=uint8_t(packet.from>>(8*i));
          echoFrame[8+i]=uint8_t(packet.id>>(8*i));
        }
        echoFrame[12]=flags;echoFrame[13]=context.channelHash;
        echoFrame[14]=uint8_t(packet.next_hop);echoFrame[15]=uint8_t(packet.relay_node);
        memcpy(echoFrame+16,packet.encrypted.bytes,packet.encrypted.size);
        (void)endpoint_.observeMqttSelfEcho(route.identity(),uint8_t(index),echoFrame,
                                           echoSize,now);
        bench::wipeKeyBuffer(echoFrame,sizeof(echoFrame));
      }
      bench::wipeKeyBuffer(&selectedRecord,sizeof(selectedRecord));
      return decision;
    }
    if(decision!=mqtt::MqttDownlinkAdmission::Decision::Allow){bench::wipeKeyBuffer(&selectedRecord,sizeof(selectedRecord));return decision;}

    ChannelScope scope(*this,index);if(!scope.valid()){bench::wipeKeyBuffer(&selectedRecord,sizeof(selectedRecord));return mqtt::MqttDownlinkAdmission::Decision::KeyMismatch;}
    // Reuse the same receive decoder, source-ignore/signature gates, duplicate
    // history, endpoint delivery, relay admission and radio scheduler as RF RX.
    const bool received=mqttDownlinkFrame(envelope.packet,context.channelHash,false,now,random,false,utcNow,uptimeSeconds);
    bench::wipeKeyBuffer(&selectedRecord,sizeof(selectedRecord));
    return received?mqtt::MqttDownlinkAdmission::Decision::Allow:
      mqtt::MqttDownlinkAdmission::Decision::InvalidPacket;
  }
  mqtt::client_proxy::Queue<1>::PushResult enqueueMqttProxyPublish(
      const mqtt::client_proxy::Message &message) {
    if(!normalMode_||!mqttProxyBridgeEnabled_)
      return mqtt::client_proxy::Queue<1>::PushResult::InvalidMessage;
    return mqttProxyPublishQueue_.push(message);
  }
  bool takeMqttProxyPublish(mqtt::client_proxy::Message &message) {
    if(!mqttProxyBridgeEnabled_){mqttProxyPublishQueue_.clear();return false;}
    return mqttProxyPublishQueue_.pop(message);
  }
  bool takeMqttProxyPublish(meshtastic_MqttClientProxyMessage &message) {
    if(!mqttProxyBridgeEnabled_){mqttProxyPublishQueue_.clear();message={};return false;}
    return mqttProxyPublishQueue_.popToProtobuf(message);
  }
  void clearMqttProxyPublish() {mqttProxyPublishQueue_.clear();}
  void setMqttProxyBridgeEnabled(bool enabled) {
    if(mqttProxyBridgeEnabled_&&!enabled)mqttProxyPublishQueue_.clear();
    mqttProxyBridgeEnabled_=enabled;
  }
  bool endpointArmed() const {return endpointArmed_;}
  uint32_t endpointCount() const {return endpointCount_;}
  bool armEndpoint(bool enable) {
    if(enable&&radio::RegionPolicy::fixedReceiveOnly(config.regionCode()))return false;
    if(enable&&private_&&!publicReady())return false;
    if(enable&&privateRequired_&&!publicReady())return false;
    if(privatePaused_)return false;
    if(!enable){
      endpointArmed_=false;nodeDiscovery.cancel();bootstrapRequest_={};
      if(normalMode_){
        if(active_){if(radio_.stop()!=0||radio_.start()!=0||radio_.health()!=1){fail();return false;}active_=false;normalActive_=nullptr;normalTransitActive_=false;noteTxUncertain();}
        endpoint_.cancel();route.clearPending();
      }
      return true;
    }
    if(normalMode_){if(!txAllowed_||!receiving_||fault_)return false;endpointArmed_=true;return true;}
    if(!txAllowed_||!receiving_||fault_||endpointCount_>=endpointLimit_)return false;
    endpointArmed_=true;return true;
  }
  int submitEndpoint(uint8_t owner,uint32_t id,const uint8_t *p,size_t n,uint32_t now) {
    if(codec.originsRestricted())return -EACCES; // Bench raw frames have no protected origin provenance.
    if(!endpointArmed_||!txAllowed_||!receiving_||fault_)return -EACCES;
    if(endpointCount_>=endpointLimit_)return -EACCES;
    if(active_||endpointQueued_||resultId_)return -EBUSY;
    if(!id||!p||n<17||n>255||p[12]!=0||bench::le32(p+4)!=route.identity()||bench::le32(p+8)!=id)return -EINVAL;
    endpointFrame_={};memcpy(endpointFrame_.bytes,p,n);endpointFrame_.len=n;endpointFrame_.expires=now+5000;
    endpointOwner_=owner;endpointId_=id;endpointQueued_=true;++endpointCount_;
    endpointArmed_=false; // One request consumes the approval; no setting resets the quota.
    return 0;
  }
#if defined(CONFIG_MT_RF_ADMIN_GET)
  // Distinct GET-only transport: no discovery, shared-channel or generic app
  // fallback. Correlation is installed only after the exact ID was queued.
  int submitPrivateAdminGet(uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
      unsigned hop,bool reliable,const uint8_t *key,size_t keySize,bool explicitPki,uint32_t now) {
    if(!normalMode_||!privateReady()||!explicitPki)return -EPERM;
    if(!endpointArmed_||!txAllowed_||!receiving_||fault_||privatePaused_)return -EACCES;
    if(owner>=LocalClientCount||to==route.identity())return -EINVAL;
    if(nodes.ignored(to))return -EPERM;
    admin::remote_protocol::Query query{};uint8_t pinned[32]{};
    if(!admin::remote_protocol::canonicalGetRequest(data,n,query))return -EINVAL;
    if(!private_->peerPublicIdentity(to,pinned))return -EPERM;
    struct Wipe {uint8_t *p;~Wipe(){bench::wipeKeyBuffer(p,32);}}wipe{pinned};
    if(keySize&&(keySize!=32||!key||memcmp(key,pinned,32)))return -EPERM;
    const auto preflight=privateAdminQueries_.canRecord(to,pinned,id,query,1u<<owner,now,30000);
    if(preflight!=admin::AdminQueryWindow::RecordResult::Recorded&&
       preflight!=admin::AdminQueryWindow::RecordResult::MergedOwner)
      return preflight==admin::AdminQueryWindow::RecordResult::Full?-ENOBUFS:-EINVAL;
    if(!pausePrivate())return -EBUSY;
    int result=private_->submitAdminGet(endpoint_,owner,to,id,data,n,hop,reliable,pinned,32,true,now);
    if(!result) {
      const auto recorded=privateAdminQueries_.recordConfirmedEnqueue(to,pinned,id,query,1u<<owner,now,30000);
      if(recorded!=admin::AdminQueryWindow::RecordResult::Recorded&&
         recorded!=admin::AdminQueryWindow::RecordResult::MergedOwner)result=-EPERM;
    }
    return resumePrivate()?result:-EPERM;
  }
#endif
  int submitPrivate(uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
    unsigned hop,bool reliable,const uint8_t *key,size_t keySize,bool explicitPki,uint32_t now) {
    // Stock licensed operation sends ordinary public plaintext and signs it;
    // it never falls back to PKI ciphertext. Caller key hints are not accepted.
    if(config.licensedOperation()) {
      if(explicitPki||keySize)return -EPERM;
      return submitLicensedUnicast(owner,to,id,data,n,hop,reliable,now);
    }
    if(!normalMode_||!privateReady())return -EPERM;
    if(!endpointArmed_||!txAllowed_||!receiving_||fault_||privatePaused_)return -EACCES;
    if(nodes.ignored(to))return -EPERM;
    if(!pausePrivate())return -EBUSY;
    const int ret=private_->submit(endpoint_,owner,to,id,data,n,hop,reliable,key,keySize,explicitPki,now);
    return resumePrivate()?ret:-EPERM;
  }
  int submitLicensedUnicast(uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                            unsigned hop,bool reliable,uint32_t now) {
    if(!normalMode_||!config.licensedOperation()||
       !endpointArmed_||!txAllowed_||!receiving_||fault_||privatePaused_||
       !private_||!publicReady())return -EACCES;
    if(owner>=LocalClientCount||!id||to<=1||to>=0xfffffffeu||to==route.identity()||
       hop>HopPolicy::Maximum||nodes.ignored(to)||!data||!n||n>239)return -EINVAL;
    PrimaryChannel::Selection activeChannel{};
    const bool licensedChannel=config.channelSelection(selectedChannel_,activeChannel)&&activeChannel.licensedPlaintext;
    PrimaryChannel::clear(activeChannel);
    if(!licensedChannel||!codec.licensedPlaintext())return -ENODEV;

    meshtastic_Data source{};bench::RouteMetadata metadata{};
    struct SourceWipe {meshtastic_Data &data;bench::RouteMetadata &metadata;~SourceWipe(){bench::wipeKeyBuffer(&data,sizeof(data));bench::wipeKeyBuffer(&metadata,sizeof(metadata));}} sourceWipe{source,metadata};
    auto input=pb_istream_from_buffer(data,n);
    if(!pb_decode(&input,meshtastic_Data_fields,&source)||input.bytes_left||
       source.payload.size>233||source.xeddsa_signature.size||source.dest||source.source)return -EINVAL;
    uint8_t canonical[239]{};auto canonicalOut=pb_ostream_from_buffer(canonical,sizeof(canonical));
    const bool canonicalInput=pb_encode(&canonicalOut,meshtastic_Data_fields,&source)&&
      canonicalOut.bytes_written==n&&!memcmp(canonical,data,n)&&bench::MtCodec::decodeData(data,n,metadata);
    bench::wipeKeyBuffer(canonical,sizeof(canonical));
    if(!canonicalInput||metadata.signatureLength||metadata.request_id)return -EINVAL;

    bool positionQuery=false,telemetryQuery=false,localStatsQuery=false,neighborQuery=false;
    uint8_t telemetryVariant=2;
    bool wantResponse=false;
    if(source.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP) {
      if(source.want_response||!source.has_bitfield||source.bitfield>1)return -EINVAL;
    } else if(source.portnum==meshtastic_PortNum_POSITION_APP||
              source.portnum==meshtastic_PortNum_TELEMETRY_APP||
              source.portnum==meshtastic_PortNum_NEIGHBORINFO_APP) {
      wantResponse=true;
      if(!source.want_response||!source.has_bitfield||source.bitfield!=2||source.reply_id||source.emoji)
        return -EINVAL;
      if(source.portnum==meshtastic_PortNum_POSITION_APP) {
        constexpr uint8_t request[]={8,3,24,1,72,2};
        if(n!=sizeof(request)||memcmp(data,request,sizeof(request)))return -EINVAL;
        positionQuery=true;
      } else if(source.portnum==meshtastic_PortNum_TELEMETRY_APP) {
        telemetryVariant=RemoteTelemetryQuery::requestVariant(source.payload.bytes,source.payload.size);
        if(!telemetryVariant)return -EINVAL;
        telemetryQuery=true;localStatsQuery=LocalStatsQuery::request(source.payload.bytes,source.payload.size);
      } else if(source.portnum==meshtastic_PortNum_NEIGHBORINFO_APP) {
        if(!NeighborQuery::requestShape(metadata))return -EINVAL;
        neighborQuery=true;
      }
    } else if(!PublicApplication::canonical(data,n)||source.want_response)return -EINVAL;
    size_t canonicalSignedSize=0;
    if(!canonicalSignableDataSize(metadata,canonicalSignedSize))return -EINVAL;
    const bool unsignedTooLarge=canonicalSignedSize+66u+16u>255u;
    const int repeat=endpoint_.repeatLicensedUnicast(owner,to,id,data,n,hop,reliable,wantResponse,
      positionQuery,telemetryQuery,localStatsQuery,telemetryVariant,neighborQuery,now,true,codec.channelHash());
    if(repeat!=-ENOENT)return repeat;
    if(!endpoint_.free())return -ENOBUFS;
    if(!pauseSharedOrigin())return -EBUSY;
    bench::Frame frame{};
    const bool encoded=private_->licensedUnicast(codec,to,id,data,n,frame);
    const bool restored=resumePrivate();
    if(!restored||!encoded||frame.len<17||frame.len>255||
       bench::le32(frame.bytes+4)!=route.identity()||bench::le32(frame.bytes)!=to||
       bench::le32(frame.bytes+8)!=id||frame.bytes[13]!=codec.channelHash()||
       (frame.bytes[12]&0x18u)||frame.bytes[14]||frame.bytes[15]!=uint8_t(route.identity())) {
      bench::wipeKeyBuffer(&frame,sizeof(frame));return -EPERM;
    }
    bench::RouteMetadata actual{};struct MetadataWipe {bench::RouteMetadata &value;~MetadataWipe(){bench::wipeKeyBuffer(&value,sizeof(value));}} metadataWipe{actual};
    if(!bench::MtCodec::decodeData(frame.bytes+16,frame.len-16,actual)||
       (actual.signatureLength!=64&&actual.signatureLength!=0)||
       (actual.signatureLength==0&&codec.signsPublic()&&metadata.portnum!=meshtastic_PortNum_ROUTING_APP&&metadata.portnum!=meshtastic_PortNum_TRACEROUTE_APP&&!unsignedTooLarge)) {
      bench::wipeKeyBuffer(&frame,sizeof(frame));return -EPERM;
    }
    meshtastic_Data signedData{};struct SignedDataWipe {meshtastic_Data &value;~SignedDataWipe(){bench::wipeKeyBuffer(&value,sizeof(value));}} signedDataWipe{signedData};
    auto signedInput=pb_istream_from_buffer(frame.bytes+16,frame.len-16);
    if(!pb_decode(&signedInput,meshtastic_Data_fields,&signedData)||signedInput.bytes_left) {
      bench::wipeKeyBuffer(&frame,sizeof(frame));return -EPERM;
    }
    signedData.xeddsa_signature.size=0;memset(signedData.xeddsa_signature.bytes,0,sizeof(signedData.xeddsa_signature.bytes));
    uint8_t unsignedCanonical[239]{};auto unsignedOut=pb_ostream_from_buffer(unsignedCanonical,sizeof(unsignedCanonical));
    const bool sameSource=pb_encode(&unsignedOut,meshtastic_Data_fields,&signedData)&&
      unsignedOut.bytes_written==n&&!memcmp(unsignedCanonical,data,n);
    bench::wipeKeyBuffer(unsignedCanonical,sizeof(unsignedCanonical));
    if(!sameSource) {bench::wipeKeyBuffer(&frame,sizeof(frame));return -EPERM;}
    frame.bytes[12]=uint8_t(hop|(hop<<5)|(reliable?8:0));
    const bool signedPublic=actual.signatureLength==64;
    const int result=endpoint_.submitLicensedUnicast(owner,frame,reliable,wantResponse,positionQuery,
      telemetryQuery,localStatsQuery,telemetryVariant,neighborQuery,signedPublic,now);
    bench::wipeKeyBuffer(&frame,sizeof(frame));
    if(result)return result;
    return 0;
  }
  int submitMessage(uint8_t owner,const uint8_t *p,size_t n,bool reliable,uint32_t now) {
    RadioDiagnostic::bump(radioDiagnostic_.submits);
    if(!localEndpointReady()||!codec.originAllowed())return submitOutcome(-EACCES,1);
    if(!p||n<17||n>255||bench::le32(p+4)!=route.identity())return submitOutcome(-EINVAL,2);
    bench::Frame origin{};origin.len=uint16_t(n);memcpy(origin.bytes,p,n);bool signedPublic=false;
    const bool decoded=actualPublicSignature(origin,signedPublic);bench::wipeKeyBuffer(&origin,sizeof(origin));
    if(!decoded)return submitOutcome(-EINVAL,2);
    return submitOutcome(endpoint_.submit(owner,p,n,reliable,now,false,true,false,nullptr,false,false,false,false,signedPublic),3);
  }
  // Built-in canned input and a dedicated serial peripheral originate normal
  // broadcast traffic. They share channel, airtime, nonce and retry admission
  // with companion messages, without borrowing a connected client's owner.
  static bool encodeLocalInputData(unsigned port,const uint8_t *payload,size_t length,
                                   uint8_t *data,size_t capacity,size_t &size) {
    size=0;
    if((port!=meshtastic_PortNum_TEXT_MESSAGE_APP&&port!=meshtastic_PortNum_SERIAL_APP)||
       !payload||!length||length>233||!data)return false;
    meshtastic_Data value{};
    value.portnum=static_cast<meshtastic_PortNum>(port);
    value.has_bitfield=true;
    value.payload.size=pb_size_t(length);
    memcpy(value.payload.bytes,payload,length);
    auto output=pb_ostream_from_buffer(data,capacity<239?capacity:239);
    if(!pb_encode(&output,meshtastic_Data_fields,&value))return false;
    size=output.bytes_written;
    return true;
  }
  int submitLocalInput(unsigned port,const uint8_t *payload,size_t length,unsigned channel,
                       bool reliable,uint32_t now,uint32_t random,uint32_t &idOut,bool positionSensitive=false) {
    idOut=0;
    if(!localEndpointReady()||
       (private_&&!publicReady()))return -EACCES;
    if((port!=meshtastic_PortNum_TEXT_MESSAGE_APP&&port!=meshtastic_PortNum_SERIAL_APP)||
       !payload||!length||length>233||channel>=8)return -EINVAL;
    if(!endpoint_.free())return -ENOBUFS;
    ChannelScope scope(*this,channel);
    if(!scope.valid())return -ENODEV;
    const uint32_t id=endpoint_.generatedId(random);
    if(!id)return -EAGAIN;
    uint8_t data[239]{};
    size_t dataSize=0;
    if(!encodeLocalInputData(port,payload,length,data,sizeof(data),dataSize)) {
      return -EMSGSIZE;
    }
    bench::Frame frame;
    bool encoded=false;
    if(port==meshtastic_PortNum_TEXT_MESSAGE_APP) {
      size_t size=0;
      encoded=encodePublicMessage(0xffffffffu,id,data,dataSize,frame.bytes,sizeof(frame.bytes),size);
      if(encoded)frame.len=size;
    } else {
      encoded=encodePublicApplication(0xffffffffu,id,data,dataSize,frame);
    }
    bench::wipeKeyBuffer(data,sizeof(data));
    if(!encoded){bench::wipeKeyBuffer(&frame,sizeof(frame));return -EMSGSIZE;}
    const unsigned hop=config.hopLimit();
    frame.bytes[12]=uint8_t(hop|(hop<<5));
    bool signedPublic=false;
    if(!actualPublicSignature(frame,signedPublic)){bench::wipeKeyBuffer(&frame,sizeof(frame));return -EINVAL;}
    const int result=endpoint_.submit(Endpoint::NoClientOwner,frame.bytes,frame.len,reliable,now,
      false,true,false,nullptr,false,false,false,false,
      signedPublic);
    bench::wipeKeyBuffer(&frame,sizeof(frame));
    if(!result){idOut=id;if(positionSensitive)endpoint_.protectLocalPosition(id);}
    return result;
  }
  // Native UI unicast keeps its own owner/result lane and uses the same
  // pinned-peer, durable nonce and channel transaction as companion PKI.
  int submitLocalPrivateInput(unsigned port,uint32_t to,const uint8_t *payload,size_t length,
                              unsigned channel,bool reliable,uint32_t now,uint32_t random,
                              uint32_t &idOut) {
    idOut=0;
    if(!normalMode_||!privateReady()||!endpointArmed_||!txAllowed_||!receiving_||fault_||privatePaused_)
      return -EACCES;
    if(port!=meshtastic_PortNum_TEXT_MESSAGE_APP||!to||to==UINT32_MAX||to==route.identity()||
       !payload||!length||length>233||channel>=8)return -EINVAL;
    if(nodes.ignored(to))return -EPERM;
    if(!endpoint_.free())return -ENOBUFS;
    struct Scratch {
      uint8_t data[227]{},key[32]{};
      ~Scratch(){bench::wipeKeyBuffer(this,sizeof(*this));}
    } scratch;
    // MultiPeerRuntime validates the requested active pin here; peer() is only
    // its first contact and is not an authorization check for this destination.
    if(!private_->peerPublicIdentity(to,scratch.key))return -EPERM;
    ChannelScope scope(*this,channel);
    if(!scope.valid())return -ENODEV;
    size_t size=0;
    if(!encodeLocalInputData(port,payload,length,scratch.data,sizeof(scratch.data),size))return -EMSGSIZE;
    const uint32_t id=endpoint_.generatedId(random);
    if(!id)return -EAGAIN;
    const int result=submitPrivate(Endpoint::LocalUiOwner,to,id,scratch.data,size,config.hopLimit(),
      reliable,scratch.key,sizeof(scratch.key),true,now);
    if(!result)idOut=id;
    return result;
  }
  bool encodePublicMessage(uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                           uint8_t *frame,size_t capacity,size_t &size) {
    size=0;
    if(!private_)return codec.encodeZeroHopData(route.identity(),to,id,data,n,frame,capacity,size);
    if(to!=0xffffffffu||!publicReady()||!pauseSharedOrigin())return false;
    bench::Frame signedFrame;
    const bool encoded=private_->broadcast(codec,id,data,n,signedFrame);
    if(!resumePrivate()||!encoded||signedFrame.len>capacity)return false;
    memcpy(frame,signedFrame.bytes,signedFrame.len);size=signedFrame.len;return true;
  }
  bool encodePublicApplication(uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                                bench::Frame &out) {
    if(!normalMode_||!id||!PublicApplication::destination(to)||to==route.identity()||
       !config.channelConfigured()||!PublicApplication::canonical(data,n))return false;
    bench::Frame candidate;size_t size=0;
    if(!private_) {
      const bool encoded=codec.encodeZeroHopData(route.identity(),to,id,data,n,candidate.bytes,sizeof(candidate.bytes),size);
      if(encoded){candidate.len=size;out=candidate;}
      bench::wipeKeyBuffer(&candidate,sizeof(candidate));return encoded;
    }
    if(!publicReady()||!pauseSharedOrigin())return false;
    const bool encoded=private_->encodePublicApplication(codec,to,id,data,n,candidate);
    const bool restored=resumePrivate();
    if(encoded&&restored)out=candidate;
    bench::wipeKeyBuffer(&candidate,sizeof(candidate));return encoded&&restored;
  }
  int submitPublicApplication(uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                              unsigned hop,bool reliable,uint32_t now) {
    if(!localEndpointReady()||
       (private_&&!publicReady()))return -EACCES;
    uint32_t port=0;
    if(owner>=LocalClientCount||!id||hop>HopPolicy::Maximum||!PublicApplication::destination(to)||to==route.identity()||
       nodes.ignored(to)||!PublicApplication::canonical(data,n,&port))return -EINVAL;
    const uint8_t requestedHop=uint8_t(hop);
    hop=hopScaling_.limit(hop,to,port);
    bool response=false;
    if(!endpoint_.resolveOriginPolicy(id,requestedHop,hop,response))return -EINVAL;
    const int repeat=endpoint_.repeatPublicApplication(codec,owner,route.identity(),to,id,data,n,hop,reliable,now);
    if(repeat!=-ENOENT)return repeat;
    if(!endpoint_.free())return -ENOBUFS;
    bench::Frame frame;
    if(!encodePublicApplication(to,id,data,n,frame))return -EPERM;
    frame.bytes[12]=uint8_t(hop|(hop<<5)|((reliable&&to!=0xffffffffu)?8:0));
    bool signedPublic=false;
    if(!actualPublicSignature(frame,signedPublic)){bench::wipeKeyBuffer(&frame,sizeof(frame));return -EINVAL;}
    // This marker describes only our locally encoded queue frame, never a
    // signature proof for a later received packet.
    const int result=endpoint_.submit(owner,frame.bytes,frame.len,reliable,now,false,true,false,
      nullptr,false,false,false,false,signedPublic,false,0,false,
      codec.licensedPlaintext()&&to!=0xffffffffu,requestedHop);
    bench::wipeKeyBuffer(&frame,sizeof(frame));return result;
  }
#if defined(CONFIG_MT_GPIO_MODULES)
  // Stock RemoteHardwareClient commands originate only on a channel actually
  // named gpio. Custom channel frames lease their packet ID durably through the
  // existing control encoder, but remain ordinary channel-encrypted packets.
  int submitGpioRequest(uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                       unsigned hop,bool reliable,uint32_t now) {
    if(!localEndpointReady()||
       (private_&&!publicReady()))return -EACCES;
    if(owner>=LocalClientCount||!id||hop>HopPolicy::Maximum||to<=1||to==0xffffffffu||
       to==0xfffffffeu||to==route.identity()||nodes.ignored(to)||
       !modules::GpioModuleChannels::gpio(config,selectedChannel_)||
       !PublicApplication::canonicalGpioRequest(data,n))return -EINVAL;
    const int repeat=endpoint_.repeatPublicApplication(codec,owner,route.identity(),to,id,data,n,hop,reliable,now);
    if(repeat!=-ENOENT)return repeat;
    if(!endpoint_.free())return -ENOBUFS;
    bench::Frame frame;bool encoded=false;
    const bool stockDefault=selectedChannel_==0&&modules::GpioModuleChannels::stockDefaultPrimary(config);
    if(stockDefault&&!codec.licensedPlaintext()) {
      size_t size=0;
      encoded=codec.encodeGpioRequest(route.identity(),to,id,data,n,frame.bytes,sizeof(frame.bytes),size);
      if(encoded)frame.len=size;
    } else if(codec.licensedPlaintext()) {
      if(!private_||!publicReady()||!pauseSharedOrigin())return -EACCES;
      const bool result=private_->licensedUnicast(codec,to,id,data,n,frame);
      const bool restored=resumePrivate();encoded=result&&restored;
    } else {
      if(!private_||!publicReady()||!pauseSharedOrigin())return -EACCES;
      const bool result=private_->encodeChannelControl(codec,to,id,data,n,frame);
      const bool restored=resumePrivate();encoded=result&&restored;
    }
    if(!encoded){bench::wipeKeyBuffer(&frame,sizeof(frame));return -EPERM;}
    frame.bytes[12]=uint8_t(hop|(hop<<5)|((reliable&&to!=0xffffffffu)?8:0));
    bool signedPublic=false;
    if(!actualPublicSignature(frame,signedPublic)){bench::wipeKeyBuffer(&frame,sizeof(frame));return -EINVAL;}
    const int result=endpoint_.submit(owner,frame.bytes,frame.len,reliable,now,false,true,false,
      nullptr,false,false,false,false,signedPublic);
    bench::wipeKeyBuffer(&frame,sizeof(frame));return result;
  }
#endif
  // Locally composed background modules share the control queue, never a
  // USB/BLE owner slot. Only one module frame may be pending, with no retry.
  int submitModuleAnnouncement(const uint8_t*data,size_t size,unsigned hop,uint32_t now,uint32_t random,uint32_t&idOut,bool beacon=false,bool serialWeather=false){
    idOut=0;bool telemetry=false,realtimeAudio=false,sensorReport=false;unsigned port=0;
    if(!localEndpointReady()||
       (private_&&!publicReady()))return -EACCES;
    if(hop>HopPolicy::Maximum||(beacon&&hop)||!ModuleAnnouncement::canonical(data,size,telemetry,beacon,&realtimeAudio,&port,&sensorReport))return -EINVAL;
    if(serialWeather&&!sensorReport)return -EINVAL;
    hop=hopScaling_.limit(hop,UINT32_MAX,port);
    if(endpoint_.modulePending())return -EAGAIN;
    const auto id=endpoint_.generatedId(random);if(!id)return -EAGAIN;
    bench::Frame frame;bool encoded=false;
    if(private_){
      if(!publicReady())return -EACCES;
      if(!pauseSharedOrigin())return -EAGAIN;
      const bool result=private_->broadcast(codec,id,data,size,frame);const bool restored=resumePrivate();encoded=result&&restored;
    }else{
      size_t n=0;encoded=codec.encodeZeroHopData(route.identity(),0xffffffffu,id,data,size,frame.bytes,sizeof(frame.bytes),n);if(encoded)frame.len=n;
    }
    if(!encoded){bench::wipeKeyBuffer(&frame,sizeof(frame));return -EPERM;}
    frame.bytes[12]=uint8_t(hop|(hop<<5));bool signedPublic=false;
    if(!actualPublicSignature(frame,signedPublic)){bench::wipeKeyBuffer(&frame,sizeof(frame));return -EINVAL;}
    const bool sensor=sensorReport&&config.role()==DeviceRolePolicy::Role::SENSOR;
    // WS85 SENSOR requests local retransmission with an implicit relay ACK.
    // Router::send always clears the broadcast ACK bit on the RF frame.
    const int result=endpoint_.submitModuleControl(frame,now,signedPublic,false,false,serialWeather&&sensor,0,realtimeAudio,sensor||serialWeather);
    bench::wipeKeyBuffer(&frame,sizeof(frame));if(!result)idOut=id;return result;
  }
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  int submitStoreForward(const modules::StoreForwardPacket &packet,uint32_t now,uint32_t random,uint32_t &idOut){
    idOut=0;
    if(!localEndpointReady()||!privateReady())return -EACCES;
    if(!packet.valid()||nodes.ignored(packet.to())||packet.to()==route.identity())return -EINVAL;
    // Capacity is stable under this serialized owner; reject pressure before
    // pausing RF or reserving a durable outgoing packet ID.
    if(!endpoint_.moduleControlAvailable())return -EAGAIN;
    ChannelScope scope(*this,packet.channel());if(!scope.valid())return -EINVAL;
    const auto id=endpoint_.generatedId(random);
    if(!id||id==packet.originalId()||!pausePrivate())return -EAGAIN;
    bench::Frame frame;
    const bool encoded=private_->encodeStoreForward(codec,packet,id,frame);
    const bool restored=resumePrivate();
    if(!encoded||!restored){bench::wipeKeyBuffer(&frame,sizeof(frame));return -EPERM;}
    frame.bytes[12]=packet.flags();frame.bytes[15]=uint8_t(route.identity());
    // Common control queue owns airtime, lifetime, completion and no retry.
    const int result=endpoint_.submitModuleControl(frame,now,false,false,true);
    bench::wipeKeyBuffer(&frame,sizeof(frame));if(!result)idOut=id;return result;
  }
#endif
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE) || defined(CONFIG_MT_DROPZONE)
#if defined(CONFIG_MT_REPLY_EXAMPLE)
  static bool stockExampleRequest(bench::MtCodec &codec,const uint8_t *p,size_t n,const bench::RouteMetadata &m){
    if(m.portnum!=meshtastic_PortNum_REPLY_APP || !m.wantResponse || !m.hasBitfield || m.bitfield>3 ||
       m.request_id || m.reply_id || m.emoji || m.payloadLength>233)return false;
    meshtastic_Data req{};req.portnum=meshtastic_PortNum_REPLY_APP;req.want_response=true;
    req.has_bitfield=true;req.bitfield=m.bitfield;req.payload.size=m.payloadLength;
    memcpy(req.payload.bytes,m.payload,m.payloadLength);
    uint8_t wire[239];auto out=pb_ostream_from_buffer(wire,sizeof(wire));
    return pb_encode(&out,meshtastic_Data_fields,&req) && codec.matchesEncodedData(p,n,wire,out.bytes_written,m.signatureVerified);
  }
#endif
  void observeStockOptional(const stock::optional::Request &q,uint32_t now){
    if(!normalMode_||!endpointArmed_||!txAllowed_||!receiving_||q.from==route.identity())return;
    if(q.port!=meshtastic_PortNum_TEXT_MESSAGE_APP && q.port!=meshtastic_PortNum_REPLY_APP)return;
    syncOptionalDomain();
    for(const auto &seen:stockSeen_)if(seen.from==q.from && seen.id==q.id && uint32_t(now-seen.at)<600000)return;
    stockSeen_[stockSeenIndex_++%64]={q.from,q.id,now};
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE)
    auto stage=[this](const stock::optional::Reply &reply) {
      if(stockReply_.ready)return;
      const size_t length=strnlen(reply.text,sizeof(stockReply_.text));
      if(length>=sizeof(stockReply_.text))return;
      memcpy(stockReply_.text,reply.text,length+1);
      stockReply_.reply=reply;stockReply_.reply.text=stockReply_.text;stockReply_.ready=true;
    };
#endif
#if defined(CONFIG_MT_REPLY_BOT)
    replyBot_.handleReceived(q,route.identity(),0,config.hopLimit(),now,stage);
#endif
#if defined(CONFIG_MT_REPLY_EXAMPLE)
    stock::optional::ReplyModule::handleReceived(q,route.identity(),HopPolicy::response(q.hopStart,q.hopLimit,config.hopLimit()),stage);
#endif
#if defined(CONFIG_MT_DROPZONE)
    if(q.port==meshtastic_PortNum_TEXT_MESSAGE_APP){
      const auto record=config.node();
      char shortName[5]{},longName[25]{};
      memcpy(shortName,record.bytes+bench::MtNodeRecord::Short,4);
      memcpy(longName,record.bytes+bench::MtNodeRecord::Name,24);
      dropzone_.handleReceived(q,shortName,longName,now);
    }
#endif
    (void)now;
  }
#endif
#if defined(CONFIG_MT_DROPZONE)
  bool takeDropzoneRequest(uint32_t now){
    syncOptionalDomain();return dropzone_.due(now);
  }
#endif
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE)
  void flushStockReply(uint32_t now,uint32_t random){
    syncOptionalDomain();
    if(!stockReply_.ready)return;
    (void)submitStockReply(stockReply_.reply,now,random);
    stockReply_={};
  }
  int submitStockReply(const stock::optional::Reply &reply,uint32_t now,uint32_t random) {
    if(!localEndpointReady()||
       (private_&&!publicReady()))return -EACCES;
    if(reply.channel>=8||reply.to<=1||reply.to>=0xfffffffeu||reply.to==route.identity()||
       nodes.ignored(reply.to)||reply.hop>HopPolicy::Maximum||!reply.text)return -EINVAL;
    if(!endpoint_.moduleControlAvailable())return -EAGAIN;
    meshtastic_Data d{};d.portnum=static_cast<meshtastic_PortNum>(reply.port);d.has_bitfield=true;
    const size_t len=strnlen(reply.text,96);if(len==96)return -EINVAL;
    d.payload.size=pb_size_t(len);memcpy(d.payload.bytes,reply.text,len);d.request_id=reply.requestId;
    if(!stock::optional::moduleReply(d))return -EINVAL;
    uint8_t wire[128]{};auto stream=pb_ostream_from_buffer(wire,sizeof(wire));
    if(!pb_encode(&stream,meshtastic_Data_fields,&d))return -EMSGSIZE;
    ChannelScope scope(*this,reply.channel);if(!scope.valid())return -ENODEV;
    const auto id=endpoint_.generatedId(random);if(!id)return -EAGAIN;
    const bool usePrivate=reply.privatePacket||(private_&&!config.licensedOperation()&&!stockChannelExcludesAutoPki(reply.channel));
    if(usePrivate){
      if(!privateReady()||config.licensedOperation()||
         privateDestinationDisposition(reply.to)!=pki::DestinationDisposition::PinnedActive)return -EPERM;
      if(!pausePrivate())return -EAGAIN;
      const int queued=private_->stockReply(endpoint_,reply.to,id,wire,stream.bytes_written,
                                             reply.hop,reply.wantAck,reply.requestId,now);
      return resumePrivate()?queued:-EIO;
    }
    bench::Frame frame{};
    if(private_){
      if(!pauseSharedOrigin())return -EAGAIN;
      const bool encoded=private_->encodeChannelControl(codec,reply.to,id,wire,stream.bytes_written,frame);
      if(!resumePrivate()||!encoded)return -EPERM;
    } else {
      size_t size=0;
      if(!codec.encodeZeroHopData(route.identity(),reply.to,id,wire,stream.bytes_written,
          frame.bytes,sizeof(frame.bytes),size))return -EINVAL;
      frame.len=size;
    }
    frame.bytes[12]=uint8_t(reply.hop|(reply.hop<<5)|(reply.wantAck?8u:0u));
    bool signedPublic=false;if(!actualPublicSignature(frame,signedPublic))return -EINVAL;
    return endpoint_.submitModuleControl(frame,now,signedPublic,false,false,reply.wantAck,reply.requestId);
  }
#endif
  bool moduleAnnouncementPending()const{return endpoint_.modulePending();}
#if defined(CONFIG_MT_GPIO_MODULES)
  // A narrow producer for the two GPIO modules. READ replies share ordinary
  // reliable endpoint jobs; unsolicited updates share the bounded control
  // queue. No callback acceptance is a statement of radio delivery.
  int submitGpioModule(unsigned port,const uint8_t *payload,size_t length,uint32_t to,
                       uint32_t request,unsigned channel,unsigned hop,bool wantAck,
                       uint32_t now,uint32_t random,uint32_t &idOut) {
    idOut=0;
    if(!localEndpointReady()||
       (private_&&!publicReady()))return -EACCES;
    const bool reply=request!=0;
    if(channel>=8||hop>HopPolicy::Maximum||to<=1||to==0xfffffffeu||to==route.identity()||
       (reply?(port!=meshtastic_PortNum_REMOTE_HARDWARE_APP||to==0xffffffffu):
              (to!=0xffffffffu||wantAck))||nodes.ignored(to))return -EINVAL;
    if(!endpoint_.moduleControlAvailable())return -EAGAIN;
    uint8_t data[64]{};size_t size=0;
    if(!modules::GpioModuleWire::outgoing(port,payload,length,request,data,sizeof(data),size))return -EINVAL;
    ChannelScope scope(*this,channel);if(!scope.valid())return -ENODEV;
    const auto id=endpoint_.generatedId(random);if(!id)return -EAGAIN;
    bench::Frame frame;
    bool encoded=false;
    if(reply)encoded=encodeChannelControl(to,id,data,size,frame);
    else if(private_) {
      if(!pauseSharedOrigin())return -EAGAIN;
      const bool result=private_->broadcast(codec,id,data,size,frame);
      const bool restored=resumePrivate();encoded=result&&restored;
    } else {
      size_t count=0;encoded=codec.encodeZeroHopData(route.identity(),to,id,data,size,
        frame.bytes,sizeof(frame.bytes),count);if(encoded)frame.len=count;
    }
    bench::wipeKeyBuffer(data,sizeof(data));
    if(!encoded){bench::wipeKeyBuffer(&frame,sizeof(frame));return -EPERM;}
    frame.bytes[12]=uint8_t(hop|(hop<<5)|((reply&&wantAck)?8:0));
    bool signedPublic=false;
    if(!actualPublicSignature(frame,signedPublic)){bench::wipeKeyBuffer(&frame,sizeof(frame));return -EINVAL;}
    const int result=endpoint_.submitModuleControl(frame,now,signedPublic,false,false,wantAck,
      reply?request:0);
    bench::wipeKeyBuffer(&frame,sizeof(frame));if(!result)idOut=id;return result;
  }
#endif
  // Fresh local position, unsigned channel control; no owner slot or retry.
#if defined(CONFIG_MT_TAK_TRACKER)
  bool takConfiguration(uint32_t team,uint32_t role,bool available=true) {
    if(!available||team>uint32_t(_meshtastic_Team_MAX)||role>uint32_t(_meshtastic_MemberRole_MAX)) {
      takReady_=false;takTeam_=takRole_=0;return false;
    }
    static_assert(_meshtastic_Team_MAX<=UINT8_MAX&&_meshtastic_MemberRole_MAX<=UINT8_MAX);
    takTeam_=uint8_t(team);takRole_=uint8_t(role);takReady_=true;return true;
  }
#endif
  int phoneNodeInfoPing(uint32_t now,uint32_t random) {
    if(!localEndpointReady()||!normalDiscovery_||!config.txEnabled()||configurationTransition_)return -EACCES;
    ChannelScope primary(*this,0);
    return primary.valid()?queueNodeInfo(UINT32_MAX,true,0,config.hopLimit(),false,now,random,true):-EACCES;
  }
  // The stock local mesh-ping button tries position before NodeInfo. Signed
  // position broadcasts currently admit announcement envelopes only; preserve
  // that signer contract and use the stock NodeInfo fallback in that case.
  int localMeshPing(bool hasPositionChannel,uint8_t channel,uint32_t now,uint32_t random) {
    if(!localEndpointReady())return -EAGAIN;
    if(hasPositionChannel&&!private_){
      ChannelScope selected(*this,channel);
      if(selected.valid()&&selectedDefault_&&announcePosition(now,random,true)==0)return 0;
    }
    ChannelScope primary(*this,0);
    return primary.valid()?queueNodeInfo(UINT32_MAX,true,0,config.hopLimit(),false,now,random):-EACCES;
  }
  int announcePositionOnChannel(uint8_t channel,uint32_t now,uint32_t random) {
    ChannelScope selected(*this,channel);
    return selected.valid()?announcePosition(now,random):-EACCES;
  }
  int announcePosition(uint32_t now,uint32_t random,bool wantReplies=false) {
#if defined(CONFIG_MT_TAK_TRACKER)
    if(config.role()==meshtastic_Config_DeviceConfig_Role_TAK_TRACKER)return announceTakPosition(now,random);
#endif
    if(!localEndpointReady()||endpoint_.modulePending())return -EAGAIN;
    uint8_t data[64];size_t size=0;meshtastic_Position p{};
    if(!localPosition(p)||!PositionQuery::fixedResponse(config,0,data,sizeof(data),size,selectedChannel_,&p,true,wantReplies))return -EACCES;
    const auto id=endpoint_.generatedId(random);if(!id)return -EAGAIN;
    bench::Frame frame;if(!encodeChannelControl(0xffffffffu,id,data,size,frame))return -EPERM;
    const unsigned hop=hopScaling_.limit(config.hopLimit(),UINT32_MAX,meshtastic_PortNum_POSITION_APP);
    frame.bytes[12]=uint8_t(hop|(hop<<5));
    bool signedPublic=false;
    if(!actualPublicSignature(frame,signedPublic)){bench::wipeKeyBuffer(&frame,sizeof(frame));return -EINVAL;}
    const int result=endpoint_.submitModuleControl(frame,now,signedPublic,true,false,false,0,false,config.role()==DeviceRolePolicy::Role::TRACKER);
    bench::wipeKeyBuffer(&frame,sizeof(frame));return result;
  }
  // Only custom-channel public controls acquire a durable outgoing-ID lease.
  // Private ACKs already hold their own pause and never enter this hook.
  bool encodeChannelControl(uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                            bench::Frame &out)override {
    if(selectedDefault_&&!(private_&&codec.signsPublic()&&to!=UINT32_MAX&&positionApplicationData(data,n))) {
      if(private_&&to==0xffffffffu&&positionApplicationData(data,n)) {
        if(!publicReady()||!pauseSharedOrigin())return false;
        const bool encoded=private_->broadcast(codec,id,data,n,out);
        const bool restored=resumePrivate();
        return encoded&&restored;
      }
      bench::Frame candidate;size_t size=0;
      const bool encoded=codec.encodeZeroHopData(route.identity(),to,id,data,n,
        candidate.bytes,sizeof(candidate.bytes),size);
      if(encoded){candidate.len=size;out=candidate;}
      bench::wipeKeyBuffer(&candidate,sizeof(candidate));return encoded;
    }
    if(!normalMode_||!config.channelConfigured()||!publicReady()||!pauseSharedOrigin())return false;
    bench::Frame candidate;
    const bool encoded=private_->encodeChannelControl(codec,to,id,data,n,candidate);
    // Resume once even if reservation/encoding failed. No frame can escape a
    // failed restore, including a successfully encrypted but burned packet ID.
    const bool restored=resumePrivate();
    if(encoded&&restored)out=candidate;
    bench::wipeKeyBuffer(&candidate,sizeof(candidate));return encoded&&restored;
  }
  bool licensedPlaintext()const override{return config.licensedOperation()&&codec.licensedPlaintext();}
  uint32_t localNode()const override{return route.identity();}
  void observeAuthenticatedOriginResponse(const bench::Frame &original,
      const uint8_t *air,size_t airSize,const bench::RouteMetadata &metadata,
      uint32_t expectedPeer,uint32_t now)override {
    (void)route.observeAuthenticatedPrivateResponse(original,air,airSize,metadata,expectedPeer,now);
  }
  void observeOpaqueOriginRelay(const bench::Frame &original,const uint8_t *air,
      size_t airSize,uint32_t now)override {
    (void)route.observeOpaqueOriginRelay(original,air,airSize,now);
  }
  bool encodePublicApplicationAck(uint32_t to,uint32_t id,const uint8_t *data,size_t n,bench::Frame &out)override {
    // Endpoint emits precisely Routing(NONE), fixed request_id and bitfield 0.
    // No caller Data or application want_response enters this control hook.
    if(!data||n!=13||data[0]!=8||data[1]!=5||data[2]!=18||data[3]!=2||data[4]!=24||data[5]||
       data[6]!=53||!bench::le32(data+7)||data[11]!=72||data[12])return false;
    if(!private_)return encodeChannelControl(to,id,data,n,out);
    if(!normalMode_||!publicReady()||!pauseSharedOrigin())return false;
    bench::Frame candidate;
    const bool encoded=private_->encodeChannelControl(codec,to,id,data,n,candidate);
    const bool restored=resumePrivate();
    if(encoded&&restored)out=candidate;
    bench::wipeKeyBuffer(&candidate,sizeof(candidate));return encoded&&restored;
  }
  bool copyReservedChannelControl(uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                                  uint8_t *out,size_t capacity,size_t &size) {
    size=0;if(!out||!data||!n||n>239||capacity<n+16)return false;
    bench::Frame candidate;
    const bool encoded=encodeChannelControl(to,id,data,n,candidate);
    const bool fits=encoded&&out&&candidate.len<=capacity;
    if(fits){memcpy(out,candidate.bytes,candidate.len);size=candidate.len;}
    bench::wipeKeyBuffer(&candidate,sizeof(candidate));return fits;
  }
  bool encodeTraceRouteQuery(uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                             uint8_t *out,size_t capacity,size_t &size) {
    if(selectedDefault_)return codec.encodeTraceRouteRequest(route.identity(),to,id,data,n,out,capacity,size);
    return copyReservedChannelControl(to,id,data,n,out,capacity,size);
  }
  bool encodePositionQuery(uint32_t to,uint32_t id,uint8_t *out,size_t capacity,size_t &size) {
    if(selectedDefault_&&(!private_||!codec.signsPublic()))return codec.encodePositionQuery(route.identity(),to,id,out,capacity,size);
    constexpr uint8_t data[]={8,3,24,1,72,2};
    return copyReservedChannelControl(to,id,data,sizeof(data),out,capacity,size);
  }
  bool encodeNeighborQuery(uint32_t to,uint32_t id,uint8_t *out,size_t capacity,size_t &size) {
    if(selectedDefault_)return codec.encodeNeighborQuery(route.identity(),to,id,out,capacity,size);
    constexpr uint8_t data[]={8,71,24,1,72,2};
    return copyReservedChannelControl(to,id,data,sizeof(data),out,capacity,size);
  }
  bool encodeTelemetryQuery(uint32_t to,uint32_t id,uint8_t *out,size_t capacity,size_t &size,bool localStatsQuery=false,uint8_t variant=0) {
    if(variant&&!RemoteTelemetryQuery::supportedVariant(variant)){size=0;return false;}
    if(selectedDefault_)return codec.encodeTelemetryQuery(route.identity(),to,id,out,capacity,size,localStatsQuery,variant);
    uint8_t data[]={8,67,18,2,18,0,24,1,72,2};
    data[4]=uint8_t(((variant?variant:uint8_t(localStatsQuery?6:2))<<3)|2);
    return copyReservedChannelControl(to,id,data,sizeof(data),out,capacity,size);
  }
  int submitTraceRoute(uint8_t owner,const uint8_t *p,size_t n,uint32_t now) {
    if(!localEndpointReady()||!codec.originAllowed())return -EACCES;
    if(!p||n<17||n>255||bench::le32(p+4)!=route.identity())return -EINVAL;
    return endpoint_.submit(owner,p,n,false,now,false,true,true,nullptr,false,true);
  }
  bool takeTraceRoute(uint8_t owner,uint32_t from,uint32_t to,uint32_t packet,
                      const bench::RouteMetadata &m,uint32_t now,uint8_t *payload,size_t capacity,size_t &size) {
    size=0;return normalMode_&&to==route.identity()&&from!=route.identity()&&
      TraceRoute::responseShape(m)&&endpoint_.takeTraceRoute(owner,from,packet,m.request_id,now,payload,capacity,size);
  }
  int submitPositionQuery(uint8_t owner,const uint8_t *p,size_t n,uint32_t now,bool signedLocally=false) {
    if(!localEndpointReady()||!codec.originAllowed())return -EACCES;
    if(!p||n<17||n>255||bench::le32(p+4)!=route.identity()||nodes.ignored(bench::le32(p)))return -EINVAL;
    return endpoint_.submit(owner,p,n,false,now,false,true,true,nullptr,false,false,true,false,signedLocally);
  }
  int submitChannelPositionQuery(uint8_t owner,uint32_t to,uint32_t id,unsigned hop,uint32_t now){
    if(!localEndpointReady()||nodes.ignored(to)||(private_&&!publicReady()))return -EACCES;
    if(owner>=LocalClientCount||hop>HopPolicy::Maximum)return -EINVAL;
    const int known=endpoint_.repeatPositionQuery(owner,id,to,hop,now);if(known!=-ENOENT)return known;
    // Empty canonical Position request, the optional 64-byte signature and CCM tag.
    uint8_t frame[22+66+bench::ChannelAead::TagSize]{};size_t size=0;
    if(!encodePositionQuery(to,id,frame,sizeof(frame),size))return -EACCES;
    frame[12]=uint8_t(hop|(hop<<5));
    return submitPositionQuery(owner,frame,size,now,private_&&codec.signsPublic());
  }
  bool takePositionQuery(uint8_t owner,uint32_t from,uint32_t to,uint32_t packet,
                         const bench::RouteMetadata &m,uint32_t now,uint8_t *payload,size_t capacity,size_t &size) {
    size=0;
    bench::RouteMetadata semantic=m;
    if((codec.licensedPlaintext()||m.portnum==meshtastic_PortNum_POSITION_APP)&&m.signatureVerified&&m.signatureLength==64)semantic.signatureLength=0;
    const bool accepted=normalMode_&&to==route.identity()&&from!=route.identity()&&Endpoint::positionResponseShape(semantic)&&
      endpoint_.takeTraceRoute(owner,from,packet,semantic.request_id,now,payload,capacity,size,true,false,false,false,uint8_t(semantic.portnum));
    bench::wipeKeyBuffer(&semantic,sizeof(semantic));if(!accepted)size=0;return accepted;
  }
  int submitNeighborQuery(uint8_t owner,const uint8_t *p,size_t n,uint32_t now) {
    if(!localEndpointReady()||!codec.originAllowed())return -EACCES;
    if(!p||n<17||n>255||bench::le32(p+4)!=route.identity()||nodes.ignored(bench::le32(p)))return -EINVAL;
    return endpoint_.submit(owner,p,n,false,now,false,true,true,nullptr,false,false,false,false,false,false,0,true);
  }
  bool takeNeighborQuery(uint8_t owner,uint32_t from,uint32_t to,uint32_t packet,
                         const bench::RouteMetadata &m,uint32_t now,uint8_t *payload,size_t capacity,size_t &size) {
    size=0;
    bench::RouteMetadata semantic=m;
    if(codec.licensedPlaintext()&&m.signatureVerified&&m.signatureLength==64)semantic.signatureLength=0;
    const bool accepted=normalMode_&&to==route.identity()&&from!=route.identity()&&NeighborQuery::responseShape(semantic)&&
      endpoint_.takeTraceRoute(owner,from,packet,semantic.request_id,now,payload,capacity,size,false,false,false,true);
    bench::wipeKeyBuffer(&semantic,sizeof(semantic));if(!accepted)size=0;return accepted;
  }
  bool takePrivateNeighbor(uint8_t owner,uint32_t from,uint32_t to,uint32_t packet,
                          uint32_t request,uint32_t now,uint8_t *payload,size_t capacity,size_t &size) {
    size=0;return normalMode_&&privateReady()&&to==route.identity()&&from!=route.identity()&&
      endpoint_.takeTraceRoute(owner,from,packet,request,now,payload,capacity,size,false,false,true,true);
  }
  void bindLocalStats(void (*source)(const NodeCache &,LocalStatsObservation &)){localStatsSource_=source;}
  LocalStatsObservation localStats()const {
    LocalStatsObservation out;out.packetsTx=completedPacketsTx_;
    // NodeDB total includes the local node and all stored peer records,
    // including local ignored placeholders. This is not an online estimate.
    out.totalNodes=uint16_t(1+nodes.count());
    out.relayed=completedRelays_;out.duplicates=route.stats.duplicates;
    out.canceled=route.routingStats.duplicateRelayCancelled;
    if(localStatsSource_)localStatsSource_(nodes,out);
    return out;
  }
  void measuredUptime(uint64_t seconds){endpoint_.measuredUptime(seconds);}
  void measuredPosition(const meshtastic_Position &value){endpoint_.measuredPosition(value);}
  bool localPosition(meshtastic_Position &out)const{return endpoint_.localPosition(out);}
  void measuredHardware(const HardwareSnapshot &value){hardware_=value;endpoint_.measuredHardware(value);}
  const HardwareSnapshot &hardware()const{return hardware_;}
  void extraTelemetry(void *context,bool (*produce)(void*,uint8_t,uint32_t,uint8_t*,size_t,size_t&)){endpoint_.extraTelemetry(context,produce);}
  void measuredEnvironment(const zt::telemetry::EnvironmentReadings &value){endpoint_.measuredEnvironment(value);}
  const zt::telemetry::EnvironmentReadings &environment()const{return endpoint_.environment();}
  void measuredPower(const zt::telemetry::PowerReadings &value){endpoint_.measuredPower(value);}
  const zt::telemetry::PowerReadings &power()const{return endpoint_.power();}
  zt::Observed<float> airUtilTx(){return airtime_.percent();}
  AirtimeMeter &airtimeMeter(){return airtime_;}
  zt::Observed<float> channelUtilization(){return channel_.percent();}
  ChannelMeter &channelMeter(){return channel_;}
  int submitTelemetryQuery(uint8_t owner,const uint8_t *p,size_t n,uint32_t now,bool localStatsQuery=false,uint8_t variant=0) {
    if(!localEndpointReady()||!codec.originAllowed())return -EACCES;
    if(!p||n<17||n>255||bench::le32(p+4)!=route.identity()||nodes.ignored(bench::le32(p)))return -EINVAL;
    return endpoint_.submit(owner,p,n,false,now,false,true,true,nullptr,false,false,false,true,false,localStatsQuery,variant);
  }
  bool takeTelemetryQuery(uint8_t owner,uint32_t from,uint32_t to,uint32_t packet,
                         const bench::RouteMetadata &m,uint32_t now,uint8_t *payload,size_t capacity,size_t &size) {
    size=0;
    bench::RouteMetadata semantic=m;
    if(codec.licensedPlaintext()&&m.signatureVerified&&m.signatureLength==64)semantic.signatureLength=0;
    const bool accepted=normalMode_&&to==route.identity()&&from!=route.identity()&&TelemetryQuery::responseShape(semantic)&&
      endpoint_.takeTraceRoute(owner,from,packet,semantic.request_id,now,payload,capacity,size,false,true);
    bench::wipeKeyBuffer(&semantic,sizeof(semantic));if(!accepted)size=0;return accepted;
  }
  bool takePrivateTelemetry(uint8_t owner,uint32_t from,uint32_t to,uint32_t packet,
                            uint32_t request,uint32_t now,uint8_t *payload,size_t capacity,size_t &size) {
    size=0;return normalMode_&&privateReady()&&to==route.identity()&&from!=route.identity()&&
      endpoint_.takeTraceRoute(owner,from,packet,request,now,payload,capacity,size,false,true,true);
  }
  bool takePrivatePosition(uint8_t owner,uint32_t from,uint32_t to,uint32_t packet,
                           uint32_t request,uint32_t now,uint8_t *payload,size_t capacity,size_t &size,uint8_t positionPort=3) {
    size=0;return normalMode_&&privateReady()&&to==route.identity()&&from!=route.identity()&&
      endpoint_.takeTraceRoute(owner,from,packet,request,now,payload,capacity,size,true,false,true,false,positionPort);
  }
  bool matchedNodeInfo(uint32_t from,uint32_t to,const bench::RouteMetadata &m,uint32_t now)const {
    return normalMode_&&to==route.identity()&&from!=route.identity()&&
      Endpoint::nodeInfoResponse(m)&&NodeUser::accepted(nodes,from,m.payload,m.payloadLength)&&
      endpoint_.matchedNodeInfo(from,m.request_id,now);
  }
  int submitNodeInfo(uint8_t owner,uint32_t id,uint32_t to,unsigned hop,bool reliable,bool wantResponse,uint32_t now) {
    if(!normalMode_||!normalDiscovery_||!endpointArmed_||!txAllowed_||!receiving_||fault_)return -EACCES;
    if(hop>HopPolicy::Maximum||to==route.identity())return -EINVAL;
    const uint8_t requestedOptions=uint8_t(hop|(wantResponse?8u:0u));
    hop=hopScaling_.limit(hop,to,meshtastic_PortNum_NODEINFO_APP);
    wantResponse=stock::nodeinfo::sendOurNodeInfo(wantResponse,config.role());
    if(!endpoint_.resolveOriginPolicy(id,requestedOptions,hop,wantResponse))return -EINVAL;
    if(private_) {
      if(!publicReady())return -EPERM;
      const int repeat=endpoint_.repeatSignedNodeInfo(owner,id,to,hop,reliable,wantResponse,now,true,codec.channelHash());
      if(repeat!=-ENOENT)return repeat;
    }
    const bool permit=broadcastDue(history::Kind::NodeInfo,60000)&&nodeDiscovery.allowed(now,nodeInfoChannelAllowed())&&!endpoint_.nodeInfoPending();
    if(private_&&(!permit||!endpoint_.free()))return -EAGAIN;
    bench::Frame frame;
    if(!encodeNodeInfo(to,id,wantResponse,0,frame))return -EINVAL;
    frame.bytes[12]=uint8_t(hop|(hop<<5)|((reliable&&to!=0xffffffffu)?8:0));
    bool created=false;
    const int ret=endpoint_.submit(owner,frame.bytes,frame.len,reliable,now,true,permit,wantResponse,&created,
                                   false,false,false,false,private_&&codec.signsPublic(),false,0,false,false,requestedOptions);
    if(!ret&&created){nodeDiscovery.admitted(now,config.node().interval());broadcastAdmitted(history::Kind::NodeInfo);}
    return ret;
  }
  bool takeEndpointResult(uint8_t owner,uint32_t &id,int &status) {
    if(!resultId_||owner!=resultOwner_)return false;
    id=resultId_;status=resultStatus_;resultId_=0;return true;
  }
  bool announce(uint32_t now,uint32_t random) {return discovery.request(now,random,relaying_&&!fault_);}
  void poll(uint32_t now,uint32_t random) {
    airtime_.ensureAnchored();
#if defined(CONFIG_MT_RF_ADMIN_GET)
    legacyAdminNow_=now;
    if(legacyAdminReplyQueued_&&!endpoint_.modulePending())legacyAdminReplyQueued_=false;
    expireCompletedLegacyAdminSets(now);
    if(legacyAdminRequest_.state_==admin::LegacyAdminRequest::State::Queued&&
       uint32_t(now-legacyAdminRequest_.receivedAt_)>=LegacyAdminRequestLifetimeMs)
      clearLegacyAdminRequest();
    if(legacyAdminBusy_.ready&&
       uint32_t(now-legacyAdminBusy_.receivedAt)>=LegacyAdminRequestLifetimeMs)
      bench::wipeKeyBuffer(&legacyAdminBusy_,sizeof(legacyAdminBusy_));
#endif
    if(!receiving_||fault_)return;
    if(normalMode_){pollNormal(now,random);return;}
    if(active_) {
      int result=0;
      if(!radio_.completion(result)) {if(bench::due(now,deadline_)){++failed;noteTxUncertain();if(endpointActive_)endpointResult(-ETIMEDOUT);fail();}return;}
      active_=false;quiet_=now+250;
      // Known terminal completion: a clean result is credited exactly once
      // before the RX-restore check, so a known duration survives a failed
      // restore. An error status is an uncertain outcome (possible early
      // abort), tracked and never credited.
      if(result){noteTxUncertain();++failed;if(endpointActive_)endpointResult(result);fail();return;}
      creditTxPending(bench::le32(activeFrame_.bytes+4)!=route.identity());
      if(endpointActive_)endpointResult(0);
      else if(own_)discovery.finish(now,true);else route.complete(activeFrame_,now,true);
      own_=false;++sent;
      if(radio_.restore()!=0||radio_.health()!=1){fail();return;}
    }
    if(radio_.health()!=1){fail();return;}
    if(endpointQueued_) {
      if(bench::due(now,endpointFrame_.expires)){endpointResult(-ETIMEDOUT);return;}
      if(!bench::due(now,quiet_)||radio_.busy())return;
      const uint32_t air=radio_.airtime(endpointFrame_.len);
      RadioTiming::Deadline timing;
      if(!air||air>2500||!RadioTiming::resolve(air,timing)){endpointResult(-EMSGSIZE);return;}
      if(uint32_t(now-window_)>=600000){window_=now;budget_=0;}
      if(budget_+air>6000){endpointResult(-EAGAIN);return;}
      budget_+=air;activeFrame_=endpointFrame_;endpointQueued_=false;endpointActive_=true;
      const int ret=radio_.send(activeFrame_.bytes,activeFrame_.len);
      if(ret){++failed;endpointResult(ret);if(radio_.restore()!=0)fail();return;} // no retries
      active_=true;own_=false;deadline_=now+timing.hostMs;noteTxAcceptedStart(air);return;
    }
    if(!relaying_||!txAllowed_)return;
    discovery.poll(now,random,true);
    bench::Frame *q=route.next(now);own_=false;
    if(!q){q=discovery.next(now);own_=q!=nullptr;}
    if(!q||!bench::due(now,quiet_))return;
    const uint32_t air=radio_.airtime(q->len);
    if(!air){++failed;fail();return;}
    // LongFast maximum 255-byte airtime is ~2157 ms, above the old dual
    // broker's 2000 ms policy cap. A policy refusal must not kill RX.
    if(air>2500){++failed;if(own_)discovery.finish(now,false);else route.complete(*q,now,false);own_=false;return;}
    RadioTiming::Deadline timing;
    if(!RadioTiming::resolve(air,timing)){++failed;fail();return;}
    if(uint32_t(now-window_)>=600000){window_=now;budget_=0;}
    if(budget_+air>6000||radio_.busy()) {
      ++throttled;if(own_)discovery.defer(now,random);else route.defer(*q,now,random);return;
    }
    if(!own_)route.noteTransmitStart(*q);
    activeFrame_=*q;q->used=false;++activeFrame_.attempts;
    if(own_)discovery.started();
    // Reserve even an unsuccessful start; no unlimited retry airtime bypass.
    budget_+=air;
    const int ret=radio_.send(activeFrame_.bytes,activeFrame_.len);
    if(ret==0){active_=true;deadline_=now+timing.hostMs;route.noteBlockedAirtime(air+250);noteTxAcceptedStart(air);return;}
    if(ret==-EBUSY) {
      if(radio_.restore()!=0||radio_.health()!=1){fail();return;}
      if(own_)discovery.retry(activeFrame_,now);else route.retry(activeFrame_,now);
      own_=false;quiet_=now+250;return;
    }
    ++failed;fail();
  }
};
}
