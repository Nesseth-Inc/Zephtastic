// SPDX-License-Identifier: GPL-3.0-only
// Native Service boundary fixture. The provider below fabricates an opaque
// private frame; this exercises Service scheduling/callback ownership, not PKI.
#define CONFIG_MT_MQTT_GATEWAY 1
#include "Service.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t Self=0x12345678u,Peer=0x24680135u;

struct Radio final:mt::Radio {
  int state=0,completionStatus=0,restoreStatus=0,sendStatus=0;
  bool completionReady=false;
  unsigned sends=0;
  uint8_t lastFrame[255]{};size_t lastSize=0;
  bool ready()override{return true;}
  int start()override{state=1;return 0;}
  int restore()override{if(!restoreStatus)state=1;return restoreStatus;}
  unsigned stops=0;
  int stop()override{++stops;state=0;return 0;}
  int health()override{return state;}
  bool busy()override{return false;}
  uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *p,size_t n)override{
    ++sends;lastSize=n;assert(n<=sizeof(lastFrame));memcpy(lastFrame,p,n);return sendStatus;
  }
  bool completion(int &result)override{
    if(!completionReady)return false;
    completionReady=false;result=completionStatus;return true;
  }
};

struct PrivateProvider final:mt::pki::PrivateRuntime {
  mt::Service *service=nullptr;
  bool readyFlag=true,signedPublicJob=false,authenticateOnProof=false;
  unsigned submitCalls=0,proofCalls=0;
  uint32_t authenticateId=0;
  bool encodeOk=false;int endpointStatus=0;
  uint32_t local;
  explicit PrivateProvider(uint32_t id):local(id){}
  uint32_t peer()const override{return Peer;}
  bool ready(uint32_t node)const override{return readyFlag&&node==local;}
  bool publicIdentity(uint8_t out[32])const override{memset(out,0x42,32);return true;}
#if defined(CONFIG_MT_RF_ADMIN_GET)
  bool peerPublicIdentity(uint32_t node,uint8_t key[32])const override{
    return matchesPeer(node)&&publicIdentity(key);
  }
  int submitAdminGet(mt::Endpoint &endpoint,uint8_t owner,uint32_t to,uint32_t id,
                     const uint8_t *data,size_t n,unsigned hop,bool reliable,const uint8_t *key,
                     size_t keySize,bool explicitPki,uint32_t now)override{
    return submit(endpoint,owner,to,id,data,n,hop,reliable,key,keySize,explicitPki,now);
  }
#endif
  bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,uint32_t,uint32_t,
                 bool,uint32_t,bench::Frame &)override{return false;}
  void freeze()override{}
  bool matchesPeer(uint32_t node)const override{return node==Peer;}
  bool revoke(uint32_t)override{return false;}
  void clearEvent()override{}
  void sweep(mt::Endpoint &)override{}
  int submit(mt::Endpoint &endpoint,uint8_t owner,uint32_t to,uint32_t id,
             const uint8_t *data,size_t size,unsigned hop,bool reliable,
             const uint8_t *,size_t,bool,uint32_t now)override{
    ++submitCalls;
    if(!service||!data||!size||size>200||hop>7)return -EINVAL;
    mt::PrimaryChannel::Selection key{};
    if(!service->config.channelSelection(0,key))return -EINVAL;
    const bool keyed=service->codec.setKey(key.hash,key.bytes,key.length);
    mt::PrimaryChannel::clear(key);if(!keyed)return -EINVAL;
    const uint8_t appPayload[]={0x5a};uint8_t canonical[239]{};size_t canonicalSize=0;
    if(!mt::PublicApplication::encode(8,appPayload,sizeof(appPayload),0,0,
                                       canonical,sizeof(canonical),canonicalSize,1))return -EINVAL;
    bench::Frame frame{};size_t frameSize=0;
    encodeOk=service->codec.encodeZeroHopData(local,to,id,canonical,canonicalSize,
                                               frame.bytes,sizeof(frame.bytes),frameSize);
    if(!encodeOk)return -EINVAL;
    frame.len=frameSize;frame.bytes[12]=uint8_t(hop|(hop<<5)|(reliable?8:0));
    // A Service job-classification adversary: the second fixture sets the
    // public-signature marker while retaining privateMessage, which must not
    // enter the private MQTT callback.
    endpointStatus=endpoint.submit(owner,frame.bytes,frame.len,reliable,now,false,true,false,
                                   nullptr,true,false,false,false,signedPublicJob);
    return endpointStatus;
  }
  void receive(mt::Endpoint &,bench::MtCodec &,const uint8_t *,size_t,
               uint32_t,uint32_t,bool)override{}
  void proof(mt::Endpoint &endpoint,const uint8_t *,size_t,
             const bench::RouteMetadata &,uint32_t)override{
    ++proofCalls;
    if(authenticateOnProof)endpoint.authenticated(authenticateId,Peer,0);
  }
  bool event(uint8_t,const uint8_t *,size_t,mt::pki::PrivateEvent &)override{return false;}
  bool encodePublicApplication(bench::MtCodec &codec,uint32_t to,uint32_t id,
      const uint8_t *data,size_t size,bench::Frame &frame)override{
    mt::PrimaryChannel::Selection key{};
    if(!service||!service->config.channelSelection(0,key))return false;
    const bool keyed=codec.setKey(key.hash,key.bytes,key.length);
    mt::PrimaryChannel::clear(key);if(!keyed)return false;
    size_t n=0;const bool ok=codec.encodeZeroHopData(local,to,id,data,size,
      frame.bytes,sizeof(frame.bytes),n);if(ok)frame.len=n;return ok;
  }
};

struct Capture {
  unsigned privateCalls=0,ordinaryCalls=0;
  uint32_t privateFrom=0,privateId=0;
  uint8_t privateFrame[255]{};size_t privateSize=0;
  static void privateTx(void *ctx,const uint8_t *p,size_t n,uint32_t from){
    auto &c=*static_cast<Capture *>(ctx);++c.privateCalls;c.privateFrom=from;
    assert(n<=sizeof(c.privateFrame));c.privateSize=n;memcpy(c.privateFrame,p,n);
    c.privateId=bench::le32(p+8);
  }
  static void ordinaryTx(void *ctx,const uint8_t *p,size_t n,
                         const bench::RouteMetadata &,unsigned){
    auto &c=*static_cast<Capture *>(ctx);assert(p&&n);++c.ordinaryCalls;
  }
};

struct Fixture {
  Radio radio;PrivateProvider provider{Self};
  mt::test::KnownChannelFixtureClock channelClock;
  mt::Service service{radio,true,Self,81,true,false,&provider};
  Capture capture;
  Fixture(){
    provider.service=&service;channelClock.bind(service);
    auto cfg=service.config;cfg.bytes[4]=1;cfg.setTxEnabled(true);
    assert(cfg.valid()&&cfg.provisioned()&&service.apply(cfg));
    assert(service.bindMqttPrivateTxSink(&Capture::privateTx,&capture));
    assert(service.bindMqttFrameSink(&Capture::ordinaryTx,&capture));
    assert(service.start(false,0)&&service.armEndpoint(true));
  }
  int privateJob(uint32_t id,bool reliable=true){
    static const uint8_t opaque[]={0x91,0x27,0x5a,0xc3};
    return service.submitPrivate(0,Peer,id,opaque,sizeof(opaque),3,reliable,
                                 nullptr,0,false,10);
  }
  void pollSend(uint32_t now,uint32_t random=0){service.poll(now,random);}
  void complete(uint32_t now,int status=0){
    radio.completionReady=true;radio.completionStatus=status;service.poll(now,0);
  }
  void authenticateDuringActiveTx(uint32_t id){
    provider.authenticateOnProof=true;provider.authenticateId=id;
    mt::PrimaryChannel::Selection key{};assert(service.config.channelSelection(0,key));
    assert(service.codec.setKey(key.hash,key.bytes,key.length));mt::PrimaryChannel::clear(key);
    const uint8_t appPayload[]={0x62};uint8_t data[239]{},frame[255]{};
    size_t dataSize=0,frameSize=0;
    assert(mt::PublicApplication::encode(8,appPayload,sizeof(appPayload),0,0,
                                         data,sizeof(data),dataSize,1));
    assert(service.codec.encodeZeroHopData(Peer,Self,id+1000,data,dataSize,
                                            frame,sizeof(frame),frameSize));
    service.rx(frame,frameSize,-80,1200,77);
    assert(provider.proofCalls==1);
  }
};

void privateSuccessOnceAcrossReliableRetry(){
  Fixture f;const int admitted=f.privateJob(101);
  if(admitted||f.provider.submitCalls!=1)std::fprintf(stderr,"private submit=%d provider calls=%u encode=%d endpoint=%d\n",admitted,f.provider.submitCalls,f.provider.encodeOk,f.provider.endpointStatus);
  assert(admitted==0&&f.provider.submitCalls==1);
  f.pollSend(1000);assert(f.radio.sends==1&&f.capture.privateCalls==0);
  const auto first=std::vector<uint8_t>(f.radio.lastFrame,f.radio.lastFrame+f.radio.lastSize);
  f.complete(2000);assert(!f.service.faulted()&&f.capture.privateCalls==1);
  assert(f.capture.privateFrom==Self&&f.capture.privateId==101&&
         f.capture.privateSize==first.size()&&
         !memcmp(f.capture.privateFrame,first.data(),first.size()));
  // Reliable endpoint retry uses the same local job/frame. The Service's
  // per-job/id notice latch must suppress a duplicate private MQTT event.
  uint32_t retryAt=0;
  for(uint32_t t=3000;t<=30000&&f.radio.sends<2;t+=1000){f.pollSend(t);retryAt=t;}
  assert(f.radio.sends==2&&f.radio.lastSize==first.size()&&
       !memcmp(f.radio.lastFrame,first.data(),first.size()));
  f.complete(retryAt+1000);assert(!f.service.faulted()&&f.capture.privateCalls==1);
  puts("PASS actual Service local private completion emits once across same-job retry");
}

void earlyAuthenticationStillWaitsForPhysicalSuccess(){
  {
    Fixture f;assert(f.privateJob(111)==0);f.pollSend(1000);
    assert(f.radio.sends==1&&f.service.active()&&f.capture.privateCalls==0);
    f.authenticateDuringActiveTx(111);
    assert(f.capture.privateCalls==0); // proof does not outrun RF completion
    f.complete(2000,0);
    assert(!f.service.faulted()&&f.capture.privateCalls==1&&
           f.capture.privateId==111&&f.capture.privateFrom==Self);
  }
  {
    Fixture f;assert(f.privateJob(112)==0);f.pollSend(1000);
    f.authenticateDuringActiveTx(112);f.complete(2000,-EIO);
    assert(f.service.faulted()&&f.capture.privateCalls==0&&f.service.sent==0);
  }
  {
    Fixture f;assert(f.privateJob(113)==0);f.pollSend(1000);
    f.authenticateDuringActiveTx(113);f.radio.restoreStatus=-EIO;f.complete(2000,0);
    assert(f.service.faulted()&&f.capture.privateCalls==0&&f.service.sent==0);
  }
  puts("PASS actual Endpoint.authenticated while TX active: uplink follows physical success only");
}

void failureAndRestoreDoNotEmit(){
  {
    Fixture f;assert(f.privateJob(102)==0);f.pollSend(1000);assert(f.radio.sends==1);
    f.complete(2000,-EIO);
    assert(f.service.faulted()&&f.capture.privateCalls==0&&f.service.sent==0);
  }
  {
    Fixture f;assert(f.privateJob(103)==0);f.pollSend(1000);assert(f.radio.sends==1);
    f.radio.restoreStatus=-EIO;f.complete(2000,0);
    assert(f.service.faulted()&&f.capture.privateCalls==0&&f.service.sent==0);
  }
  puts("PASS actual Service TX error and RX-restore failure emit no private event");
}

void ordinaryAndNonPrivateClassesStaySeparate(){
  {
    Fixture f;const uint8_t payload[]={'x'};uint8_t text[239]{};size_t textSize=0;
    assert(mt::PublicApplication::encode(8,payload,sizeof(payload),0,0,text,
                                         sizeof(text),textSize,1));
    assert(!f.service.submitPublicApplication(0,0xffffffffu,104,text,textSize,
                                               0,false,10));
    f.pollSend(1000);f.complete(2000);
    assert(!f.service.faulted()&&f.capture.ordinaryCalls==1&&f.capture.privateCalls==0);
  }
  {
    Fixture f;f.provider.signedPublicJob=true;assert(f.privateJob(105)==0);
    f.pollSend(1000);f.complete(2000);
    assert(!f.service.faulted()&&f.capture.privateCalls==0&&f.capture.ordinaryCalls==1);
  }
  {
    Fixture f;uint8_t rx[255]{};size_t n=0;uint8_t data[239]{};size_t dataSize=0;
    const uint8_t appPayload[]={0x33};
    mt::PrimaryChannel::Selection key{};assert(f.service.config.channelSelection(0,key));
    assert(f.service.codec.setKey(key.hash,key.bytes,key.length));mt::PrimaryChannel::clear(key);
    assert(mt::PublicApplication::encode(8,appPayload,sizeof(appPayload),0,0,data,
                                         sizeof(data),dataSize,1));
    assert(f.service.codec.encodeZeroHopData(Peer,Self,106,data,dataSize,rx,sizeof(rx),n));
    f.service.rx(rx,n,-80,100,7);
    assert(f.capture.privateCalls==0&&f.capture.ordinaryCalls==1&&f.radio.sends==0);
  }
  puts("PASS ordinary local callback preserved; signedPublic-marked job and RX stay out of private callback");
}
}

int main(){
  privateSuccessOnceAcrossReliableRetry();
  earlyAuthenticationStillWaitsForPhysicalSuccess();
  failureAndRestoreDoNotEmit();
  ordinaryAndNonPrivateClassesStaySeparate();
  puts("PASS M122 real Service private-uplink native fixture; fake provider/radio only");
  return 0;
}
