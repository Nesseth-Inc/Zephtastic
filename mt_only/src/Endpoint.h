// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "LocalClients.h"
#include "RoutingCore.h"
#include "TraceRoute.h"
#include "PositionQuery.h"
#include "stock/modules/PositionModule.h"
#include "TelemetryQuery.h"
#include "LocalStatsQuery.h"
#include "RemoteTelemetryQuery.h"
#include "EnvironmentQuery.h"
#include "PowerQuery.h"
#include "PublicApplication.h"
#include "RadioTiming.h"
#include "PacketLifetime.h"
#include "NeighborQuery.h"
#include "NeighborReplyWindow.h"
#if defined(CONFIG_MT_PAXCOUNTER)
#include "modules/PaxQuery.h"
#endif
#if defined(CONFIG_MT_TAK_TRACKER)
#include "modules/TakPliReplyValidation.h"
#endif
#include <errno.h>

namespace mt {
// Fixed-memory Public-channel endpoint. No RF side effects: Service owns the
// one physical radio, gate, airtime budget, native completion and fault policy.
class Endpoint {
public:
  // Radio-local input uses the normal job/history machinery without routing
  // its completion into a connected USB, BLE or TCP client's result queue.
  static constexpr uint8_t NoClientOwner=0xff;
  static constexpr uint8_t LocalUiOwner=0xfe;
  // Borrowed immutable owner hook. Standalone/default-Public fixtures retain
  // their direct codec path; an installed owner never falls back on failure.
  struct ControlEncoder {
    virtual unsigned channelIndex()const{return 0;}
    virtual bool positionSharingAllowed()const{return true;}
    virtual bool neighborSnapshot(meshtastic_NeighborInfo &out) {
      out=meshtastic_NeighborInfo_init_zero;return false;
    }
#if defined(CONFIG_MT_RF_ADMIN_GET)
    // Synchronous, borrowed inputs are supplied only by PkiRuntime after
    // authentication and durable Fresh replay commit. The installed owner
    // must return canonical bounded Admin Data; the runtime validates it.
    virtual uint32_t privateAdminResponseOwners(uint32_t,uint32_t,const uint8_t[32],uint32_t,
        const uint8_t *,size_t,uint32_t,bool){return 0;}
    // Only PkiRuntime's verified, nonzero Routing proof may terminate an
    // outstanding Admin query. A positive proof still waits for application data.
    virtual void privateAdminFailure(uint32_t,const uint8_t[32],uint32_t){}
    virtual bool privateAdminReplyData(uint32_t,uint32_t,const uint8_t[32],uint32_t,
        const uint8_t *,size_t,uint8_t *,size_t,size_t &size)const {
      size=0;return false;
    }
    // Called only by the authenticated private runtime after a Fresh replay
    // ID has been durably committed. The owner may stage one bounded request;
    // it must not apply settings or emit an RF response from this RX hook.
    virtual bool privateAdminMutationRequest(uint32_t,uint32_t,const uint8_t[32],uint32_t,
        uint8_t,bool,const uint8_t *,size_t,uint32_t){return false;}
#endif
    virtual bool sameChannel(unsigned a,unsigned b)const{return a==b;}
    // Construct only bounded Data plaintext. Job IDs, transport policy and
    // any channel encryption remain in Endpoint/Service. Optional roles must
    // fail closed unless the installed owner has an explicit encoder.
    virtual bool positionReplyData(const Config &config,const meshtastic_Position &live,
                                   unsigned channel,uint32_t request,uint8_t *out,
                                   size_t capacity,size_t &size)const {
      size=0;
      if(config.role()==10)return false;
      return PositionQuery::fixedResponse(config,request,out,capacity,size,channel,&live);
    }
    virtual bool encodeChannelControl(uint32_t,uint32_t,const uint8_t *,size_t,bench::Frame &)=0;
    virtual bool encodePublicApplicationAck(uint32_t to,uint32_t id,const uint8_t *data,size_t n,bench::Frame &out) {
      return encodeChannelControl(to,id,data,n,out);
    }
    virtual bool licensedPlaintext()const{return false;}
    virtual uint32_t localNode()const{return 0;}
    // Route provenance is emitted only by Endpoint after the owner has matched
    // a private response or verified Routing proof to an immutable job frame.
    virtual void observeAuthenticatedOriginResponse(const bench::Frame &,
        const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t,uint32_t){}
    virtual void observeOpaqueOriginRelay(const bench::Frame &,const uint8_t *,size_t,uint32_t){}
  };
  static bool positionResponsePort(uint32_t port) {
    if(port==meshtastic_PortNum_POSITION_APP)return true;
#if defined(CONFIG_MT_TAK_TRACKER)
    if(port==meshtastic_PortNum_ATAK_PLUGIN_V2)return true;
#endif
    return false;
  }
  static bool positionResponseShape(const bench::RouteMetadata &m) {
    if(PositionQuery::responseShape(m))return true;
#if defined(CONFIG_MT_TAK_TRACKER)
    return tak_pli_reply::responseShape(m);
#else
    return false;
#endif
  }
  static bool positionResponseData(const bench::RouteMetadata &m,uint8_t *out,size_t capacity,size_t &size) {
    if(m.portnum==meshtastic_PortNum_POSITION_APP)return PositionQuery::response(m,out,capacity,size);
#if defined(CONFIG_MT_TAK_TRACKER)
    return tak_pli_reply::response(m,out,capacity,size);
#else
    size=0;return false;
#endif
  }
  enum class Kind:uint8_t {Local,Peer,Relay,Response,Authenticated,Failed,PrivateResponse,MqttEcho};
  struct Result {uint32_t id=0,peer=0;Kind kind=Kind::Failed;uint8_t code=0,attempts=0;};
  enum class State:uint8_t {Free,Queued,Active,Waiting};
  using LifetimeAirtimeProvider=uint32_t (*)(size_t,void *);
  struct Job {
    bench::Frame frame;
    State state=State::Free;
    uint8_t owners=0,attempts=0,telemetryVariant=2,channelSlot=0;
    bool rolePriority=false,reliable=false,control=false,moduleMessage=false,transmitted=false,terminal=false,nodeInfo=false,wantResponse=false,privateMessage=false,traceroute=false,positionQuery=false,telemetryQuery=false,positionResponse=false,signedPublic=false,licensedUnicast=false,proofSeen=false,localStatsQuery=false,neighborQuery=false,localInput=false,localUiInput=false;
#if defined(CONFIG_ZEPHCORE_AUDIO_CODEC2)
    bool realtimeAudio=false;
#endif
    uint32_t born=0,due=0,queuedAt=0,air=0,request=0;
    Result result;
    uint32_t queueLifetimeMs=10000,pendingLifetimeMs=90000;
    bool lifetimeCaptured=false;
    uint8_t requestedOptions=UINT8_MAX; // Original phone hop and want-response before broadcast policy.
  };
private:
  ControlEncoder *const controlEncoder_;
  NeighborReplyWindow neighborReplyWindow_;
#if defined(CONFIG_MT_PAXCOUNTER)
  bool (*paxSnapshot_)(void *, meshtastic_Paxcount &) = nullptr;
  void *paxContext_ = nullptr;
  struct PaxRequest { uint32_t from=0,id=0,at=0; } paxReplies_[8]{};
  PaxRequest *paxSlot(uint32_t from,uint32_t id,uint32_t now) {
    PaxRequest *free=nullptr;
    for (auto &r:paxReplies_) {
      const uint32_t age=now-r.at;
      if (r.from==from && (age<5000 || (r.id==id && age<600000))) return nullptr;
      if (!free && (!r.id || age>=600000)) free=&r;
    }
    return free;
  }
#endif
  uint32_t lastPositionReplyAt_=0;
  bool positionReplySeen_=false;
  unsigned channelIndex()const{return controlEncoder_?controlEncoder_->channelIndex():0;}
  bool onChannel(const Job &job)const{if(job.privateMessage&&job.frame.bytes[13]==0)return true;return controlEncoder_?controlEncoder_->sameChannel(job.channelSlot,channelIndex()):job.channelSlot==channelIndex();}
  bool encodeControl(bench::MtCodec &codec,uint32_t ours,uint32_t to,uint32_t id,
                     const uint8_t *data,size_t size,bench::Frame &frame) {
    if(controlEncoder_)return controlEncoder_->encodeChannelControl(to,id,data,size,frame);
    size_t n=0;
    if(!codec.encodeZeroHopData(ours,to,id,data,size,frame.bytes,sizeof(frame.bytes),n))return false;
    frame.len=n;return true;
  }
  bool annotatePublicControl(Job &job,uint32_t to)const {
    if(job.frame.len<17||job.frame.len>255)return false;
    // Shared-channel frames contain ciphertext. Their owner already supplies
    // the signature flag after codec decoding; Endpoint has no channel key.
    // Only the explicit plaintext path can inspect Data directly here.
    if(!controlEncoder_||!controlEncoder_->licensedPlaintext())return true;
    bench::RouteMetadata metadata{};
    const bool valid=bench::MtCodec::decodeData(job.frame.bytes+16,job.frame.len-16,metadata)&&
       (!metadata.signatureLength||metadata.signatureLength==64);
    if(!valid){bench::wipeKeyBuffer(&metadata,sizeof(metadata));return false;}
    job.signedPublic=metadata.signatureLength==64;
    job.licensedUnicast=controlEncoder_&&controlEncoder_->licensedPlaintext()&&
      bench::le32(job.frame.bytes+4)==controlEncoder_->localNode()&&to!=0xffffffffu;
    bench::wipeKeyBuffer(&metadata,sizeof(metadata));return true;
  }
  bool positionReplyData(uint32_t request,uint8_t *out,size_t capacity,size_t &size,
                         unsigned channel)const {
    size=0;
    if(controlEncoder_)
      return controlEncoder_->positionReplyData(sharedPosition_,livePosition_,channel,
                                                 request,out,capacity,size);
    if(sharedPosition_.role()==10)return false;
    return PositionQuery::fixedResponse(sharedPosition_,request,out,capacity,size,
                                        channel,&livePosition_);
  }
  Job jobs_[4]{},acks_[4]{};
  LifetimeAirtimeProvider lifetimeAirtime_=nullptr;
  void *lifetimeContext_=nullptr;
  bool captureLifetime(Job &candidate) const {
        if(candidate.lifetimeCaptured) return true;
        const size_t wire = candidate.frame.len;
        if(wire < 17 || wire > 255) return false;
        if(!lifetimeAirtime_) {
          candidate.queueLifetimeMs = 10000;
          candidate.pendingLifetimeMs = 90000;
          candidate.lifetimeCaptured = true;
          return true;
        }
        const uint32_t air = lifetimeAirtime_(wire, lifetimeContext_);
        mt::RadioTiming::Deadline timing;
        if(!mt::RadioTiming::resolve(air, timing)) return false;
        const uint64_t rto = 16000u + 2u * uint64_t(air);
        if(rto >= 0x100000000ULL) return false;
        mt::PacketLifetime::Bounds bounds;
        if(!mt::PacketLifetime::resolve(timing.hostMs, uint32_t(rto), bounds)) return false;
        candidate.queueLifetimeMs = bounds.queueMs;
        candidate.pendingLifetimeMs = bounds.pendingMs;
        candidate.lifetimeCaptured = true;
        return true;
      }

  struct History {Job job;bool used=false;};History history_[8]{};unsigned historyAt_=0;
  const Job *originJob(uint32_t id)const {
    for(const auto &job:jobs_)if(job.state!=State::Free&&bench::le32(job.frame.bytes+8)==id)return &job;
    for(const auto &item:history_)if(item.used&&item.job.result.id==id)return &item.job;
    return nullptr;
  }
  // Never cleared by config/disarm. False positives reject a new ID safely.
  // Prevents reusing evicted IDs with a different CTR plaintext during a boot.
  uint8_t nonceBits_[1024]{};
  struct Replies {uint32_t from=0,id=0,at=0,last=0;uint8_t count=0;};Replies replies_[16]{};
  bool uptimeAvailable_=false;uint32_t uptimeSeconds_=0,telemetryUtc_=0;zt::Observed<float> channelUtilization_;
  zt::Observed<float> airUtilTx_;
  HardwareSnapshot hardware_;
  zt::telemetry::EnvironmentReadings environment_;
  zt::telemetry::PowerReadings power_;
  const void *statsOwner_=nullptr;
  LocalStatsObservation (*statsRead_)(const void *)=nullptr;
  bool localStatsResponse(uint32_t request,uint8_t *data,size_t capacity,size_t &size)const{
    const auto observed=statsRead_?statsRead_(statsOwner_):LocalStatsObservation{};
    return LocalStatsQuery::uptimeResponse(uptimeSeconds_,request,airUtilTx_,channelUtilization_,
                                          data,capacity,size,&observed,telemetryUtc_);
  }
  void *extraTelemetryContext_=nullptr;
  bool (*extraTelemetry_)(void*,uint8_t,uint32_t,uint8_t*,size_t,size_t&)=nullptr;
  struct TelemetryReply {uint32_t from=0,id=0,at=0;};TelemetryReply telemetryReplies_[16]{},positionReplies_[16]{};
  Config sharedPosition_;
  meshtastic_Position livePosition_{};
  struct TraceReply {uint32_t from=0,id=0,at=0;};TraceReply traceReplies_[16]{};
  struct TraceApproval {
    uint32_t from=0,packet=0,request=0,at=0;uint8_t owners=0,delivered=0,size=0,channelSlot=0,positionPort=0,payload[TraceRoute::MaxPayload]{};bool positionQuery=false,telemetryQuery=false,privateResponse=false,neighborQuery=false;
  };TraceApproval traceApprovals_[4]{};unsigned traceApprovalAt_=0;
  struct Results {Result items[8]{};unsigned read=0,count=0;};Results results_[LocalClientCount]{};
  struct LocalResults {Result items[4]{};unsigned read=0,count=0;} localResults_;
  Result localUiResult_{};bool localUiResultReady_=false;
  static uint32_t mix(uint32_t v) {v^=v>>16;v*=0x7feb352d;v^=v>>15;v*=0x846ca68b;return v^(v>>16);}
  bool nonce(uint32_t id,bool reserve) {
    bool seen=true;
    for(unsigned i=0;i<3;++i){const auto bit=mix(id+i*0x9e3779b9u)&8191;seen&=(nonceBits_[bit/8]&(1u<<(bit%8)))!=0;if(reserve)nonceBits_[bit/8]|=1u<<(bit%8);}
    return seen;
  }
  bool same(const Job &j,const uint8_t *p,size_t n,bool reliable) const {
    return onChannel(j)&&j.frame.len==n&&j.reliable==reliable&&!memcmp(j.frame.bytes,p,n);
  }
  static bool sameLicensedSource(const Job &job,const uint8_t *source,size_t sourceSize) {
    if(!source||!sourceSize||job.frame.len<17||job.frame.len>255)return false;
    struct ScratchWipe {void *data;size_t size;~ScratchWipe(){bench::wipeKeyBuffer(data,size);}};
    meshtastic_Data stored{};auto input=pb_istream_from_buffer(job.frame.bytes+16,job.frame.len-16);
    ScratchWipe wipeStored{&stored,sizeof(stored)};
    if(!pb_decode(&input,meshtastic_Data_fields,&stored)||input.bytes_left||
       (job.signedPublic?stored.xeddsa_signature.size!=64:stored.xeddsa_signature.size!=0))return false;
    uint8_t signedWire[239]{};ScratchWipe wipeSigned{signedWire,sizeof(signedWire)};auto signedOutput=pb_ostream_from_buffer(signedWire,sizeof(signedWire));
    if(!pb_encode(&signedOutput,meshtastic_Data_fields,&stored)||signedOutput.bytes_written!=job.frame.len-16||
       memcmp(signedWire,job.frame.bytes+16,job.frame.len-16))return false;
    stored.xeddsa_signature.size=0;memset(stored.xeddsa_signature.bytes,0,sizeof(stored.xeddsa_signature.bytes));
    uint8_t unsignedWire[239]{};ScratchWipe wipeUnsigned{unsignedWire,sizeof(unsignedWire)};auto unsignedOutput=pb_ostream_from_buffer(unsignedWire,sizeof(unsignedWire));
    const bool exact=pb_encode(&unsignedOutput,meshtastic_Data_fields,&stored)&&
      unsignedOutput.bytes_written==sourceSize&&!memcmp(unsignedWire,source,sourceSize);
    return exact;
  }
  void emit(uint8_t owners,const Result &r) {
    for(unsigned i=0;i<LocalClientCount;++i)if(owners&(1u<<i)) {
      auto &q=results_[i];if(q.count==8){q.read=(q.read+1)%8;--q.count;}
      q.items[(q.read+q.count++)%8]=r;
    }
  }
  void finish(Job &j,Kind kind,uint8_t code,uint32_t peer=0) {
    j.result={bench::le32(j.frame.bytes+8),(kind==Kind::Peer||kind==Kind::Response||kind==Kind::Authenticated||kind==Kind::PrivateResponse)?(peer?peer:bench::le32(j.frame.bytes)):0,kind,code,j.attempts};
    if(!j.control){auto &h=history_[historyAt_++%8];h.job=j;h.used=true;emit(j.owners,j.result);}
    if(j.localInput) {
      auto &q=localResults_;
      if(q.count==4){q.read=(q.read+1)%4;--q.count;}
      q.items[(q.read+q.count++)%4]=j.result;
    }
    if(j.localUiInput) {
      // Admission permits only one UI job/result at a time. Preserve an unread
      // result defensively if a caller ever violates that invariant.
      if(!localUiResultReady_) {
        localUiResult_=j.result;
        localUiResultReady_=true;
      }
    }
    j={};
  }
  void outcome(Job &j,Kind kind,uint8_t code,uint32_t peer=0) {
    if(j.terminal)return;
    // Keep the slot alive until the native send completes, even on an early ACK.
    if(j.state==State::Active){j.terminal=true;j.result.kind=kind;j.result.code=code;j.result.peer=peer;}
    else finish(j,kind,code,peer);
  }
  static uint8_t retryLimit(const Job &j) {
    return j.reliable&&bench::le32(j.frame.bytes)!=0xffffffffu?5u:3u;
  }
  void retry(Job &j,uint32_t now,uint32_t random) {
    if(j.attempts>=retryLimit(j)){finish(j,Kind::Failed,5);return;}
    j.state=State::Queued;j.due=now+250+(random%751);j.queuedAt=now;
  }
  bool replyEligible(uint32_t from,uint32_t id,uint32_t now) {
    Replies *slot=nullptr;
    for(auto &r:replies_) {
      if(r.from==from&&r.id==id&&uint32_t(now-r.at)<600000) {
        if(r.count>=3||uint32_t(now-r.last)<2000)return false;
        ++r.count;r.last=now;return true;
      }
      if(!slot&&(!r.id||uint32_t(now-r.at)>=600000))slot=&r;
    }
    // Do not evict live rate-limit records under a sender flood.
    if(!slot)return false;
    *slot={from,id,now,now,1};return true;
  }
  bool traceReplyEligible(uint32_t from,uint32_t id,uint32_t now) {
    TraceReply *slot=nullptr;
    for(auto &r:traceReplies_) {
      if(r.from==from&&r.id==id&&uint32_t(now-r.at)<600000)return false;
      if(!slot&&(!r.id||uint32_t(now-r.at)>=600000))slot=&r;
    }
    if(!slot)return false;
    *slot={from,id,now};return true;
  }
  void reply(bench::MtCodec &codec,uint32_t ours,uint32_t from,uint32_t request,
             unsigned hop,uint32_t now,uint32_t random,bool reliable=false,bool application=false) {
    // An existing reliable response owns its ID, retries and airtime. Repeated
    // text must not allocate parallel ACKs while that response is pending.
    for(const auto &j:acks_)if(j.state!=State::Free&&j.reliable&&
        bench::le32(j.frame.bytes)==from&&j.request==request)return;
    Job *slot=nullptr;for(auto &j:acks_)if(j.state==State::Free){slot=&j;break;}
    if(!slot||!replyEligible(from,request,now))return;
    uint32_t id=random;unsigned tries=0;
    while((!id||nonce(id,false))&&++tries<16)id=mix(id+0x9e3779b9u);
    if(!id||nonce(id,false))return;
    // Routing(error_reason=NONE), request_id and explicit Data.bitfield=0.
    uint8_t data[13]={8,5,18,2,24,0,53,0,0,0,0,72,0};
    for(unsigned i=0;i<4;++i)data[7+i]=uint8_t(request>>(8*i));
    Job j;j.channelSlot=uint8_t(channelIndex());
    if(!(application&&controlEncoder_?controlEncoder_->encodePublicApplicationAck(from,id,data,sizeof(data),j.frame):
         encodeControl(codec,ours,from,id,data,sizeof(data),j.frame)))return;
    if(!annotatePublicControl(j,from))return;
    j.frame.bytes[12]=uint8_t(hop|(hop<<5)|(reliable?8:0));
    j.reliable=reliable;j.request=request;j.control=true;j.state=State::Queued;j.born=j.queuedAt=now;j.due=now+250;
    if(!captureLifetime(j))return;
    nonce(id,true);*slot=j;
  }
  void telemetryReply(bench::MtCodec &codec,uint32_t ours,uint32_t from,uint32_t request,
                      unsigned hop,unsigned start,uint32_t now,uint32_t random,bool ack,bool localStats=false,bool environment=false,bool power=false,uint8_t extra=0) {
    Job *slot=nullptr;for(auto &j:acks_)if(j.state==State::Free){slot=&j;break;}
    if(!slot||(!environment&&!power&&!extra&&!uptimeAvailable_))return;
    TelemetryReply *record=nullptr;
    for(auto &r:telemetryReplies_) {
      if(r.from==from&&((r.id==request&&uint32_t(now-r.at)<600000)||uint32_t(now-r.at)<5000))return;
      if(!record&&(!r.id||uint32_t(now-r.at)>=600000))record=&r;
    }
    if(!record)return; // Never evict a live replay/rate record under a sender flood.
    uint8_t data[128]{};size_t size=0;
    if(!(extra?(extraTelemetry_&&extraTelemetry_(extraTelemetryContext_,extra,request,data,sizeof(data),size)):power?PowerQuery::response(power_,request,data,sizeof(data),size,telemetryUtc_):environment?EnvironmentQuery::responseMetrics(zt::telemetry::toMeshtasticEnvironmentMetrics(environment_),request,data,sizeof(data),size,telemetryUtc_):localStats?localStatsResponse(request,data,sizeof(data),size):
        TelemetryQuery::uptimeResponse(uptimeSeconds_,request,airUtilTx_,channelUtilization_,data,sizeof(data),size,hardware_,telemetryUtc_)))return;
    uint32_t id=random;unsigned tries=0;
    while((!id||nonce(id,false))&&++tries<16)id=mix(id+0x9e3779b9u);
    if(!id||nonce(id,false))return;
    Job j;j.channelSlot=uint8_t(channelIndex());
    if(!encodeControl(codec,ours,from,id,data,size,j.frame))return;
    if(!annotatePublicControl(j,from))return;
    const unsigned responseHop=HopPolicy::response(start,hop,sharedPosition_.hopLimit());
    j.frame.bytes[12]=uint8_t(responseHop|(responseHop<<5)|(ack?8:0));
    j.request=request;j.reliable=ack;j.control=true;j.state=State::Queued;j.born=j.queuedAt=now;j.due=now+250;
    if(!captureLifetime(j))return;
    nonce(id,true);*record={from,request,now};*slot=j;
  }
  void neighborReply(bench::MtCodec &codec,uint32_t ours,uint32_t from,uint32_t request,
                     unsigned hop,unsigned start,uint32_t now,uint32_t random,bool ack) {
    Job *slot=nullptr;for(auto &j:acks_)if(j.state==State::Free){slot=&j;break;}
    uint8_t data[239]{};size_t size=0;
    if(!slot||!neighborReplyData(request,now,data,sizeof(data),size))return;
    if (enqueueSnapshotReply(*slot,codec,ours,from,request,hop,start,now,random,ack,data,size))
      neighborReplyAccepted(now);
  }
  bool enqueueSnapshotReply(Job &slot,bench::MtCodec &codec,uint32_t ours,uint32_t from,uint32_t request,
                            unsigned hop,unsigned start,uint32_t now,uint32_t random,bool ack,
                            const uint8_t *data,size_t size) {
    const auto id=generatedId(random);if(!id)return false;
    Job j;j.channelSlot=uint8_t(channelIndex());
    if(!encodeControl(codec,ours,from,id,data,size,j.frame))return false;
    if(!annotatePublicControl(j,from))return false;
    const auto responseHop=HopPolicy::response(start,hop,sharedPosition_.hopLimit());
    j.frame.bytes[12]=uint8_t(responseHop|(responseHop<<5)|(ack?8:0));
    j.request=request;j.reliable=ack;j.control=true;j.state=State::Queued;
    j.born=j.queuedAt=now;j.due=now+250;
    if(!captureLifetime(j))return false;
    nonce(id,true);slot=j;return true;
  }
  void positionReply(bench::MtCodec &codec,uint32_t ours,uint32_t from,uint32_t request,
                      unsigned hop,unsigned start,uint32_t now,uint32_t random,bool ack) {
    Job *slot=nullptr;for(auto &j:acks_)if(j.state==State::Free){slot=&j;break;}
    if(!slot||!sharedPosition_.positionSharing()||!positionReplyAllowed(now))return;
    TelemetryReply *record=nullptr;
    for(auto &r:positionReplies_) {
      if(r.from==from&&((r.id==request&&uint32_t(now-r.at)<600000)||uint32_t(now-r.at)<5000))return;
      if(!record&&(!r.id||uint32_t(now-r.at)>=600000))record=&r;
    }
    if(!record)return; // Never evict a live replay/rate record under a sender flood.
#if defined(CONFIG_MT_TAK_TRACKER)
    uint8_t data[239]{};
#else
    uint8_t data[64]{};
#endif
    size_t size=0;
    if(!positionReplyData(request,data,sizeof(data),size,channelIndex()))return;
    uint32_t id=random;unsigned tries=0;
    while((!id||nonce(id,false))&&++tries<16)id=mix(id+0x9e3779b9u);
    if(!id||nonce(id,false))return;
    Job j;j.channelSlot=uint8_t(channelIndex());
    if(!encodeControl(codec,ours,from,id,data,size,j.frame))return;
    if(!annotatePublicControl(j,from))return;
    const unsigned responseHop=HopPolicy::response(start,hop,sharedPosition_.hopLimit());
    j.frame.bytes[12]=uint8_t(responseHop|(responseHop<<5)|(ack?8:0));
    j.request=request;j.reliable=ack;j.control=j.positionResponse=true;j.state=State::Queued;j.born=j.queuedAt=now;j.due=now+250;
    if(!captureLifetime(j))return;
    nonce(id,true);*record={from,request,now};*slot=j;positionReplyAccepted(now);
  }
  void traceReply(bench::MtCodec &codec,uint32_t ours,uint32_t from,uint32_t request,
                  unsigned hop,unsigned start,int8_t snr,uint32_t now,uint32_t random,
                  const bench::RouteMetadata &metadata,bool ack) {
    Job *slot=nullptr;for(auto &j:acks_)if(j.state==State::Free){slot=&j;break;}
    // A schema-valid request has already used this source/packet-ID nonce.
    // Consume its replay slot before semantic response construction so a
    // changed payload cannot reuse that nonce after a rejected request.
    if(!slot||!traceReplyEligible(from,request,now))return;
    uint8_t payload[TraceRoute::MaxPayload]{},data[239]{};size_t payloadSize=0,dataSize=0;
    if(!TraceRoute::destination(metadata,hop,start,snr,payload,sizeof(payload),payloadSize)||
       !TraceRoute::responseData(payload,payloadSize,request,data,sizeof(data),dataSize))return;
    uint32_t id=random;unsigned tries=0;
    while((!id||nonce(id,false))&&++tries<16)id=mix(id+0x9e3779b9u);
    if(!id||nonce(id,false))return;
    Job j;j.channelSlot=uint8_t(channelIndex());
    if(!encodeControl(codec,ours,from,id,data,dataSize,j.frame))return;
    if(!annotatePublicControl(j,from))return;
    const unsigned responseHop=HopPolicy::response(start,hop,sharedPosition_.hopLimit());
    j.frame.bytes[12]=uint8_t(responseHop|(responseHop<<5)|(ack?8:0));
    j.reliable=ack;j.request=request;j.control=j.traceroute=true;j.state=State::Queued;j.born=j.queuedAt=now;j.due=now+250;
    if(!captureLifetime(j))return;
    nonce(id,true);*slot=j;
  }
public:
  explicit Endpoint(ControlEncoder *encoder=nullptr):controlEncoder_(encoder){}
  bool setLifetimeAirtimeProvider(LifetimeAirtimeProvider provider, void *context=nullptr) {
        if (provider == nullptr) return false;
        if (lifetimeAirtime_ != nullptr) return false;
        if (!idle()) return false;
        lifetimeAirtime_ = provider;
        lifetimeContext_ = context;
        return true;
      }
#if defined(CONFIG_MT_PAXCOUNTER)
  bool localPaxSnapshot(meshtastic_Paxcount &out){return paxSnapshot_&&paxSnapshot_(paxContext_,out);}
  bool bindPaxSnapshot(bool (*provider)(void *,meshtastic_Paxcount &),void *ctx) {
    if(!provider || paxSnapshot_)return false;
    paxSnapshot_=provider;paxContext_=ctx;return true;
  }
  bool paxReplyData(uint32_t from,uint32_t id,uint32_t now,uint8_t *out,size_t capacity,size_t &size) {
    size=0;meshtastic_Paxcount count{};
    return paxSnapshot_ && paxSlot(from,id,now) && paxSnapshot_(paxContext_,count) &&
           modules::pax::encodeReply(id,count,out,capacity,size);
  }
  void paxReplyAccepted(uint32_t from,uint32_t id,uint32_t now) {
    if(auto *slot=paxSlot(from,id,now))*slot={from,id,now};
  }
#endif
  bool neighborReplyAllowed(uint32_t now)const{return neighborReplyWindow_.allowed(now);}
  bool neighborReplyData(uint32_t request,uint32_t now,uint8_t *out,size_t capacity,size_t &size) {
    size=0;
    if(!request||!out||!controlEncoder_||!neighborReplyWindow_.allowed(now))return false;
    meshtastic_NeighborInfo info{};
    if(!controlEncoder_->neighborSnapshot(info))return false;
    return NeighborQuery::response(request,info,out,capacity,size);
  }
#if defined(CONFIG_MT_RF_ADMIN_GET)
  uint32_t privateAdminResponseOwners(uint32_t local,uint32_t from,const uint8_t key[32],
      uint32_t packetId,const uint8_t *data,size_t size,uint32_t now,bool consume) {
    return controlEncoder_?controlEncoder_->privateAdminResponseOwners(local,from,key,packetId,data,size,now,consume):0;
  }
  void privateAdminFailure(uint32_t from,const uint8_t key[32],uint32_t requestId) {
    if(controlEncoder_)controlEncoder_->privateAdminFailure(from,key,requestId);
  }
  bool privateAdminReplyData(uint32_t local,uint32_t from,const uint8_t peerKey[32],
      uint32_t requestId,const uint8_t *request,size_t requestSize,uint8_t *out,
      size_t capacity,size_t &size)const {
    size=0;
    if(!local||!from||!peerKey||!requestId||!request||!requestSize||requestSize>227||
       !out||!capacity||capacity>227||!controlEncoder_)return false;
    if(controlEncoder_->privateAdminReplyData(local,from,peerKey,requestId,request,
        requestSize,out,capacity,size)&&size&&size<=capacity)return true;
    if(out&&capacity)memset(out,0,capacity);
    size=0;return false;
  }
  bool privateAdminMutationRequest(uint32_t local,uint32_t from,const uint8_t peerKey[32],
      uint32_t request,uint8_t responseHop,bool reliable,const uint8_t *data,size_t size,uint32_t now) {
    return controlEncoder_&&controlEncoder_->privateAdminMutationRequest(local,from,peerKey,
        request,responseHop,reliable,data,size,now);
  }
#endif
  void neighborReplyAccepted(uint32_t now){neighborReplyWindow_.accepted(now);}
  // Public and authenticated-private requests share a single stock reply
  // interval. Ordinary retransmission of an already admitted response uses
  // the cached frame, outside the fresh-reply budget.
  bool positionReplyAllowed(uint32_t now)const {
    return stock::position::replyAllowed(sharedPosition_.role(),positionReplySeen_,now,lastPositionReplyAt_);
  }
  void positionReplyAccepted(uint32_t now) {
    lastPositionReplyAt_=now;positionReplySeen_=true;
  }
  void revokePositionData(){
    for(auto &j:acks_)if(j.positionResponse&&j.state!=State::Free)outcome(j,Kind::Failed,4);
    for(auto &j:jobs_)if(j.positionResponse&&j.state!=State::Free)outcome(j,Kind::Failed,4);
  }
  void protectLocalPosition(uint32_t id){
    for(auto &j:jobs_)if(j.state!=State::Free&&j.localInput&&bench::le32(j.frame.bytes+8)==id){j.positionResponse=true;return;}
  }
  void positionConfig(const Config &cfg) {
    if(sharedPosition_.same(cfg))return;
    // Revoke pending location replies; retain active TX buffers and replay history.
    revokePositionData();
    sharedPosition_=cfg;
  }
  void measuredPosition(const meshtastic_Position &value) {
    // Revocation, staleness or replacement cancels queued old location data.
    // Identical projected bytes may represent a new fresh fix. Retain active TX.
    if(livePosition_.has_latitude_i&&(!value.has_latitude_i||value.latitude_i!=livePosition_.latitude_i||
       value.longitude_i!=livePosition_.longitude_i||value.has_altitude!=livePosition_.has_altitude||
       value.altitude!=livePosition_.altitude||value.sats_in_view!=livePosition_.sats_in_view))
      revokePositionData();
    livePosition_=value;
  }
  bool localPosition(meshtastic_Position &out)const{return PositionQuery::localPosition(sharedPosition_,&livePosition_,out);}
  void telemetryUtc(uint32_t utc){telemetryUtc_=utc;}
  void measuredUptime(uint64_t seconds){uptimeAvailable_=seconds<=UINT32_MAX;uptimeSeconds_=uptimeAvailable_?uint32_t(seconds):0;}
  void localStatsSource(const void *owner,LocalStatsObservation (*read)(const void *)){
    statsOwner_=owner;statsRead_=read;
  }
  void measuredAirUtilTx(zt::Observed<float> value){airUtilTx_=value;}
  void measuredChannelUtilization(zt::Observed<float> value){channelUtilization_=value;}
  void measuredHardware(const HardwareSnapshot &value){hardware_=value;}
  void extraTelemetry(void *context,bool (*produce)(void*,uint8_t,uint32_t,uint8_t*,size_t,size_t&)){extraTelemetryContext_=context;extraTelemetry_=produce;}
  void measuredEnvironment(const zt::telemetry::EnvironmentReadings &value){environment_=value;}
  const zt::telemetry::EnvironmentReadings &environment()const{return environment_;}
  void measuredPower(const zt::telemetry::PowerReadings &value){power_=value;}
  const zt::telemetry::PowerReadings &power()const{return power_;}
  unsigned free()const {unsigned n=0;for(auto &j:jobs_)n+=j.state==State::Free;return n;}
  unsigned pendingControls()const {
    unsigned count=0;
    for(const auto &j:jobs_)count+=j.control&&j.state!=State::Free;
    for(const auto &j:acks_)count+=j.state!=State::Free;
    return count;
  }
  bool idle()const {
    for(const auto &j:jobs_)if(j.state!=State::Free)return false;
    for(const auto &j:acks_)if(j.state!=State::Free)return false;
    return true;
  }
  bool nodeInfoPending()const {
    for(const auto &j:jobs_)if(j.state!=State::Free&&j.nodeInfo)return true;
    for(const auto &j:acks_)if(j.state!=State::Free&&j.nodeInfo)return true;
    return false;
  }
  // A separately owned radio excursion reserves its immutable frame ID in the
  // same boot-wide nonreuse filter without creating an Endpoint job. Private
  // runtime producers additionally reserve their durable identity-bound nonce.
  bool reserveExternalId(uint32_t id){if(!id||nonce(id,false))return false;(void)nonce(id,true);return true;}
  uint32_t generatedId(uint32_t random) {
    unsigned tries=0;while((!random||nonce(random,false))&&++tries<16)random=mix(random+0x9e3779b9u);
    return !random||nonce(random,false)?0:random;
  }
  int submitNodeInfoControl(const bench::Frame &frame,bool reliable,uint32_t now,bool signedPublic=false,bool interactive=false) {
    if(frame.len<17||frame.len>255||!bench::le32(frame.bytes+8)||nodeInfoPending())return -ENOBUFS;
    const auto id=bench::le32(frame.bytes+8);if(nonce(id,false))return -EALREADY;
    for(auto &slot:acks_)if(slot.state==State::Free) {
      Job j;
      j.channelSlot=uint8_t(channelIndex());j.frame=frame;j.reliable=reliable;j.control=j.nodeInfo=true;j.signedPublic=signedPublic;
      j.rolePriority=interactive; // The stock interactive NodeInfo uses DEFAULT instead of BACKGROUND.
      if(!annotatePublicControl(j,bench::le32(frame.bytes)))return -EINVAL;
      j.state=State::Queued;j.born=j.queuedAt=now;j.due=now+250;if(!captureLifetime(j))return -EMSGSIZE;slot=j;nonce(id,true);return 0;
    }
    return -ENOBUFS;
  }
  bool modulePending()const{for(const auto&job:acks_)if(job.state!=State::Free&&job.moduleMessage)return true;return false;}
  bool moduleControlAvailable()const {
    if(modulePending())return false;
    for(const auto &slot:acks_)if(slot.state==State::Free)return true;
    return false;
  }
  bool privateControlAvailable()const {
    for(const auto &slot:acks_)if(slot.state==State::Free)return true;
    return false;
  }
  bool privateControlPending()const {
    for(const auto &slot:acks_)if(slot.state!=State::Free&&slot.privateMessage)return true;
    return false;
  }
  // Owner observation of an actually accepted response job. This does not
  // authorize its request or expose ciphertext/borrowed queue state.
  bool queuedControlResponse(uint32_t peer,uint32_t request,bool privateMessage,
                             uint32_t &wireId)const {
    wireId=0;if(peer<=1||peer>=0xfffffffeu||!request)return false;
    for(const auto &job:acks_)if(job.state!=State::Free&&job.control&&
        job.privateMessage==privateMessage&&job.request==request&&
        job.frame.len>=17&&job.frame.len<=255&&bench::le32(job.frame.bytes)==peer) {
      const uint32_t id=bench::le32(job.frame.bytes+8);
      if(!id||wireId){wireId=0;return false;}
      wireId=id;
    }
    return wireId!=0;
  }
  int submitModuleControl(const bench::Frame&frame,uint32_t now,bool signedPublic,
                          bool position=false,bool privateMessage=false,bool reliable=false,
                          uint32_t request=0,bool realtimeAudio=false,bool rolePriority=false){
    if(signedPublic&&privateMessage)return -EINVAL;
    if(frame.len<17||frame.len>255||!bench::le32(frame.bytes+8)||modulePending())return -ENOBUFS;
    const auto id=bench::le32(frame.bytes+8);if(nonce(id,false))return -EALREADY;
    for(auto&slot:acks_)if(slot.state==State::Free){
      Job job;
      job.channelSlot=uint8_t(channelIndex());job.frame=frame;job.control=job.moduleMessage=true;job.positionResponse=position;
      job.privateMessage=privateMessage;job.signedPublic=signedPublic;
      job.request=request;job.rolePriority=rolePriority;
#if defined(CONFIG_ZEPHCORE_AUDIO_CODEC2)
      job.realtimeAudio=realtimeAudio;
#else
      (void)realtimeAudio;
#endif
      if(!privateMessage&&!annotatePublicControl(job,bench::le32(frame.bytes)))return -EINVAL;
      // Callers that own a reliable module control opt into Endpoint retry
      // tracking explicitly. Data.want_response remains independent.
      job.reliable=reliable;
      job.state=State::Queued;job.born=job.queuedAt=now;job.due=now+250;if(!captureLifetime(job))return -EMSGSIZE;slot=job;nonce(id,true);return 0;
    }
    return -ENOBUFS;
  }
  // Service calls this only after admitting a canonical KeyVerification Data
  // payload into its bounded receive slot. Application want_response is not a
  // Routing ACK: only the packet header's want_ack requests this ordinary ACK.
  void acknowledgeKeyVerification(bench::MtCodec &codec,uint32_t ours,uint32_t from,
      uint32_t packetId,unsigned hop,unsigned start,uint32_t now,uint32_t random,
      bool wantAck) {
    if(!wantAck||from<=1||from>=0xfffffffeu||!packetId||hop>start||start>HopPolicy::Maximum)return;
    const unsigned responseHop=HopPolicy::response(start,hop,sharedPosition_.hopLimit());
    reply(codec,ours,from,packetId,responseHop,now,random,true,true);
  }
  bool matchedNodeInfo(uint32_t from,uint32_t request,uint32_t now)const {
    auto matches=[&](const Job &j){return onChannel(j)&&!j.privateMessage&&j.nodeInfo&&j.wantResponse&&j.transmitted&&
      !(j.terminal&&j.result.kind==Kind::Failed)&&
      uint32_t(now-j.born)<j.pendingLifetimeMs&&bench::le32(j.frame.bytes+8)==request&&
      (bench::le32(j.frame.bytes)==from||bench::le32(j.frame.bytes)==0xffffffffu);};
    for(const auto &j:jobs_)if(j.state!=State::Free&&matches(j))return true;
    for(const auto &h:history_)if(h.used&&h.job.result.kind!=Kind::Failed&&matches(h.job))return true;
    return false;
  }
  static bool nodeInfoResponse(const bench::RouteMetadata &m) {
    return m.portnum==4&&m.nodeInfoValid&&m.request_id&&!m.reply_id&&!m.emoji&&
      !m.wantResponse&&!(m.hasBitfield&&(m.bitfield&2));
  }
  // A retry keeps the policy chosen when the packet ID was first reserved.
  bool resolveOriginPolicy(uint32_t id,uint8_t requested,unsigned &hop,bool &response)const {
    const Job *job=originJob(id);
    if(!job||job->requestedOptions==UINT8_MAX)return true;
    if(job->requestedOptions!=requested)return false;
    hop=job->frame.bytes[12]&7;
    response=job->wantResponse;
    return true;
  }
  int submit(uint8_t owner,const uint8_t *p,size_t n,bool reliable,uint32_t now,
             bool nodeInfo=false,bool permitNew=true,bool wantResponse=false,bool *created=nullptr,bool privateMessage=false,
             bool traceroute=false,bool positionQuery=false,bool telemetryQuery=false,bool signedPublic=false,bool localStatsQuery=false,uint8_t telemetryVariant=0,bool neighborQuery=false,bool licensedUnicast=false,uint8_t requestedOptions=UINT8_MAX) {
    if(created)*created=false;
    const uint8_t variant=telemetryVariant?telemetryVariant:uint8_t(localStatsQuery?6:2);
    if((telemetryQuery&&!RemoteTelemetryQuery::supportedVariant(variant))||(localStatsQuery&&variant!=6))return -EINVAL;
    const bool localInput=owner==NoClientOwner;
    const bool localUiInput=owner==LocalUiOwner;
    if((owner>=LocalClientCount&&!localInput&&!localUiInput)||!p||n<17||n>255||!bench::le32(p+8)||(p[12]&7)>HopPolicy::Maximum||
       (localInput&&(nodeInfo||wantResponse||privateMessage||traceroute||positionQuery||telemetryQuery||neighborQuery))||
       (localUiInput&&(!privateMessage||nodeInfo||wantResponse||traceroute||positionQuery||telemetryQuery||signedPublic||localStatsQuery||neighborQuery))||
       (localStatsQuery&&(!telemetryQuery||!wantResponse))||
       (neighborQuery&&(!wantResponse||nodeInfo||traceroute||positionQuery||telemetryQuery||localStatsQuery||(signedPublic&&!licensedUnicast)))||
       (licensedUnicast&&(owner>=LocalClientCount||privateMessage||nodeInfo||traceroute||localInput||localUiInput||
         (!positionQuery&&!telemetryQuery&&!neighborQuery&&wantResponse))))return -EINVAL;
    const auto id=bench::le32(p+8);
    if(localUiInput&&!localUiSubmissionAllowed(id))return -EBUSY;
    for(auto &j:jobs_)if(j.state!=State::Free&&bench::le32(j.frame.bytes+8)==id) {
      if(j.localUiInput!=localUiInput||!same(j,p,n,reliable)||j.nodeInfo!=nodeInfo||j.wantResponse!=wantResponse||j.privateMessage!=privateMessage||j.traceroute!=traceroute||j.positionQuery!=positionQuery||j.telemetryQuery!=telemetryQuery||j.signedPublic!=signedPublic||j.licensedUnicast!=licensedUnicast||j.localStatsQuery!=localStatsQuery||j.neighborQuery!=neighborQuery||j.telemetryVariant!=variant)return -EINVAL;
      if(!localInput&&!localUiInput)j.owners|=1u<<owner;
      return 0;
    }
    for(auto &h:history_)if(h.used&&h.job.result.id==id) {
      if(h.job.localUiInput!=localUiInput||!same(h.job,p,n,reliable)||h.job.nodeInfo!=nodeInfo||h.job.wantResponse!=wantResponse||h.job.privateMessage!=privateMessage||h.job.traceroute!=traceroute||h.job.positionQuery!=positionQuery||h.job.telemetryQuery!=telemetryQuery||h.job.signedPublic!=signedPublic||h.job.licensedUnicast!=licensedUnicast||h.job.localStatsQuery!=localStatsQuery||h.job.neighborQuery!=neighborQuery||h.job.telemetryVariant!=variant)return -EINVAL;
      if(localUiInput) {
        localUiResult_=h.job.result;localUiResultReady_=true;
      } else if(!localInput)emit(1u<<owner,h.job.result);
      return 0;
    }
    if(nonce(id,false))return -EALREADY;
    if(!permitNew)return -EAGAIN;
    for(auto &slot:jobs_)if(slot.state==State::Free) {
      Job j;
      j.channelSlot=uint8_t(channelIndex());memcpy(j.frame.bytes,p,n);j.frame.len=n;j.reliable=reliable;j.nodeInfo=nodeInfo;j.wantResponse=wantResponse;j.privateMessage=privateMessage;j.traceroute=traceroute;j.positionQuery=positionQuery;j.telemetryQuery=telemetryQuery;j.signedPublic=signedPublic;j.licensedUnicast=licensedUnicast;j.localStatsQuery=localStatsQuery;j.neighborQuery=neighborQuery;j.telemetryVariant=variant;
      j.requestedOptions=requestedOptions;j.localInput=localInput;j.localUiInput=localUiInput;
      j.state=State::Queued;j.owners=(localInput||localUiInput)?0:uint8_t(1u<<owner);j.born=j.queuedAt=j.due=now;
      if(!captureLifetime(j))return -EMSGSIZE;
      slot=j;nonce(id,true);if(created)*created=true;return 0;
    }
    return -ENOBUFS;
  }
  int repeatSignedNodeInfo(uint8_t owner,uint32_t id,uint32_t to,unsigned hop,bool reliable,bool wantResponse,uint32_t now,uint8_t channelHash=0) {
    return repeatSignedNodeInfo(owner,id,to,hop,reliable,wantResponse,now,channelHash!=0,channelHash);
  }
  // Explicit presence is required because encrypted keyed channels may
  // legitimately compute hash zero. The legacy overload above keeps its
  // historical nonzero-only optional check for existing callers.
  int repeatSignedNodeInfo(uint8_t owner,uint32_t id,uint32_t to,unsigned hop,bool reliable,bool wantResponse,
                           uint32_t now,bool expectedHashPresent,uint8_t expectedHash) {
    if(owner>=LocalClientCount)return -EINVAL;
    const auto *j=originJob(id);
    if(!j)return -ENOENT;
    if(!j->nodeInfo||!onChannel(*j)||
       (expectedHashPresent&&j->frame.bytes[13]!=expectedHash)||bench::le32(j->frame.bytes)!=to||
       (j->frame.bytes[12]&7)!=hop||j->reliable!=reliable||j->wantResponse!=wantResponse)return -EINVAL;
    return submit(owner,j->frame.bytes,j->frame.len,reliable,now,true,true,wantResponse,nullptr,
                  j->privateMessage,false,false,false,j->signedPublic);
  }
  int repeatPositionQuery(uint8_t owner,uint32_t id,uint32_t to,unsigned hop,uint32_t now){
    const Job *j=originJob(id);if(!j)return -ENOENT;
    if(!j->positionQuery||j->privateMessage||j->licensedUnicast||j->reliable||!j->wantResponse||
       !onChannel(*j)||bench::le32(j->frame.bytes)!=to||(j->frame.bytes[12]&7)!=hop)return -EINVAL;
    return submit(owner,j->frame.bytes,j->frame.len,false,now,false,true,true,nullptr,false,false,true,false,j->signedPublic);
  }
  // Rejoin only already-owned generic ciphertext. A comparison encoding does
  // not publish or queue a new frame and cannot bypass a missing durable lease.
  int repeatPublicApplication(bench::MtCodec &codec,uint8_t owner,uint32_t ours,uint32_t to,uint32_t id,
                              const uint8_t *data,size_t n,unsigned hop,bool reliable,uint32_t now) {
    if(owner>=LocalClientCount)return -EINVAL;
    const auto *known=originJob(id);
    if(!known)return nonce(id,false)?-EALREADY:-ENOENT;
    if(known->privateMessage||known->nodeInfo||known->traceroute||known->positionQuery||known->telemetryQuery||known->neighborQuery||
       !onChannel(*known)||bench::le32(known->frame.bytes+4)!=ours||bench::le32(known->frame.bytes)!=to||
       known->frame.bytes[12]!=(uint8_t(hop|(hop<<5)|((reliable&&to!=0xffffffffu)?8:0)))||
       known->reliable!=reliable)return -EINVAL;
    if(known->licensedUnicast) {
      if(to<=1||to>=0xfffffffeu||!PublicApplication::canonical(data,n)||
         !(known->owners&(uint8_t(1u)<<owner))||
         !codec.matchesEncodedData(known->frame.bytes,known->frame.len,data,n,known->signedPublic))return -EINVAL;
      return submit(owner,known->frame.bytes,known->frame.len,reliable,now,false,false,
                    false,nullptr,false,false,false,false,known->signedPublic,false,0,false,true);
    }
    if(known->signedPublic) {
      // Rejoin the exact locally produced signed broadcast. This comparison
      // uses the queue's provenance flag only to strip its own signature field;
      // it does not treat that flag as RF verification.
      bool canonical=false;
#if defined(CONFIG_MT_GPIO_MODULES)
      canonical=to!=0xffffffffu&&PublicApplication::canonicalGpioRequest(data,n);
#endif
      if(!canonical)canonical=to==0xffffffffu&&PublicApplication::canonical(data,n);
      if(!(known->owners&(uint8_t(1u)<<owner))||(to!=0xffffffffu&&!canonical)||!canonical||!codec.matchesEncodedData(
          known->frame.bytes,known->frame.len,data,n,true))return -EINVAL;
      return submit(owner,known->frame.bytes,known->frame.len,reliable,now,false,false,
                    false,nullptr,false,false,false,false,true);
    }
    if(!PublicApplication::canonical(data,n))return -EINVAL;
    bench::Frame candidate;size_t size=0;
    bool encoded=codec.encodeZeroHopData(ours,to,id,data,n,candidate.bytes,sizeof(candidate.bytes),size);
#if defined(CONFIG_MT_GPIO_MODULES)
    // Stock RemoteHardware READ carries want_response, which the generic
    // application encoder intentionally refuses. Retry only the narrow GPIO
    // canonical encoder so an exact existing READ can be compared/rejoined;
    // the ordinary encoder remains authoritative for all other applications.
    if(!encoded)encoded=codec.encodeGpioRequest(ours,to,id,data,n,candidate.bytes,sizeof(candidate.bytes),size);
#endif
    if(!encoded)return -EINVAL;
    candidate.len=size;candidate.bytes[12]=uint8_t(hop|(hop<<5)|((reliable&&to!=0xffffffffu)?8:0));
    const int result=submit(owner,candidate.bytes,candidate.len,reliable,now,false,false);
    bench::wipeKeyBuffer(&candidate,sizeof(candidate));return result;
  }
  // Rejoin only an exact already-owned licensed plaintext job. Compare the
  // caller's canonical unsigned Data against the stored frame after removing
  // only its XEdDSA signature field; never sign again or reserve another ID.
  int repeatLicensedUnicast(uint8_t owner,uint32_t to,uint32_t id,const uint8_t *source,size_t sourceSize,
      unsigned hop,bool reliable,bool wantResponse,bool positionQuery,bool telemetryQuery,
      bool localStatsQuery,uint8_t telemetryVariant,bool neighborQuery,uint32_t now,uint8_t channelHash=0) {
    return repeatLicensedUnicast(owner,to,id,source,sourceSize,hop,reliable,wantResponse,positionQuery,
      telemetryQuery,localStatsQuery,telemetryVariant,neighborQuery,now,channelHash!=0,channelHash);
  }
  int repeatLicensedUnicast(uint8_t owner,uint32_t to,uint32_t id,const uint8_t *source,size_t sourceSize,
      unsigned hop,bool reliable,bool wantResponse,bool positionQuery,bool telemetryQuery,
      bool localStatsQuery,uint8_t telemetryVariant,bool neighborQuery,uint32_t now,
      bool expectedHashPresent,uint8_t expectedHash) {
    if(owner>=LocalClientCount||!id||hop>HopPolicy::Maximum)return -EINVAL;
    const Job *known=nullptr;
    for(const auto &job:jobs_)if(job.state!=State::Free&&bench::le32(job.frame.bytes+8)==id){known=&job;break;}
    if(!known)for(const auto &history:history_)if(history.used&&history.job.result.id==id){known=&history.job;break;}
    if(!known)return -ENOENT;
    if(!known->licensedUnicast||known->privateMessage||known->nodeInfo||known->traceroute||
       known->positionQuery!=positionQuery||known->telemetryQuery!=telemetryQuery||
       known->localStatsQuery!=localStatsQuery||known->neighborQuery!=neighborQuery||
       known->telemetryVariant!=(telemetryVariant?telemetryVariant:uint8_t(localStatsQuery?6:2))||
       known->wantResponse!=wantResponse||!onChannel(*known)||
       (expectedHashPresent&&known->frame.bytes[13]!=expectedHash)||bench::le32(known->frame.bytes)!=to||
       (known->frame.bytes[12]&7)!=hop||known->reliable!=reliable||
       bool(known->frame.bytes[12]&8)!=reliable||
       !sameLicensedSource(*known,source,sourceSize))return -EINVAL;
    return submit(owner,known->frame.bytes,known->frame.len,reliable,now,false,true,wantResponse,nullptr,
      false,false,positionQuery,telemetryQuery,known->signedPublic,localStatsQuery,telemetryVariant,
      neighborQuery,true);
  }
  int submitLicensedUnicast(uint8_t owner,const bench::Frame &frame,bool reliable,bool wantResponse,
      bool positionQuery,bool telemetryQuery,bool localStatsQuery,uint8_t telemetryVariant,
      bool neighborQuery,bool signedPublic,uint32_t now) {
    if(frame.len<17||frame.len>255||
       bench::le32(frame.bytes+4)<=1||bench::le32(frame.bytes+4)>=0xfffffffeu||
       bench::le32(frame.bytes)<=1||bench::le32(frame.bytes)==0xfffffffeu||
       !bench::le32(frame.bytes+8))return -EINVAL;
    bench::RouteMetadata metadata{};
    const bool correct=bench::MtCodec::decodeData(frame.bytes+16,frame.len-16,metadata)&&
       (signedPublic?metadata.signatureLength==64:metadata.signatureLength==0);
    bench::wipeKeyBuffer(&metadata,sizeof(metadata));if(!correct)return -EINVAL;
    return submit(owner,frame.bytes,frame.len,reliable,now,false,true,wantResponse,nullptr,false,false,
      positionQuery,telemetryQuery,
      signedPublic,localStatsQuery,telemetryVariant,neighborQuery,true);
  }
  // Live Endpoint buffers outlive a hot private session. A terminal Active
  // buffer is still borrowed by the native radio until completion releases it.
  bool hasPrivatePeerWork(uint32_t peer)const {
    if(peer<=1||peer>=0xfffffffeu)return false;
    for(const auto &job:jobs_)if(job.state!=State::Free&&job.privateMessage&&bench::le32(job.frame.bytes)==peer)return true;
    for(const auto &job:acks_)if(job.state!=State::Free&&job.privateMessage&&bench::le32(job.frame.bytes)==peer)return true;
    return false;
  }
  bool pendingPrivate(uint32_t id,uint32_t peer=0,bool transmitted=false)const {
    for(const auto &j:jobs_)if(j.state!=State::Free&&j.privateMessage&&
      bench::le32(j.frame.bytes+8)==id&&(!peer||bench::le32(j.frame.bytes)==peer)&&
      (!transmitted||(j.transmitted&&j.reliable&&!j.terminal&&!j.proofSeen)))return true;
    for(const auto &j:acks_)if(j.state!=State::Free&&j.privateMessage&&
      bench::le32(j.frame.bytes+8)==id&&(!peer||bench::le32(j.frame.bytes)==peer)&&
      (!transmitted||(j.transmitted&&j.reliable&&!j.terminal)))return true;
    return false;
  }
#if defined(CONFIG_MT_RF_ADMIN_GET)
  // A validated application reply ends the sender's reliable wait, without
  // labelling it as a stock Routing proof. Called only after Fresh PKI commit
  // and exact Service query correlation, never from public/host ingress.
  void privateAdminResponse(uint32_t peer,uint32_t request) {
    for(auto &j:jobs_)if(j.state!=State::Free&&j.privateMessage&&j.transmitted&&!j.terminal&&
        bench::le32(j.frame.bytes)==peer&&bench::le32(j.frame.bytes+8)==request)
      outcome(j,Kind::PrivateResponse,0,peer);
  }
#endif
  void authenticated(uint32_t id,uint32_t peer,uint8_t code,const uint8_t *air,size_t airSize,
                     const bench::RouteMetadata &metadata,uint32_t now,bool routeEligible) {
    for(auto &j:jobs_)if(j.state!=State::Free&&j.privateMessage&&j.transmitted&&j.reliable&&!j.terminal&&!j.proofSeen&&
      bench::le32(j.frame.bytes+8)==id&&bench::le32(j.frame.bytes)==peer) {
      if(routeEligible&&!code&&controlEncoder_&&air&&airSize)
        controlEncoder_->observeAuthenticatedOriginResponse(j.frame,air,airSize,metadata,peer,now);
      // A successful proof completes the TX phase once. Keep query/session
      // ownership until the matching application response, expiry or revoke.
      if((j.telemetryQuery||j.positionQuery||j.neighborQuery)&&j.wantResponse&&!code) {
        j.proofSeen=true;
        if(j.state!=State::Active)emit(j.owners,{id,peer,Kind::Authenticated,0,j.attempts});
      }else outcome(j,Kind::Authenticated,code,peer);
    }
    for(auto &j:acks_)if(j.state!=State::Free&&j.privateMessage&&j.transmitted&&j.reliable&&
      bench::le32(j.frame.bytes+8)==id&&bench::le32(j.frame.bytes)==peer)
      outcome(j,Kind::Authenticated,code,peer);
  }
  // Compatibility for local/bench owners that can complete an Endpoint job
  // but have no authenticated air-frame provenance for route learning.
  void authenticated(uint32_t id,uint32_t peer,uint8_t code) {
    bench::RouteMetadata metadata{};
    authenticated(id,peer,code,nullptr,0,metadata,0,false);
    bench::wipeKeyBuffer(&metadata,sizeof(metadata));
  }
  // Exact opaque PKI request echoes are useful only as mutable relay-header
  // observations. Match against the complete immutable ciphertext retained by
  // a recently transmitted endpoint job before asking Service to remember the
  // relay byte; no decrypt, signature, or key decision occurs here.
  bool observePrivateRelayEcho(const uint8_t *air,size_t airSize,uint32_t now) {
    if(!controlEncoder_||!air||airSize<17||airSize>255||
       bench::le32(air+4)!=controlEncoder_->localNode()||air[13]!=0)return false;
    auto matches=[&](const Job &j) {
      const auto &f=j.frame;
      return j.privateMessage&&j.transmitted&&uint32_t(now-j.born)<j.pendingLifetimeMs&&
        f.len==airSize&&f.bytes[13]==0&&bench::le32(f.bytes+4)==bench::le32(air+4)&&
        bench::le32(f.bytes)==bench::le32(air)&&bench::le32(f.bytes+8)==bench::le32(air+8)&&
        (air[12]>>5)==(f.bytes[12]>>5)&&(air[12]&0x18u)==(f.bytes[12]&0x18u)&&
        (air[12]&7u)<=(f.bytes[12]&7u)&&air[15]&&air[15]!=uint8_t(controlEncoder_->localNode())&&
        !memcmp(air+16,f.bytes+16,airSize-16);
    };
    for(const auto &j:jobs_)if(j.state!=State::Free&&matches(j)) {
      controlEncoder_->observeOpaqueOriginRelay(j.frame,air,airSize,now);return true;
    }
    for(const auto &h:history_)if(h.used&&matches(h.job)) {
      controlEncoder_->observeOpaqueOriginRelay(h.job.frame,air,airSize,now);return true;
    }
    return false;
  }
  // Quiescent trust removal only: keep other peers, public broadcasts and all
  // nonce/replay/rate histories. Result positions remain stable for both owners.
  bool invalidatePrivatePeer(uint32_t peer,uint8_t code=4) {
    if(peer<2||peer>=0xfffffffeu||!idle())return false;
    for(auto &h:history_)if(h.used&&(h.job.privateMessage||(h.job.signedPublic&&!h.job.licensedUnicast))&&
        bench::le32(h.job.frame.bytes)==peer)h={};
    for(auto &a:traceApprovals_)if(a.privateResponse&&a.from==peer)a={};
    for(auto &q:results_)for(unsigned i=0;i<q.count;++i) {
      auto &r=q.items[(q.read+i)%8];
      if(r.peer==peer&&(r.kind==Kind::Authenticated||r.kind==Kind::PrivateResponse)) {
        r.kind=Kind::Failed;r.code=code;r.peer=0;
      }
    }
    if(localUiResultReady_&&localUiResult_.peer==peer&&
       (localUiResult_.kind==Kind::Authenticated||localUiResult_.kind==Kind::PrivateResponse)) {
      localUiResult_.kind=Kind::Failed;localUiResult_.code=code;localUiResult_.peer=0;
    }
    return true;
  }
  void cancelPrivate(uint8_t code=4) {
    for(auto &j:jobs_)if(j.state!=State::Free&&(j.privateMessage||j.signedPublic||j.licensedUnicast)) {
      if(j.state==State::Active){j.terminal=true;j.result.kind=Kind::Failed;j.result.code=code;j.result.peer=0;}
      else finish(j,Kind::Failed,code);
    }
    for(auto &j:acks_)if(j.state!=State::Free&&(j.privateMessage||j.signedPublic||j.licensedUnicast))outcome(j,Kind::Failed,code);
    // Do not preserve successful private completion across trust revocation.
    for(auto &h:history_)if(h.used&&(h.job.privateMessage||h.job.signedPublic||h.job.licensedUnicast))h={};
    for(auto &a:traceApprovals_)if(a.privateResponse)a={};
    for(auto &q:results_)for(unsigned i=0;i<q.count;++i) {
      auto &r=q.items[(q.read+i)%8];if(r.kind==Kind::Authenticated||r.kind==Kind::PrivateResponse)r.kind=Kind::Failed,r.code=code,r.peer=0;
    }
    if(localUiResultReady_&&(localUiResult_.kind==Kind::Authenticated||localUiResult_.kind==Kind::PrivateResponse))
      localUiResult_.kind=Kind::Failed,localUiResult_.code=code,localUiResult_.peer=0;
  }
  int submitPrivateControl(const bench::Frame &frame,uint32_t request,uint32_t now,bool positionResponse=false) {
    const auto id=bench::le32(frame.bytes+8);
    if(frame.len<17||frame.len>255||!id||nonce(id,false))return -EINVAL;
    for(auto &slot:acks_)if(slot.state==State::Free) {
      Job j;
      j.channelSlot=uint8_t(channelIndex());j.frame=frame;j.control=j.privateMessage=true;j.request=request;j.reliable=(frame.bytes[12]&8)!=0;
      j.positionResponse=positionResponse;
      j.state=State::Queued;j.born=j.queuedAt=now;j.due=now+250;if(!captureLifetime(j))return -EMSGSIZE;slot=j;nonce(id,true);return 0;
    }
    return -ENOBUFS;
  }
  void cancel(uint8_t code=4) {
    for(auto &j:jobs_)if(j.state!=State::Free)finish(j,Kind::Failed,code);
    for(auto &j:acks_)j={};
    for(auto &a:traceApprovals_)a={};
  }
  void disconnect(uint8_t owner) {
    if(owner>=LocalClientCount)return;
    results_[owner]=Results{};
    for(auto &a:traceApprovals_)if(a.owners&(1u<<owner)) {
      a.owners&=~(1u<<owner);a.delivered&=~(1u<<owner);if(!a.owners)a={};
    }
    for(auto &j:jobs_)if(j.state!=State::Free) {
      j.owners&=~(1u<<owner);
      if(!j.owners&&!j.localInput&&!j.localUiInput)outcome(j,Kind::Failed,4);
    }
  }
  bool take(uint8_t owner,Result &result) {
    if(owner>=LocalClientCount)return false;
    auto &q=results_[owner];if(!q.count)return false;
    result=q.items[q.read];q.read=(q.read+1)%8;--q.count;return true;
  }
  bool takeLocalInput(Result &result) {
    auto &q=localResults_;
    if(!q.count)return false;
    result=q.items[q.read];q.read=(q.read+1)%4;--q.count;
    return true;
  }
  bool takeLocalUiResult(Result &result) {
    if(!localUiResultReady_)return false;
    result=localUiResult_;localUiResult_={};localUiResultReady_=false;
    return true;
  }
  bool localUiSubmissionAllowed(uint32_t id)const {
    if(localUiResultReady_)return false;
    for(const auto &j:jobs_)if(j.state!=State::Free&&j.localUiInput)
      return id&&bench::le32(j.frame.bytes+8)==id;
    return true;
  }
  bool takeTraceRoute(uint8_t owner,uint32_t from,uint32_t packet,uint32_t request,uint32_t now,
                      uint8_t *payload,size_t capacity,size_t &size,bool positionQuery=false,bool telemetryQuery=false,bool privateResponse=false,bool neighborQuery=false,uint8_t positionPort=3) {
    size=0;if(owner>=LocalClientCount||!payload)return false;
    for(auto &a:traceApprovals_)if((!positionQuery||a.positionPort==positionPort)&&a.positionQuery==positionQuery&&a.telemetryQuery==telemetryQuery&&a.privateResponse==privateResponse&&a.neighborQuery==neighborQuery&&(a.privateResponse||!controlEncoder_||controlEncoder_->sameChannel(a.channelSlot,channelIndex()))&&a.from==from&&a.packet==packet&&a.request==request&&
      uint32_t(now-a.at)<90000&&(a.owners&(1u<<owner))&&!(a.delivered&(1u<<owner))) {
      if(a.size>capacity)return false;
      memcpy(payload,a.payload,a.size);size=a.size;a.delivered|=1u<<owner;
      if((a.delivered&a.owners)==a.owners)a={};
      return true;
    }
    return false;
  }
  // Called only by the private runtime after AEAD authentication and durable
  // replay commit. Public request_id claims cannot enter this approval path.
  bool privateTelemetry(uint32_t ours,uint32_t from,uint32_t to,uint32_t packet,
                        const bench::RouteMetadata &m,const uint8_t *air,size_t airSize,uint32_t now) {
    tick(now,0);
    if(to!=ours||from==ours||!packet||!TelemetryQuery::responseShape(m))return false;
    for(auto &j:jobs_)if(j.state==State::Waiting&&j.privateMessage&&j.telemetryQuery&&j.transmitted&&!j.terminal&&
       bench::le32(j.frame.bytes)==from&&bench::le32(j.frame.bytes+4)==ours&&
       bench::le32(j.frame.bytes+8)==m.request_id&&uint32_t(now-j.born)<j.pendingLifetimeMs) {
      uint8_t canonical[239]{};size_t size=0;
      if(!RemoteTelemetryQuery::response(m,j.telemetryVariant,canonical,sizeof(canonical),size)||m.payloadLength>TraceRoute::MaxPayload)return false;
      if(controlEncoder_&&air&&airSize)controlEncoder_->observeAuthenticatedOriginResponse(j.frame,air,airSize,m,from,now);
      auto &approval=traceApprovals_[traceApprovalAt_++%4];approval={};
      approval.from=from;approval.packet=packet;approval.request=m.request_id;approval.at=now;
      approval.owners=j.owners;approval.size=uint8_t(m.payloadLength);approval.telemetryQuery=approval.privateResponse=true;
      memcpy(approval.payload,m.payload,m.payloadLength);outcome(j,Kind::PrivateResponse,0,from);return true;
    }
    return false;
  }
  bool privateTelemetry(uint32_t ours,uint32_t from,uint32_t to,uint32_t packet,
                        const bench::RouteMetadata &m,uint32_t now) {
    return privateTelemetry(ours,from,to,packet,m,nullptr,0,now);
  }
  // Same authenticated, replay-committed admission as private telemetry. The
  // requested peer and application request_id must match a transmitted query;
  // an ACK proof alone keeps the application phase pending.
  bool privatePosition(uint32_t ours,uint32_t from,uint32_t to,uint32_t packet,
                       const bench::RouteMetadata &m,const uint8_t *air,size_t airSize,uint32_t now) {
    tick(now,0);
    if(to!=ours||from==ours||!packet||!positionResponseShape(m))return false;
    for(auto &j:jobs_)if(j.state==State::Waiting&&j.privateMessage&&j.positionQuery&&j.transmitted&&!j.terminal&&
       bench::le32(j.frame.bytes)==from&&bench::le32(j.frame.bytes+4)==ours&&
       bench::le32(j.frame.bytes+8)==m.request_id&&uint32_t(now-j.born)<j.pendingLifetimeMs) {
      uint8_t canonical[239]{};size_t size=0;
      const bool valid=positionResponseData(m,canonical,sizeof(canonical),size);
      bench::wipeKeyBuffer(canonical,sizeof(canonical));
      if(!valid||m.payloadLength>TraceRoute::MaxPayload)return false;
      if(controlEncoder_&&air&&airSize)controlEncoder_->observeAuthenticatedOriginResponse(j.frame,air,airSize,m,from,now);
      auto &approval=traceApprovals_[traceApprovalAt_++%4];approval={};
      approval.from=from;approval.packet=packet;approval.request=m.request_id;approval.at=now;
      approval.owners=j.owners;approval.size=uint8_t(m.payloadLength);approval.positionQuery=approval.privateResponse=true;approval.positionPort=uint8_t(m.portnum);
      memcpy(approval.payload,m.payload,m.payloadLength);outcome(j,Kind::PrivateResponse,0,from);return true;
    }
    return false;
  }
  bool privatePosition(uint32_t ours,uint32_t from,uint32_t to,uint32_t packet,
                       const bench::RouteMetadata &m,uint32_t now) {
    return privatePosition(ours,from,to,packet,m,nullptr,0,now);
  }
  bool privateNeighbor(uint32_t ours,uint32_t from,uint32_t to,uint32_t packet,
                       const bench::RouteMetadata &m,const uint8_t *air,size_t airSize,uint32_t now) {
    tick(now,0);
    if(to!=ours||from==ours||!packet||!NeighborQuery::responseShape(m))return false;
    for(auto &j:jobs_)if(j.state==State::Waiting&&j.privateMessage&&j.neighborQuery&&j.transmitted&&!j.terminal&&
       bench::le32(j.frame.bytes)==from&&bench::le32(j.frame.bytes+4)==ours&&
       bench::le32(j.frame.bytes+8)==m.request_id&&uint32_t(now-j.born)<j.pendingLifetimeMs) {
      uint8_t canonical[227]{};size_t size=0;
      const bool valid=NeighborQuery::response(m,canonical,sizeof(canonical),size);
      bench::wipeKeyBuffer(canonical,sizeof(canonical));
      if(!valid||m.payloadLength>TraceRoute::MaxPayload)return false;
      if(controlEncoder_&&air&&airSize)controlEncoder_->observeAuthenticatedOriginResponse(j.frame,air,airSize,m,from,now);
      auto &approval=traceApprovals_[traceApprovalAt_++%4];approval={};
      approval.from=from;approval.packet=packet;approval.request=m.request_id;approval.at=now;
      approval.owners=j.owners;approval.size=uint8_t(m.payloadLength);approval.neighborQuery=approval.privateResponse=true;
      memcpy(approval.payload,m.payload,m.payloadLength);outcome(j,Kind::PrivateResponse,0,from);return true;
    }
    return false;
  }
  bool privateNeighbor(uint32_t ours,uint32_t from,uint32_t to,uint32_t packet,
                       const bench::RouteMetadata &m,uint32_t now) {
    return privateNeighbor(ours,from,to,packet,m,nullptr,0,now);
  }
  // Private responses use the same measured local producer and bounded
  // per-peer rate/replay records as Public responses, never caller readings.
  bool privateTelemetryReply(uint32_t from,uint32_t request,uint32_t now,
                             uint8_t *data,size_t capacity,size_t &size,bool localStats=false,bool environment=false,bool power=false,uint8_t extra=0) {
    size=0;if((!environment&&!power&&!extra&&!uptimeAvailable_)||!from||!request)return false;
    TelemetryReply *record=nullptr;
    for(auto &r:telemetryReplies_) {
      if(r.from==from&&((r.id==request&&uint32_t(now-r.at)<600000)||uint32_t(now-r.at)<5000))return false;
      if(!record&&(!r.id||uint32_t(now-r.at)>=600000))record=&r;
    }
    if(!record||!(extra?(extraTelemetry_&&extraTelemetry_(extraTelemetryContext_,extra,request,data,capacity,size)):power?PowerQuery::response(power_,request,data,capacity,size,telemetryUtc_):environment?EnvironmentQuery::responseMetrics(zt::telemetry::toMeshtasticEnvironmentMetrics(environment_),request,data,capacity,size,telemetryUtc_):localStats?localStatsResponse(request,data,capacity,size):
        TelemetryQuery::uptimeResponse(uptimeSeconds_,request,airUtilTx_,channelUtilization_,data,capacity,size,hardware_,telemetryUtc_)))return false;
    *record={from,request,now};return true;
  }
  // Reuse public fixed-position consent and bounded peer rate/replay history.
  // The caller must mark the encrypted control as positionResponse so a
  // position-only configuration update revokes it before another send.
  bool privatePositionReply(uint32_t from,uint32_t request,uint32_t now,
                            uint8_t *data,size_t capacity,size_t &size) {
    size=0;if(!data||!capacity||from<=1||from>=0xfffffffeu||!request||!positionReplyAllowed(now))return false;
    TelemetryReply *record=nullptr;
    for(auto &r:positionReplies_) {
      if(r.from==from&&((r.id==request&&uint32_t(now-r.at)<600000)||uint32_t(now-r.at)<5000))return false;
      if(!record&&(!r.id||uint32_t(now-r.at)>=600000))record=&r;
    }
    const unsigned replyChannel=0;
    if(!record||!data||!capacity)return false;
#if defined(CONFIG_MT_TAK_TRACKER)
    uint8_t candidate[239]{};
#else
    uint8_t candidate[64]{};
#endif
    size_t candidateSize=0;
    if(!positionReplyData(request,candidate,sizeof(candidate),candidateSize,replyChannel)||
       !candidateSize||candidateSize>capacity)return false;
    memcpy(data,candidate,candidateSize);size=candidateSize;
    *record={from,request,now};return true;
  }
  void tick(uint32_t now,uint32_t random) {
    for(auto &j:jobs_)if(j.state!=State::Free) {
      if(j.state==State::Active) {
        // Expiry removes response eligibility without releasing a frame still
        // borrowed by the radio's native send/completion contract.
        if((lifetimeAirtime_||(j.privateMessage&&(j.telemetryQuery||j.positionQuery||j.neighborQuery)))&&uint32_t(now-j.born)>=j.pendingLifetimeMs)outcome(j,Kind::Failed,3);
        continue;
      }
      if(uint32_t(now-j.born)>=j.pendingLifetimeMs){finish(j,Kind::Failed,3);continue;}
      if(j.state==State::Queued&&uint32_t(now-j.queuedAt)>=j.queueLifetimeMs){finish(j,Kind::Failed,3);continue;}
      if(j.state==State::Waiting&&bench::due(now,j.due)&&!j.traceroute&&!j.positionQuery&&!j.telemetryQuery&&!j.neighborQuery)retry(j,now,random);
    }
    for(auto &j:acks_)if(j.state!=State::Free) {
      if(j.state==State::Active){if(lifetimeAirtime_&&uint32_t(now-j.born)>=j.pendingLifetimeMs)outcome(j,Kind::Failed,3);continue;}
      if(uint32_t(now-j.born)>=j.pendingLifetimeMs||
          (j.state==State::Queued&&uint32_t(now-j.queuedAt)>=j.queueLifetimeMs)){j={};continue;}
      if(j.state==State::Waiting&&bench::due(now,j.due))retry(j,now,random);
    }
  }
  void blocked(uint32_t ms) {
    for(auto &j:jobs_)if(j.state==State::Waiting)j.due+=ms;
    for(auto &j:acks_)if(j.state==State::Waiting)j.due+=ms;
  }
  Job *next(uint32_t now) {
    Job *selected=nullptr;
    unsigned priority=0;
    for(auto &job:acks_) {
      if(job.state!=State::Queued||!bench::due(now,job.due))continue;
#if defined(CONFIG_ZEPHCORE_AUDIO_CODEC2)
      if(job.realtimeAudio)return &job;
#endif
      // Role controls precede ordinary controls and interactive NodeInfo.
      // Keep the first eligible slot on a tie, including background NodeInfo.
      const unsigned rank=job.nodeInfo?(job.rolePriority?2u:1u):(job.rolePriority?3u:2u);
      if(rank>priority){selected=&job;priority=rank;}
    }
    if(priority>=2)return selected;
    for(auto &job:jobs_)if(job.state==State::Queued&&bench::due(now,job.due))return &job;
    return selected;
  }
  void refused(Job &j,uint8_t code) {finish(j,Kind::Failed,code);}
  void started(Job &j,int status,uint32_t now,uint32_t air,uint32_t random) {
    ++j.attempts;j.air=air;
    if(!status){j.transmitted=true;j.state=State::Active;return;}
    // CAD/channel-busy means native TX was not accepted. Reuse this exact
    // frame and bounded attempt/lifetime policy, including no-ACK controls.
    if(status==-EBUSY){retry(j,now,random);return;}
    finish(j,Kind::Failed,4);
  }
  void complete(Job &j,uint32_t now,uint32_t random) {
    if(j.terminal){finish(j,j.result.kind,j.result.code,j.result.peer);return;}
    if((lifetimeAirtime_||(j.privateMessage&&(j.telemetryQuery||j.positionQuery||j.neighborQuery)))&&uint32_t(now-j.born)>=j.pendingLifetimeMs){finish(j,Kind::Failed,3);return;}
    if((j.traceroute||j.positionQuery||j.telemetryQuery||j.neighborQuery)&&!j.control){
      j.state=State::Waiting;j.due=j.born+j.pendingLifetimeMs;
      // Early proof notification follows native completion, so restore/fault
      // and revocation paths can still override it while the frame is active.
      if(j.privateMessage&&(j.telemetryQuery||j.positionQuery||j.neighborQuery)&&j.proofSeen)
        emit(j.owners,{bench::le32(j.frame.bytes+8),bench::le32(j.frame.bytes),Kind::Authenticated,0,j.attempts});
      return;
    }
    if(!j.reliable){finish(j,Kind::Local,0);return;}
    j.state=State::Waiting;j.due=now+15000+2*j.air+(random%1001);
  }
  // Called exactly once per physical RX, after channel/signature/header checks.
  // Public AES is not authentication: Peer means matching wire evidence only.
  void receive(bench::MtCodec &codec,bench::RoutingCore &routing,uint32_t ours,const uint8_t *p,size_t n,
               const bench::RouteMetadata &wireMetadata,int8_t snr,uint32_t now,uint32_t random,bool armed) {
    bench::RouteMetadata m=wireMetadata;
    struct MetadataWipe {bench::RouteMetadata &value;~MetadataWipe(){bench::wipeKeyBuffer(&value,sizeof(value));}} metadataWipe{m};
    const bool verifiedLicensedSignature=(codec.licensedPlaintext()||wireMetadata.portnum==meshtastic_PortNum_POSITION_APP)&&
      wireMetadata.signatureVerified&&wireMetadata.signatureLength==64;
    // Semantic request/response helpers describe the canonical unsigned Data
    // body. Keep verification provenance in signatureVerified and normalize
    // only the local copy consumed by those helpers.
    if(verifiedLicensedSignature||m.portnum==meshtastic_PortNum_TRACEROUTE_APP)m.signatureLength=0;
    tick(now,random); // Late RX cannot revive an expired request before poll().
    const uint32_t from=bench::le32(p+4),to=bench::le32(p),id=bench::le32(p+8);
    const unsigned hop=p[12]&7,start=(p[12]>>5)&7;
    if(from==ours) {
      for(auto &j:jobs_)if(j.state!=State::Free&&!j.privateMessage&&j.reliable&&j.transmitted&&
          bench::le32(j.frame.bytes)==0xffffffffu&&to==0xffffffffu&&bench::le32(j.frame.bytes+8)==id&&
          n==j.frame.len&&p[13]==j.frame.bytes[13]&&start==(j.frame.bytes[12]>>5)&&
          hop<(j.frame.bytes[12]&7)&&p[15]!=uint8_t(ours)&&!memcmp(p+16,j.frame.bytes+16,n-16))
        outcome(j,Kind::Relay,0);
      // A reliable response can stop once its exact bytes are overheard from
      // a relay. This is control-queue progress, never a user delivery result.
      for(auto &j:acks_)if(j.state!=State::Free&&!j.privateMessage&&j.reliable&&j.transmitted&&
          bench::le32(j.frame.bytes)==to&&bench::le32(j.frame.bytes+8)==id&&
          n==j.frame.len&&p[13]==j.frame.bytes[13]&&start==(j.frame.bytes[12]>>5)&&
          hop<(j.frame.bytes[12]&7)&&p[15]!=uint8_t(ours)&&!memcmp(p+16,j.frame.bytes+16,n-16))
        outcome(j,Kind::Relay,0);
      return;
    }
    if(to!=ours||!from||from>=0xfffffffeu||!id)return;
    // A flooded response already gained an implicit ACK on its relayed path.
    // Stock only sends a terminal zero-hop ACK for direct/next-hop responses.
    const bool terminalAckPath=stock::routing::getHopsAway(start,hop,m.hasBitfield)==0||p[14]!=0;
    const bool nextHopAck=armed&&!(p[12]&8)&&hop>0&&p[14]&&p[14]==uint8_t(ours);
    if(m.portnum==meshtastic_PortNum_POSITION_APP&&!m.request_id) {
      if(armed&&(!controlEncoder_||controlEncoder_->positionSharingAllowed())&&sharedPosition_.positionSharing()&&sharedPosition_.positionPotential()&&hop<=start&&start<=HopPolicy::Maximum&&!(p[12]&0x10)&&
         codec.validatePositionRequest(p,n,verifiedLicensedSignature))positionReply(codec,ours,from,id,hop,start,now,random,(p[12]&8)!=0);
      return;
    }
    if(m.portnum==meshtastic_PortNum_TELEMETRY_APP&&!m.request_id) {
      if(armed&&hop<=start&&start<=HopPolicy::Maximum&&!(p[12]&0x10)&&
         (codec.validateTelemetryRequest(p,n,verifiedLicensedSignature)||codec.validateLocalStatsRequest(p,n,verifiedLicensedSignature)||codec.validateEnvironmentRequest(p,n,verifiedLicensedSignature)||codec.validatePowerRequest(p,n,verifiedLicensedSignature)||codec.validateSupplementalTelemetryRequest(p,n,verifiedLicensedSignature)))
        telemetryReply(codec,ours,from,id,hop,start,now,random,(p[12]&8)!=0,LocalStatsQuery::request(m.payload,m.payloadLength),EnvironmentQuery::request(m.payload,m.payloadLength),PowerQuery::request(m.payload,m.payloadLength),codec.supplementalTelemetryVariant(m.payload,m.payloadLength));
      return;
    }
#if defined(CONFIG_MT_PAXCOUNTER)
    if(m.portnum==meshtastic_PortNum_PAXCOUNTER_APP && m.wantResponse) {
      if(!armed || hop>start || (p[12]&0x10) || !codec.validatePaxRequest(p,n,m.bitfield,verifiedLicensedSignature))return;
      Job *slot=nullptr;for(auto &j:acks_)if(j.state==State::Free){slot=&j;break;}
      uint8_t data[40];size_t size=0;
      if(slot && paxReplyData(from,id,now,data,sizeof(data),size) &&
         enqueueSnapshotReply(*slot,codec,ours,from,id,hop,start,now,random,(p[12]&8)!=0,data,size))
        paxReplyAccepted(from,id,now);
      return;
    }
#endif
    if(m.portnum==meshtastic_PortNum_NEIGHBORINFO_APP&&!m.request_id&&m.wantResponse) {
      if(armed&&hop<=start&&start<=HopPolicy::Maximum&&!(p[12]&0x10)&&codec.validateNeighborRequest(p,n,verifiedLicensedSignature))
        neighborReply(codec,ours,from,id,hop,start,now,random,(p[12]&8)!=0);
      return;
    }
    if((positionResponsePort(m.portnum)||m.portnum==meshtastic_PortNum_TELEMETRY_APP||
        m.portnum==meshtastic_PortNum_NEIGHBORINFO_APP)&&m.request_id) {
      const bool telemetry=m.portnum==meshtastic_PortNum_TELEMETRY_APP;
      const bool neighbor=m.portnum==meshtastic_PortNum_NEIGHBORINFO_APP;
      for(auto &j:jobs_)if(j.state==State::Waiting&&(neighbor?j.neighborQuery:telemetry?j.telemetryQuery:j.positionQuery)&&!j.privateMessage&&j.transmitted&&
        onChannel(j)&&j.frame.bytes[13]==p[13]&&bench::le32(j.frame.bytes)==from&&bench::le32(j.frame.bytes+8)==m.request_id&&uint32_t(now-j.born)<j.pendingLifetimeMs) {
        uint8_t canonical[239]{};size_t size=0;
        if(!(neighbor?NeighborQuery::response(m,canonical,sizeof(canonical),size):telemetry?RemoteTelemetryQuery::response(m,j.telemetryVariant,canonical,sizeof(canonical),size):positionResponseData(m,canonical,sizeof(canonical),size))||
          !(neighbor?codec.validateNeighborQueryReply(p,n,canonical,size,verifiedLicensedSignature):telemetry?codec.validateTelemetryQueryReply(p,n,canonical,size,verifiedLicensedSignature):codec.validatePositionQueryReply(p,n,canonical,size,m.portnum,verifiedLicensedSignature))||
          m.payloadLength>TraceRoute::MaxPayload)return;
        auto &approval=traceApprovals_[traceApprovalAt_++%4];approval={};
        approval.from=from;approval.packet=id;approval.request=m.request_id;approval.at=now;
        approval.channelSlot=uint8_t(channelIndex());approval.owners=j.owners;approval.size=m.payloadLength;approval.positionQuery=!telemetry&&!neighbor;approval.positionPort=approval.positionQuery?uint8_t(m.portnum):0;approval.telemetryQuery=telemetry;approval.neighborQuery=neighbor;
        memcpy(approval.payload,m.payload,m.payloadLength);routing.observeOriginResponse(j.frame,p,n,m,now);outcome(j,Kind::Response,0,from);return;
      }
    } else if(m.portnum==meshtastic_PortNum_TRACEROUTE_APP) {
      if(TraceRoute::responseShape(m)) {
        for(auto &j:jobs_)if(j.state==State::Waiting&&j.traceroute&&!j.privateMessage&&j.transmitted&&
          onChannel(j)&&j.frame.bytes[13]==p[13]&&bench::le32(j.frame.bytes)==from&&bench::le32(j.frame.bytes+8)==m.request_id&&uint32_t(now-j.born)<j.pendingLifetimeMs) {
          uint8_t normalized[TraceRoute::MaxPayload]{};size_t size=0;
          if(!TraceRoute::origin(m,hop,start,snr,normalized,sizeof(normalized),size))return;
          auto &approval=traceApprovals_[traceApprovalAt_++%4];approval={};
          approval.from=from;approval.packet=id;approval.request=m.request_id;approval.at=now;
          approval.channelSlot=uint8_t(channelIndex());approval.owners=j.owners;approval.size=uint8_t(size);memcpy(approval.payload,normalized,size);
          routing.observeOriginResponse(j.frame,p,n,m,now);outcome(j,Kind::Response,0,from);return;
        }
      } else if(armed&&TraceRoute::requestShape(m))traceReply(codec,ours,from,id,hop,start,snr,now,random,m,(p[12]&8)!=0);
    } else if(m.portnum==5) {
      if(!m.routingPresent||!m.routingErrorPresent||!m.request_id)return;
      // A successful Routing ACK is not an application response. A peer NAK terminates
      // the query with its defined error; no raw Routing bypasses correlation.
      for(auto &j:jobs_)if(j.state==State::Waiting&&!j.privateMessage&&(j.positionQuery||j.telemetryQuery||j.neighborQuery)&&j.transmitted&&m.routingError&&
          onChannel(j)&&j.frame.bytes[13]==p[13]&&bench::le32(j.frame.bytes)==from&&bench::le32(j.frame.bytes+8)==m.request_id) {
        uint8_t canonical[239]{};size_t size=0;
        if(!PositionQuery::negative(m,canonical,sizeof(canonical),size)||
          !codec.validatePositionQueryReply(p,n,canonical,size,3,verifiedLicensedSignature))continue;
        const uint8_t code=(m.routingError<=9||(m.routingError>=32&&m.routingError<=39))?uint8_t(m.routingError):2;
        outcome(j,Kind::Failed,code,from);
      }
      bool match=false;
      for(auto &j:jobs_)if(j.state!=State::Free&&!j.privateMessage&&j.reliable&&j.transmitted&&
          onChannel(j)&&j.frame.bytes[13]==p[13]&&bench::le32(j.frame.bytes)==from&&bench::le32(j.frame.bytes+8)==m.request_id) {
        // Preserve defined stock NAK reasons for the requesting client. Unknown
        // future values fall back to GOT_NAK rather than truncating to success.
        const uint8_t code=(m.routingError<=9||(m.routingError>=32&&m.routingError<=39))?
          uint8_t(m.routingError):2;
        match=true;if(!m.routingError)routing.observeOriginResponse(j.frame,p,n,m,now);
        outcome(j,m.routingError?Kind::Failed:Kind::Peer,code);
      }
      for(auto &h:history_)if(h.used&&!h.job.privateMessage&&h.job.transmitted&&h.job.reliable&&
          onChannel(h.job)&&h.job.frame.bytes[13]==p[13]&&bench::le32(h.job.frame.bytes)==from&&h.job.result.id==m.request_id) {
        match=true;if(!m.routingError)routing.observeOriginResponse(h.job.frame,p,n,m,now);
      }
      // ACKs of our reliable generated responses terminate only that control
      // job. Never ACK an ACK-of-ACK, even if a malformed peer sets want_ack.
      for(auto &j:acks_)if(j.state!=State::Free&&!j.privateMessage&&j.reliable&&j.transmitted&&
          onChannel(j)&&j.frame.bytes[13]==p[13]&&bench::le32(j.frame.bytes)==from&&bench::le32(j.frame.bytes+8)==m.request_id)
        outcome(j,m.routingError?Kind::Failed:Kind::Peer,m.routingError?2:0);
      // Stock direct-text ACK can itself request ACK. Terminal response never does.
      if(match&&armed&&(p[12]&8)&&!m.routingError&&terminalAckPath)reply(codec,ours,from,id,0,now,random);
    } else if(m.portnum==4&&m.nodeInfoValid) {
      const bool match=nodeInfoResponse(m)&&matchedNodeInfo(from,m.request_id,now);
      if(match)for(auto &j:jobs_)if(j.state!=State::Free&&onChannel(j)&&j.nodeInfo&&j.wantResponse&&
          j.reliable&&j.transmitted&&onChannel(j)&&j.frame.bytes[13]==p[13]&&bench::le32(j.frame.bytes+8)==m.request_id&&
          (bench::le32(j.frame.bytes)==from||bench::le32(j.frame.bytes)==0xffffffffu)) {
        // Broadcast response evidence names its actual responding peer.
        routing.observeOriginResponse(j.frame,p,n,m,now);outcome(j,Kind::Response,0,from);
      }
      if(match&&armed&&(p[12]&8)&&terminalAckPath)reply(codec,ours,from,id,0,now,random);
      else if(armed&&(p[12]&8)&&!m.request_id&&!m.reply_id&&!m.wantResponse&&!(m.hasBitfield&&(m.bitfield&2))) {
        reply(codec,ours,from,id,HopPolicy::response(start,hop,sharedPosition_.hopLimit(),m.hasBitfield),now,random);
      }
      // Discovery queries already own their NodeInfo response. Limit this
      // next-hop completion to accepted responses or plain announcements.
      else if(nextHopAck&&(match||(!m.request_id&&!m.reply_id&&!m.emoji&&
          !m.wantResponse&&!(m.hasBitfield&&(m.bitfield&2)))))reply(codec,ours,from,id,0,now,random);
    } else if(armed&&(p[12]&8)&&(m.portnum==1||(PublicApplication::allowed(m.portnum)&&PublicApplication::received(codec,p,n,m)))) {
      const unsigned responseHop=HopPolicy::response(start,hop,sharedPosition_.hopLimit(),m.hasBitfield);
      reply(codec,ours,from,id,responseHop,now,random,true,m.portnum!=1);
    } else if(nextHopAck&&(m.portnum==1||(PublicApplication::allowed(m.portnum)&&PublicApplication::received(codec,p,n,m))))
      reply(codec,ours,from,id,0,now,random,false,m.portnum!=1);
  }

  // A broker echo is a local positive Routing acknowledgement, not radio RX.
  // It may finish only an exact, already-transmitted, reliable own-channel job.
  // The service wrapper sets only the synthetic via_mqtt marker; all other
  // frame bytes must match the original outbound frame exactly.
  bool observeMqttSelfEcho(uint32_t ours,uint8_t channelSlot,const uint8_t *frame,
                           size_t length,uint32_t now) {
    if(!frame||length<17||length>255||!(frame[12]&0x10u)||
       bench::le32(frame+4)!=ours||!bench::le32(frame+8))return false;
    for(auto &j:jobs_) {
      const uint32_t age=uint32_t(now-j.born);
      if(j.state==State::Free||j.terminal||!j.reliable||!j.transmitted||
         j.privateMessage||j.signedPublic||j.channelSlot!=channelSlot||
         ((j.state!=State::Active||lifetimeAirtime_)&&age>=j.pendingLifetimeMs)||
         (j.state==State::Queued&&uint32_t(now-j.queuedAt)>=j.queueLifetimeMs)||
         j.frame.len!=length||j.frame.bytes[13]!=frame[13]||
         memcmp(j.frame.bytes,frame,12)!=0||
         (j.frame.bytes[12]!=(frame[12]&uint8_t(~0x10u)))||
         memcmp(j.frame.bytes+13,frame+13,length-13)!=0)continue;
      outcome(j,Kind::MqttEcho,0);
      return true;
    }
    return false;
  }
};
}
