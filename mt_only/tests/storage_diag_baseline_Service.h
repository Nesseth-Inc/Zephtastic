// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "RoutingCore.h"
#include "MtSignature.h"
#include "MtNodeInfo.h"
#include "Endpoint.h"
#include "pki/PrivateRuntime.h"
#include "Config.h"
#include "Discovery.h"
#include "NodeCache.h"
#include "NodeUser.h"
#include "AirtimeMeter.h"
#include <errno.h>

namespace mt {
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
  virtual bool ready()=0;
  virtual int start()=0;
  virtual int restore()=0;
  virtual int stop()=0;
  // Storage/config transitions need a stronger stop than ordinary shutdown:
  // implementations must preserve completed application RX events and return
  // -EAGAIN after restoring RX if an event raced the transition. A hard error
  // cannot authorize stopped service bookkeeping; -ESHUTDOWN is the special
  // fail-closed result for verified physical standby and reconciled events.
  virtual int stopForStorage(){return stop();}
  virtual int health()=0;
  virtual bool busy()=0;
  virtual uint32_t airtime(size_t n)=0;
  virtual int send(uint8_t *p,size_t n)=0;
  virtual bool completion(int &result)=0;
};
class Service {
  Radio &radio_;
  bool txAllowed_,receiving_=false,relaying_=false,fault_=false,active_=false,own_=false;
  uint32_t deadline_=0,quiet_=0,window_=0,budget_=0;
  bench::Frame activeFrame_,endpointFrame_;
  bool endpointArmed_=false,endpointQueued_=false,endpointActive_=false;
  uint8_t endpointOwner_=0,resultOwner_=0;
  uint32_t endpointCount_=0,endpointLimit_=1,endpointId_=0,resultId_=0;
  int resultStatus_=0;
  bool normalMode_=false,normalDiscovery_=true;
  uint8_t hardwareModel_=81;
  Endpoint endpoint_;
  pki::PrivateRuntime *private_=nullptr;
  bool privatePaused_=false,privateWasReceiving_=false;
  struct BootstrapRequest {uint32_t to=0,request=0,expires=0;uint8_t hop=0;bool reliable=false,ready=false;};
  BootstrapRequest bootstrapRequest_{};
  Endpoint::Job *normalActive_=nullptr;
  bool normalTransitActive_=false;
  static constexpr unsigned TraceAssociations=16;
  struct TraceAssociation {
    TraceRoute::Discovery incoming{},outgoing{};
    uint32_t from=0,id=0,to=0,request=0,at=0;
    uint8_t channel=0,start=0,next=0;
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
  static bool matchesTraceAssociation(const TraceAssociation &entry,const uint8_t *p,
                                      const bench::RouteMetadata &metadata,
                                      const TraceRoute::Discovery &semantic,unsigned start) {
    return entry.to==bench::le32(p)&&entry.channel==p[13]&&entry.start==start&&
      entry.next==p[14]&&entry.response==!metadata.wantResponse&&
      entry.request==metadata.request_id&&TraceRoute::sameSemantic(entry.incoming,semantic);
  }
  static bool matchesTraceLoopAssociation(const TraceAssociation &entry,const uint8_t *p,
                                          const bench::RouteMetadata &metadata,
                                          const TraceRoute::Discovery &semantic,unsigned start) {
    return entry.to==bench::le32(p)&&entry.channel==p[13]&&entry.start==start&&
      entry.next==p[14]&&entry.response==!metadata.wantResponse&&
      entry.request==metadata.request_id&&TraceRoute::sameSemantic(entry.outgoing,semantic);
  }
  void endpointResult(int status) {resultId_=endpointId_;resultOwner_=endpointOwner_;resultStatus_=status;endpointQueued_=endpointActive_=false;}
  void stoppedState() {
    // A stopped or canceled in-flight TX has an unknown outcome: tracked in
    // the bounded quality counter, never credited as airtime or zeroed.
    airtime_.noteUncertain();
    if(endpointQueued_||endpointActive_)endpointResult(-ECANCELED);
    endpointArmed_=false;nodeDiscovery.cancel();bootstrapRequest_={};
    if(private_)private_->clearEvent();
    privatePaused_=false;receiving_=relaying_=false;
    endpoint_.cancel();normalActive_=nullptr;normalTransitActive_=false;
    active_=own_=false;route.clearPending();discovery.cancel();
  }

  static uint32_t airtime(size_t n,void *ctx) {return static_cast<Radio *>(ctx)->airtime(n);}
  void fail() {fault_=true;if(private_)private_->freeze();stop();}
  bool privateReady()const {
    return private_&&private_->ready(route.identity())&&!nodes.ignored(private_->peer());
  }
  bool pausePrivate() {
    if(privatePaused_||fault_||active_||own_||normalActive_||!radio_.ready()||radio_.busy())return false;
    privateWasReceiving_=receiving_;
    if(radio_.stop()!=0||radio_.health()!=0){fail();return false;}
    receiving_=false;privatePaused_=true;return true;
  }
  bool resumePrivate() {
    if(!privatePaused_){fail();return false;}
    privatePaused_=false;
    if(!privateReady()){fail();return false;}
    if(privateWasReceiving_) {
      if(radio_.start()!=0||radio_.health()!=1){fail();return false;}
      receiving_=true;
    } else if(radio_.health()!=0){fail();return false;}
    return true;
  }
  bool encodeNodeInfo(uint32_t to,uint32_t id,bool wantResponse,uint32_t request,bench::Frame &frame) {
    if(!private_)return Discovery::encode(codec,config.node(),route.identity(),hardwareModel_,to,id,wantResponse,request,frame);
    if(!privateReady()||!pausePrivate())return false;
    const bool encoded=private_->bootstrap(codec,config.node(),hardwareModel_,to,id,wantResponse,request,frame);
    return resumePrivate()&&encoded;
  }
  int queueNodeInfo(uint32_t to,bool wantResponse,uint32_t request,unsigned hop,
                    bool reliable,uint32_t now,uint32_t random) {
    if(!nodeDiscovery.allowed(now)||endpoint_.nodeInfoPending())return -EAGAIN;
    const uint32_t id=endpoint_.generatedId(random);bench::Frame f;
    if(!id||!encodeNodeInfo(to,id,wantResponse,request,f))return -EINVAL;
    f.bytes[12]=uint8_t(hop|(hop<<5)|((reliable&&to!=0xffffffffu)?8:0));
    const int ret=endpoint_.submitNodeInfoControl(f,reliable,now,private_!=nullptr);
    if(!ret)nodeDiscovery.admitted(now,config.node().interval());
    return ret;
  }
  void receiveTraceTransit(const uint8_t *p,size_t n,const bench::RouteMetadata &metadata,
                           int8_t snr,uint32_t now,uint32_t random) {
    const auto from=bench::le32(p+4),to=bench::le32(p),packet=bench::le32(p+8);
    const unsigned hop=p[12]&7,start=(p[12]>>5)&7;
    if(private_||!endpointArmed_||!txAllowed_||p[13]!=codec.channelHash()||!p[13]||
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
      slot->request=metadata.request_id;slot->at=now;slot->channel=p[13];
      slot->start=uint8_t(start);slot->next=p[14];slot->response=!metadata.wantResponse;slot->used=true;
    } else if(admitted){association->outgoing=outgoing;association->at=now;}
    bench::wipeKeyBuffer(payload,sizeof(payload));bench::wipeKeyBuffer(oldData,sizeof(oldData));
    bench::wipeKeyBuffer(newData,sizeof(newData));bench::wipeKeyBuffer(rewritten,sizeof(rewritten));
    bench::wipeKeyBuffer(&rewrittenMetadata,sizeof(rewrittenMetadata));
  }
  void pollNormal(uint32_t now,uint32_t random) {
    if(private_) {
      if(!privateReady()){private_->clearEvent();endpoint_.cancelPrivate();bootstrapRequest_={};}
      else private_->sweep(endpoint_);
    }
    // Signed replies run after RX returns, so its decoded metadata/scratch does
    // not remain on the 8KiB main stack across the signer/curve call chain.
    if(bootstrapRequest_.ready) {
      if(bench::due(now,bootstrapRequest_.expires)||!endpointArmed_||!txAllowed_)bootstrapRequest_={};
      else if(!active_&&!radio_.busy()) {
        const auto request=bootstrapRequest_;bootstrapRequest_={};
        if(!nodes.ignored(request.to)&&nodeDiscovery.replyAllowed(request.to,now)&&
          !queueNodeInfo(request.to,false,request.request,request.hop,request.reliable,now,random))nodeDiscovery.replied(request.to,now);
        else ++nodeDiscovery.limited;
      }
    }
    nodeDiscovery.observeArm(normalDiscovery_&&endpointArmed_&&txAllowed_,now);
    if(nodeDiscovery.periodic(now)&&queueNodeInfo(0xffffffffu,false,0,3,false,now,random))nodeDiscovery.defer(now);
    endpoint_.tick(now,random);
    if(active_) {
      int result=0;
      if(!radio_.completion(result)){if(bench::due(now,deadline_)){++failed;airtime_.noteUncertain();fail();}return;}
      active_=false;quiet_=now+250;
      // Known terminal completion: a clean result is credited exactly once
      // before the RX-restore check, so a known duration survives a failed
      // restore. An error status is an uncertain outcome (possible early
      // abort), tracked and never credited.
      if(result)airtime_.noteUncertain();else airtime_.creditPending();
      if(result||radio_.restore()!=0||radio_.health()!=1){
        if(normalTransitActive_)route.complete(activeFrame_,now,false);
        normalTransitActive_=false;++failed;fail();return;
      }
      ++sent;
      if(normalTransitActive_)route.complete(activeFrame_,now,true);
      else endpoint_.complete(*normalActive_,now,random);
      normalActive_=nullptr;normalTransitActive_=false;
    }
    if(radio_.health()!=1){fail();return;}
    if(!endpointArmed_||!txAllowed_||!bench::due(now,quiet_)||radio_.busy())return;
    auto *j=endpoint_.next(now);bench::Frame *transit=j?nullptr:route.next(now);
    if(!j&&!transit)return;
    if(j&&j->privateMessage&&(!privateReady()||nodes.ignored(bench::le32(j->frame.bytes)))){endpoint_.refused(*j,4);return;}
    const auto air=radio_.airtime(j?j->frame.len:transit->len);
    if(!air||air>2500){
      if(j)endpoint_.refused(*j,7);else {++failed;route.complete(*transit,now,false);}
      return;
    }
    if(uint32_t(now-window_)>=600000){window_=now;budget_=0;}
    if(budget_+air>6000){
      ++throttled;if(j)endpoint_.refused(*j,9);else route.defer(*transit,now,random);return;
    }
    budget_+=air;
    if(j)activeFrame_=j->frame;
    else {route.noteTransmitStart(*transit);activeFrame_=*transit;transit->used=false;++activeFrame_.attempts;}
    // Stable buffer until native completion.
    const int ret=radio_.send(activeFrame_.bytes,activeFrame_.len);
    quiet_=now+250;
    if(j)endpoint_.started(*j,ret,now,air,random);
    if(!ret){
      active_=true;normalActive_=j;normalTransitActive_=transit!=nullptr;deadline_=now+air+1000;
      if(j)endpoint_.blocked(air+250);else route.noteBlockedAirtime(air+250);
      airtime_.noteAcceptedStart(air);
      return;
    }
    if(transit) {
      if(ret==-EBUSY) {
        const int restored=radio_.restore();
        const int health=restored?0:radio_.health();
        if(restored||health!=1){++failed;fail();return;}
        if(route.retry(activeFrame_,now))return;
        route.retireRetryRefusal(activeFrame_);
        normalActive_=nullptr;normalTransitActive_=false;++failed;return;
      }
      route.complete(activeFrame_,now,false);++failed;fail();return;
    }
    if(ret!=-EBUSY||radio_.restore()!=0||radio_.health()!=1){++failed;fail();}
  }
public:
  Config config;
  NodeCache nodes;
  bench::RoutingCore route;
  bench::MtCodec codec;
  bench::MtSignature signature;
  bench::MtNodeInfo discovery;
  Discovery nodeDiscovery;
  uint32_t received=0,rejected=0,failed=0,sent=0,throttled=0,traceLoopObservations=0;
  AirtimeMeter airtime_;
  Service(Radio &r,bool allowTx,uint32_t node,uint8_t model,bool normalMode=false,bool normalDiscovery=true,pki::PrivateRuntime *privateRuntime=nullptr):radio_(r),txAllowed_(allowTx),normalMode_(normalMode),normalDiscovery_(normalDiscovery),hardwareModel_(model),private_(privateRuntime) {
    route.configureContinuous(uint8_t(node),8);route.configureIdentity(node);
    route.setAirtimeProvider(airtime,&radio_);discovery.identity(node);discovery.hardwareModel(model);
    apply(config);
  }
  bool apply(const Config &c) {
    if(privatePaused_||receiving_||active_||!c.valid())return false;
    const auto oldInterval=config.node().interval();
    endpoint_.positionConfig(c);config=c;codec.clearKey();signature.clear();route.clearRoutingState();clearTraceAssociations();
    auto record=c.node();discovery.configure(record);
    if(record.interval()!=oldInterval)nodeDiscovery.reschedule(record.interval());
    if(record.isPublic())codec.setKey(record.bytes[4],record.bytes+bench::MtNodeRecord::Key,16);
    return true;
  }
  // Fixed position is local companion metadata. Publishing a validated
  // position-only Config must not reset unrelated crypto, duplicate/routing,
  // discovery, endpoint or private-session state. SettingsEdit proves the
  // whole transaction differs from its durable base only in bytes 84-96.
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
    if(privatePaused_||fault_||receiving_||!nodes.ready()||!config.provisioned()||!route.identity()||!radio_.ready()||(relay&&(!txAllowed_||normalMode_)))return false;
    if(radio_.start()!=0||radio_.health()!=1){fail();return false;}
    receiving_=true;relaying_=relay;quiet_=now+250;return true;
  }
  void stop() {
    const int ret=radio_.stop();stoppedState();if(ret)fault_=true;
  }
  bool privatePeerMetadata(uint32_t from,const bench::RouteMetadata &m)const {
    return !private_||m.portnum!=4||private_->acceptsPeerKey(from,m.publicKey,m.publicKeyLength);
  }
  bool privateConfigured()const{return private_!=nullptr;}
  // Trusted boot composition only, before publishing any client. A provider
  // cannot be replaced, attached to a running radio, or change the local ID.
  bool bindPrivateRuntime(pki::PrivateRuntime &runtime) {
    if(private_||!normalMode_||!storageQuiescent()||!radio_.ready()||radio_.busy()||
       radio_.health()!=0||!nodes.ready()||nodes.ignored(runtime.peer())||
       !runtime.ready(route.identity()))return false;
    private_=&runtime;return true;
  }
  // A configured private startup failure cannot silently regain Public RF.
  // There is no companion/admin operation that clears this terminal latch.
  void holdPrivateStartup(){fail();}
  bool privatePublicIdentity(uint8_t out[32])const{return privateReady()&&private_->publicIdentity(out);}
  // Actual native RF-off proof for the injected ledger's storage guard. Queue
  // and arm are intentionally retained; all entry points reject reentry.
  bool privateStoragePaused() {return privatePaused_&&!fault_&&!receiving_&&!active_&&!own_&&!normalActive_&&radio_.ready()&&radio_.health()==0;}
  bool takePrivateEvent(uint8_t owner,const uint8_t *p,size_t n,pki::PrivateEvent &out) {
    return receiving_&&!privatePaused_&&!fault_&&privateReady()&&private_->event(owner,p,n,out);
  }
  bool revokePrivatePeer(uint32_t peer) {
    if(!private_||!private_->matchesPeer(peer))return true;
    if(!pausePrivate())return false;
    const bool persisted=private_->revoke(peer);
    // Revocation is terminal for this one-peer coordinator. Remain RF off and
    // disarmed. A subsequent cache edit failure cannot restore trust.
    stop();if(!persisted)fault_=true;return persisted;
  }
  bool receiving() const {return receiving_;}
  bool relaying() const {return relaying_;}
  bool faulted() const {return fault_;}
  bool active() const {return active_;}
  bool txAllowed() const {return txAllowed_;}
  bool normalMode()const {return normalMode_;}
  // Pre-stop admission for settings/storage operations. Healthy RX and the
  // durable endpoint arm may remain live, but no accepted user, control,
  // discovery, routing or native work may be discarded to obtain quiescence.
  // Callers still prove native ready/not-busy separately before stopping RX.
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
    if(!storageStopSafe()||radio_.busy())return -EBUSY;
    if(!receiving_)
      return radio_.ready()&&!radio_.busy()&&radio_.health()==0?0:-EIO;
    const int ret=radio_.stopForStorage();
    if(ret==-EAGAIN)return -EBUSY;
    if(ret){
      if(ret==-ESHUTDOWN)stoppedState();
      fault_=true;return ret;
    }
    if(radio_.health()!=0){fault_=true;return -EIO;}
    stoppedState();return 0;
  }
  // State-only eligibility: callers must additionally prove the native radio
  // is ready and fully off (RX health is not storage quiescence).
  bool storageQuiescent()const {
    return !receiving_&&!relaying_&&!endpointArmed_&&storageStopSafe();
  }
  unsigned endpointFree()const {return normalMode_?endpoint_.free():unsigned(!endpointQueued_&&!endpointActive_);}
  bool takeMessageResult(uint8_t owner,Endpoint::Result &r){return endpoint_.take(owner,r);}
  void disconnect(uint8_t owner){if(normalMode_)endpoint_.disconnect(owner);}
  void rx(const uint8_t *p,size_t n,int8_t snr,uint32_t now,uint32_t random) {
    if(private_)private_->clearEvent();
    if(!receiving_||fault_||privatePaused_)return;
    if(!p&&!n){if(radio_.health()==1)++rejected;else fail();return;}
    if(!p||n<17||n>255){++rejected;return;}
    // Local ignored-node policy precedes ACK/retry completion, discovery,
    // cache admission and routing. Header IDs are not authenticated identities.
    if(nodes.ignored(bench::le32(p+4))){++rejected;return;}
    ++received;
    if(normalMode_) {
      const auto from=bench::le32(p+4),to=bench::le32(p);const unsigned hop=p[12]&7,start=(p[12]>>5)&7;
      const bool endpointPacket=to==0xffffffffu||to==route.identity()||from==route.identity();
      if(from<=1||from>=0xfffffffeu||to<=1||!bench::le32(p+8)||hop>start)return;
      // Own-source echoes must reach the endpoint even for unicast control
      // responses. Endpoint requires exact ID/destination/ciphertext and lower
      // hops; an ordinary direct-message echo is never recipient delivery.
      if(p[13]==0&&to==route.identity()) {
        if(!privateReady()||!pausePrivate())return;
        endpoint_.blocked(radio_.airtime(n));
        private_->receive(endpoint_,codec,p,n,now,random,endpointArmed_&&txAllowed_);
        resumePrivate();return;
      }
      bench::RouteMetadata m;
      if(!codec.decode(p,n,m)||!m.channelDecoded||(!start&&!m.hasBitfield))return;
      if(!endpointPacket) {
        // The normal-profile transit extension is trace-only and stateless
        // until its own canonical admission. Unrelated signed transit must not
        // touch the public signature authority cache.
        if(private_||m.portnum!=meshtastic_PortNum_TRACEROUTE_APP||m.signatureLength) {
          bench::wipeKeyBuffer(&m,sizeof(m));return;
        }
        receiveTraceTransit(p,n,m,snr,now,random);
        bench::wipeKeyBuffer(&m,sizeof(m));return;
      }
      if(!privatePeerMetadata(from,m))return;
      signature.apply(p,n,m,now);
      if(m.signatureRejected||m.signatureQuarantined)return;
      endpoint_.blocked(radio_.airtime(n));
      if(from!=route.identity()&&m.portnum==4&&m.nodeInfoValid&&
          NodeUser::permits(nodes,route.identity(),from,m.payload,m.payloadLength,start-hop)<0) {
        bench::wipeKeyBuffer(&m,sizeof(m));return;
      }
      if(normalDiscovery_&&endpointArmed_&&txAllowed_&&from!=route.identity()&&Discovery::request(m)) {
        if(private_) {
          if(privateReady()&&nodeDiscovery.replyAllowed(from,now)&&!bootstrapRequest_.ready)
            bootstrapRequest_={from,bench::le32(p+8),now+5000,uint8_t(Discovery::responseHop(p)),(p[12]&8)!=0,true};
          else ++nodeDiscovery.limited;
        } else if(nodeDiscovery.replyAllowed(from,now)&&!queueNodeInfo(from,false,bench::le32(p+8),Discovery::responseHop(p),(p[12]&8)!=0,now,random))
          nodeDiscovery.replied(from,now);
        else ++nodeDiscovery.limited;
      }
      if(privateReady())private_->proof(endpoint_,p,n,m,now);
      // Sample current accounting at this receive/response boundary; uptime
      // availability remains a separate observation supplied by main.
      endpoint_.measuredAirUtilTx(airtime_.percent());
      endpoint_.receive(codec,route.identity(),p,n,m,snr,now,random,endpointArmed_&&txAllowed_);
      bench::wipeKeyBuffer(&m,sizeof(m));return;
    }
    if(!relaying_)return;
    bench::RouteMetadata metadata;codec.decode(p,n,metadata);
    if(metadata.channelDecoded)signature.apply(p,n,metadata,now);
    route.setRxSnr(snr);
    route.ingestWithMetadata(bench::Protocol::Meshtastic,p,n,metadata,now,random);
    bench::wipeKeyBuffer(&metadata,sizeof(metadata));
  }
  bool endpointArmed() const {return endpointArmed_;}
  uint32_t endpointCount() const {return endpointCount_;}
  bool armEndpoint(bool enable) {
    if(privatePaused_)return false;
    if(!enable){
      endpointArmed_=false;nodeDiscovery.cancel();bootstrapRequest_={};
      if(normalMode_){
        if(active_){if(radio_.stop()!=0||radio_.start()!=0||radio_.health()!=1){fail();return false;}active_=false;normalActive_=nullptr;normalTransitActive_=false;airtime_.noteUncertain();}
        endpoint_.cancel();route.clearPending();
      }
      return true;
    }
    if(normalMode_){if(!txAllowed_||!receiving_||fault_)return false;endpointArmed_=true;return true;}
    if(!txAllowed_||!receiving_||fault_||endpointCount_>=endpointLimit_)return false;
    endpointArmed_=true;return true;
  }
  int submitEndpoint(uint8_t owner,uint32_t id,const uint8_t *p,size_t n,uint32_t now) {
    if(!endpointArmed_||!txAllowed_||!receiving_||fault_)return -EACCES;
    if(endpointCount_>=endpointLimit_)return -EACCES;
    if(active_||endpointQueued_||resultId_)return -EBUSY;
    if(!id||!p||n<17||n>255||p[12]!=0||bench::le32(p+4)!=route.identity()||bench::le32(p+8)!=id)return -EINVAL;
    endpointFrame_={};memcpy(endpointFrame_.bytes,p,n);endpointFrame_.len=n;endpointFrame_.expires=now+5000;
    endpointOwner_=owner;endpointId_=id;endpointQueued_=true;++endpointCount_;
    endpointArmed_=false; // One request consumes the approval; no setting resets the quota.
    return 0;
  }
  int submitPrivate(uint8_t owner,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
    unsigned hop,bool reliable,const uint8_t *key,size_t keySize,bool explicitPki,uint32_t now) {
    if(!normalMode_||!privateReady())return -EPERM;
    if(!endpointArmed_||!txAllowed_||!receiving_||fault_||privatePaused_)return -EACCES;
    if(nodes.ignored(to))return -EPERM;
    if(!pausePrivate())return -EBUSY;
    const int ret=private_->submit(endpoint_,owner,to,id,data,n,hop,reliable,key,keySize,explicitPki,now);
    return resumePrivate()?ret:-EPERM;
  }
  int submitMessage(uint8_t owner,const uint8_t *p,size_t n,bool reliable,uint32_t now) {
    if(!normalMode_||!endpointArmed_||!txAllowed_||!receiving_||fault_||privatePaused_)return -EACCES;
    if(!p||n<17||n>255||bench::le32(p+4)!=route.identity())return -EINVAL;
    return endpoint_.submit(owner,p,n,reliable,now);
  }
  int submitTraceRoute(uint8_t owner,const uint8_t *p,size_t n,uint32_t now) {
    if(!normalMode_||!endpointArmed_||!txAllowed_||!receiving_||fault_||privatePaused_)return -EACCES;
    if(!p||n<17||n>255||bench::le32(p+4)!=route.identity())return -EINVAL;
    return endpoint_.submit(owner,p,n,false,now,false,true,true,nullptr,false,true);
  }
  bool takeTraceRoute(uint8_t owner,uint32_t from,uint32_t to,uint32_t packet,
                      const bench::RouteMetadata &m,uint32_t now,uint8_t *payload,size_t capacity,size_t &size) {
    size=0;return normalMode_&&to==route.identity()&&from!=route.identity()&&
      TraceRoute::responseShape(m)&&endpoint_.takeTraceRoute(owner,from,packet,m.request_id,now,payload,capacity,size);
  }
  int submitPositionQuery(uint8_t owner,const uint8_t *p,size_t n,uint32_t now) {
    if(!normalMode_||!endpointArmed_||!txAllowed_||!receiving_||fault_||privatePaused_)return -EACCES;
    if(!p||n<17||n>255||bench::le32(p+4)!=route.identity()||nodes.ignored(bench::le32(p)))return -EINVAL;
    return endpoint_.submit(owner,p,n,false,now,false,true,true,nullptr,false,false,true);
  }
  bool takePositionQuery(uint8_t owner,uint32_t from,uint32_t to,uint32_t packet,
                         const bench::RouteMetadata &m,uint32_t now,uint8_t *payload,size_t capacity,size_t &size) {
    size=0;return normalMode_&&to==route.identity()&&from!=route.identity()&&PositionQuery::responseShape(m)&&
      endpoint_.takeTraceRoute(owner,from,packet,m.request_id,now,payload,capacity,size,true);
  }
  void measuredUptime(uint64_t seconds){endpoint_.measuredUptime(seconds);}
  zt::Observed<float> airUtilTx(){return airtime_.percent();}
  AirtimeMeter &airtimeMeter(){return airtime_;}
  int submitTelemetryQuery(uint8_t owner,const uint8_t *p,size_t n,uint32_t now) {
    if(!normalMode_||!endpointArmed_||!txAllowed_||!receiving_||fault_||privatePaused_)return -EACCES;
    if(!p||n<17||n>255||bench::le32(p+4)!=route.identity()||nodes.ignored(bench::le32(p)))return -EINVAL;
    return endpoint_.submit(owner,p,n,false,now,false,true,true,nullptr,false,false,false,true);
  }
  bool takeTelemetryQuery(uint8_t owner,uint32_t from,uint32_t to,uint32_t packet,
                         const bench::RouteMetadata &m,uint32_t now,uint8_t *payload,size_t capacity,size_t &size) {
    size=0;return normalMode_&&to==route.identity()&&from!=route.identity()&&TelemetryQuery::responseShape(m)&&
      endpoint_.takeTraceRoute(owner,from,packet,m.request_id,now,payload,capacity,size,false,true);
  }
  bool matchedNodeInfo(uint32_t from,uint32_t to,const bench::RouteMetadata &m,uint32_t now)const {
    return normalMode_&&to==route.identity()&&from!=route.identity()&&
      Endpoint::nodeInfoResponse(m)&&NodeUser::accepted(nodes,from,m.payload,m.payloadLength)&&
      endpoint_.matchedNodeInfo(from,m.request_id,now);
  }
  int submitNodeInfo(uint8_t owner,uint32_t id,uint32_t to,unsigned hop,bool reliable,bool wantResponse,uint32_t now) {
    if(!normalMode_||!normalDiscovery_||!endpointArmed_||!txAllowed_||!receiving_||fault_)return -EACCES;
    if(hop>3||to==route.identity())return -EINVAL;
    if(private_) {
      if(!privateReady())return -EPERM;
      const int repeat=endpoint_.repeatSignedNodeInfo(owner,id,to,hop,reliable,wantResponse,now);
      if(repeat!=-ENOENT)return repeat;
      if(!nodeDiscovery.allowed(now)||endpoint_.nodeInfoPending()||!endpoint_.free())return -EAGAIN;
    }
    bench::Frame frame;
    if(!encodeNodeInfo(to,id,wantResponse,0,frame))return -EINVAL;
    frame.bytes[12]=uint8_t(hop|(hop<<5)|((reliable&&to!=0xffffffffu)?8:0));
    const bool permit=nodeDiscovery.allowed(now)&&!endpoint_.nodeInfoPending();
    bool created=false;
    const int ret=endpoint_.submit(owner,frame.bytes,frame.len,reliable,now,true,permit,wantResponse,&created,private_!=nullptr);
    if(!ret&&created)nodeDiscovery.admitted(now,config.node().interval());
    return ret;
  }
  bool takeEndpointResult(uint8_t owner,uint32_t &id,int &status) {
    if(!resultId_||owner!=resultOwner_)return false;
    id=resultId_;status=resultStatus_;resultId_=0;return true;
  }
  bool announce(uint32_t now,uint32_t random) {return discovery.request(now,random,relaying_&&!fault_);}
  void poll(uint32_t now,uint32_t random) {
    airtime_.ensureAnchored();
    if(!receiving_||fault_)return;
    if(normalMode_){pollNormal(now,random);return;}
    if(active_) {
      int result=0;
      if(!radio_.completion(result)) {if(bench::due(now,deadline_)){++failed;airtime_.noteUncertain();if(endpointActive_)endpointResult(-ETIMEDOUT);fail();}return;}
      active_=false;quiet_=now+250;
      // Known terminal completion: a clean result is credited exactly once
      // before the RX-restore check, so a known duration survives a failed
      // restore. An error status is an uncertain outcome (possible early
      // abort), tracked and never credited.
      if(result){airtime_.noteUncertain();++failed;if(endpointActive_)endpointResult(result);fail();return;}
      airtime_.creditPending();
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
      if(!air||air>2500){endpointResult(-EMSGSIZE);return;}
      if(uint32_t(now-window_)>=600000){window_=now;budget_=0;}
      if(budget_+air>6000){endpointResult(-EAGAIN);return;}
      budget_+=air;activeFrame_=endpointFrame_;endpointQueued_=false;endpointActive_=true;
      const int ret=radio_.send(activeFrame_.bytes,activeFrame_.len);
      if(ret){++failed;endpointResult(ret);if(radio_.restore()!=0)fail();return;} // no retries
      active_=true;own_=false;deadline_=now+air+1000;airtime_.noteAcceptedStart(air);return;
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
    if(ret==0){active_=true;deadline_=now+air+1000;route.noteBlockedAirtime(air+250);airtime_.noteAcceptedStart(air);return;}
    if(ret==-EBUSY) {
      if(radio_.restore()!=0||radio_.health()!=1){fail();return;}
      if(own_)discovery.retry(activeFrame_,now);else route.retry(activeFrame_,now);
      own_=false;quiet_=now+250;return;
    }
    ++failed;fail();
  }
};
}
