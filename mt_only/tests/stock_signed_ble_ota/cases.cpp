// SPDX-License-Identifier: GPL-3.0-only
#include "BleTransport.cpp"
#include <string>
#include <vector>
#include <cstdio>
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
#include <future>
extern "C" int __wrap_settings_save_one(const char *,const void *,size_t);
static int nativeResult;
extern "C" int __real_settings_save_one(const char *,const void *,size_t){return nativeResult;}
extern "C" int __real_settings_delete(const char *){return nativeResult;}
extern "C" bool zt_bt_settings_identity_work_pending(){return false;}
#endif
// Zephyr bt_gatt_is_subscribed locates the characteristic's CCC and calls
// its read callback. Execute the actual carrier's per-epoch CCC logic here.
static bool otaIsSubscribed(bt_conn *conn,const bt_gatt_attr *value,uint16_t flag){
  if(!fixtureDb||value!=&fixtureDb->attrs[2]||flag!=1)return false;
  auto *attr=&fixtureDb->attrs[3];uint8_t bits[2]{};
  return attr->read(conn,attr,bits,2,0)==2&&bits[0]==1&&!bits[1];
}
// Model a BT RX callback scheduled after main's epoch snapshot and before
// Zephyr dequeues its first accepted command.
static void (*beforeOtaGet)();
static int otaGet(MockQueue *queue,void *data,int timeout){
  if(beforeOtaGet){auto callback=beforeOtaGet;beforeOtaGet=nullptr;callback();}
  return k_msgq_get(queue,data,timeout);
}
#define k_msgq_get otaGet
#define bt_gatt_is_subscribed otaIsSubscribed
#include "update/SignedBleOta.cpp"
#undef bt_gatt_is_subscribed
#undef k_msgq_get

#if defined(TEST_STOCK_PROCESSOR)
// Both byte-stream parsers see the same valid Android writes. Flash ownership
// stays in the separate synthetic sink on each side of this comparison.
#define INFO(...) ((void)0)
class OtaProcessor {
  enum {STATE_IDLE,STATE_DOWNLOADING} _state=STATE_IDLE;
  uint8_t hash_[32]{0x11};
  char _cmd_buffer[96]{};size_t _cmd_len=0;
  mt::update::signedOta::Session sink_{hash_};
  void capture(const char *p){
    while(*p){const char *end=strchr(p,'\n');assert(end);out.emplace_back(p,end-p+1);p=end+1;}
  }
  void handleCommand(){capture(sink_.command(_cmd_buffer));if(sink_.downloading())_state=STATE_DOWNLOADING;}
  void handleBinaryChunk(const uint8_t *p,size_t n){capture(sink_.write(p,n));}
public:
  std::vector<std::string> out;
  void process(const uint8_t*,size_t);
};
#include "stock-process.inc"
#undef INFO
static void compareStock(const std::vector<std::string> &actual){
  mt::update::signedOta::stored.clear();mt::update::signedOta::selected=false;
  OtaProcessor stock;
  const auto text=std::string("VERSION\r\nOTA 24 ")+std::string(64,'1')+"\n";
  for(size_t pos=0;pos<text.size();pos+=20)
    stock.process(reinterpret_cast<const uint8_t *>(text.data()+pos),std::min<size_t>(20,text.size()-pos));
  uint8_t bytes[24];for(unsigned i=0;i<24;++i)bytes[i]=i;
  stock.process(bytes,20);stock.process(bytes+20,4);
  assert(stock.out==actual);
}
#endif

namespace {
using namespace mt::update;
std::string scenario;
bt_conn original,update,unrelated;
unsigned phase=0,pumps=0,advertisements=0,pressure=0;
std::vector<std::string> replies;
static std::string queuedError;
static int64_t errorQueuedAt,disconnectAt,peerSawErrorAt;
static bool connectionStopped;
static bool peerLag(){return scenario=="terminal-peer-lag"||scenario=="terminal-local-timeout";}
static void completeNotification(){
  if(queuedNotification.func){
    const auto completed=queuedNotification;queuedNotification={};
    completed.func(&update,completed.user_data);
  }
}
static bool delayedError(){return peerLag()||scenario=="signature-full-async"||scenario=="signature-async"||scenario=="syntax-async"||scenario=="terminal-disconnect"||scenario=="terminal-timeout";}
static ssize_t data(bt_conn &peer,const uint8_t *p,size_t n,uint8_t flags=0){
  auto *attr=&attributes[5];return attr->write(&peer,attr,p,uint16_t(n),0,flags);
}
static void sendCommand(const std::string &line){
  for(size_t i=0;i<line.size();i+=20){
    const auto count=std::min<size_t>(20,line.size()-i);
    assert(data(update,reinterpret_cast<const uint8_t *>(line.data()+i),count)==ssize_t(count));
  }
}
static void secure(bt_conn &conn){
  conn.security=BT_SECURITY_L2;mt::ble::callbacks.security_changed(&conn,BT_SECURITY_L2,BT_SECURITY_ERR_SUCCESS);
}
#include "TimingCases.h"
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
std::future<int> nativeWrite;
unsigned leasePumps=0,leaseOpens=0;
uint32_t leaseToken;
static void openLease(){
  using namespace mt::ble;
  assert(!leaseToken&&store::payloadAllowed());
  nativeResult=scenario=="lease-failure"?-EIO:0;
  nativeWrite=std::async(std::launch::async,[]{return __wrap_settings_save_one("bt/ccc/synthetic",nullptr,0);});
  for(unsigned i=0;i<1000&&!leaseToken;++i){
    leaseToken=store::pendingToken();
    if(!leaseToken)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(leaseToken&&store::healthy()&&!store::payloadAllowed());
  ++leaseOpens;leasePumps=0;
}
static void finishLease(){
  using namespace mt::ble;
  assert(store::prepare(leaseToken)&&store::grant(leaseToken));store::wake();
  assert(nativeWrite.get()==nativeResult);
  uint32_t token=0;int result=0;
  assert(store::completed(token,result)&&token==leaseToken&&result==nativeResult);
  store::settle(token,0);leaseToken=0;
}
static void serviceLease(){
  if(!leaseToken)return;
  ++leasePumps;
  if(scenario=="lease-late"){
    if(leasePumps>1)return;
    // The native primitive has returned; main can still be restoring the
    // storage window. A granted operation has no admission timeout.
    assert(mt::ble::store::prepare(leaseToken)&&mt::ble::store::grant(leaseToken));
    mt::ble::store::wake();assert(!nativeWrite.get());uint32_t done=0;int result=0;
    assert(mt::ble::store::completed(done,result)&&done==leaseToken&&!result);
    assert(mt::update::signedOta::stored.size()==20);
    clockOffset+=9100;
    return;
  }
  if(scenario=="lease-first")assert(replies.empty()&&mt::update::signedOta::stored.empty());
  else if(scenario=="lease-ack")assert(replies.size()==3);
  else assert(mt::update::signedOta::stored.size()==20);
  assert(!mt::update::signedOta::selected);
  if(leasePumps<3)return;
  if(scenario=="lease-disconnect")mt::ble::callbacks.disconnected(&update,0x13);
  else if(scenario=="lease-stale"){
    update.security=BT_SECURITY_L1;mt::ble::callbacks.security_changed(&update,BT_SECURITY_L1,BT_SECURITY_ERR_SUCCESS);
  }
  finishLease();
  if(scenario=="lease-disconnect"){
    mt::ble::callbacks.connected(&unrelated,0);secure(unrelated);
    uint8_t bytes[]={20,21,22,23};assert(data(unrelated,bytes,sizeof(bytes))<0);
    mt::ble::callbacks.disconnected(&unrelated,0x13);
  }
}
#endif
static void pump(){
  ++pumps;
  if(peerLag()&&disconnectAt&&!connectionStopped&&scenario!="terminal-local-timeout"&&k_uptime_get()-disconnectAt>=75){
    assert(fixtureDb&&mt::ble::connected());
    mt::ble::callbacks.disconnected(&update,0x13);connectionStopped=true;
  }
  if(!queuedError.empty()&&atomic_get(&armed)&&mt::ble::connected()&&k_uptime_get()-errorQueuedAt>=55){
    if(scenario=="terminal-disconnect"){
      mt::ble::callbacks.disconnected(&update,0x13);mt::ble::callbacks.connected(&unrelated,0);secure(unrelated);
      completeNotification(); // A late completion belongs to the disconnected epoch.
      mt::ble::callbacks.disconnected(&unrelated,0x13);queuedError.clear();
    }else if(scenario!="terminal-timeout"){
      replies.push_back(queuedError);queuedError.clear();peerSawErrorAt=k_uptime_get();completeNotification();
    }
  }
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
  serviceLease();
  if(mockPendingWork&&job.action==Action::Notify&&scenario=="lease-ack"&&job.length==4&&!memcmp(job.text,"ACK\n",4)&&!leaseOpens)openLease();
#endif
  if(mockPendingWork){
    const Action a=job.action;
    if(a==Action::Notify&&scenario=="budget")mockNotifyResult=pressure++<3?-ENOMEM:0;
    if(a==Action::Advertise){
      if(scenario=="recycle")mockAdvertiseResult=pressure++<3?-ENOMEM:0;
      else if(scenario=="recycle-busy"){++pressure;mockAdvertiseResult=-EBUSY;}
      else if(scenario=="wrong-advertiser"){++pressure;mockAdvertiseResult=-EALREADY;}
    }
    const auto beforeDisconnect=mockDisconnectCalls;
    mockRunWork();
    if(mockDisconnectCalls!=beforeDisconnect&&mt::ble::connected()){
      if(atomic_get(&armed))mt::ble::callbacks.disconnected(&original,0x13);
      else if(peerLag())disconnectAt=k_uptime_get();
      else mt::ble::callbacks.disconnected(&update,0x13);
    }
    if(a==Action::Advertise&&job.result==0)++advertisements;
    if(a==Action::Notify&&job.result==0){
      std::string value(mockNotification.begin(),mockNotification.end());
      if(delayedError() && value.rfind("ERR",0)==0){
        assert(queuedError.empty());queuedError=value;errorQueuedAt=k_uptime_get();
        if(peerLag())completeNotification(); // Controller completion can precede client delivery.
      }else{replies.push_back(value);completeNotification();}
    }
    if(a==Action::Cleanup&&peerLag()){
      assert(peerSawErrorAt>=errorQueuedAt+55&&disconnectAt>=errorQueuedAt+2000);
      if(scenario=="terminal-peer-lag")assert(connectionStopped);
      else assert(mt::ble::connected()&&k_uptime_get()-disconnectAt>=5000&&k_uptime_get()-disconnectAt<5100);
    }
  }
  if(!fixtureDb||!advertisements||!atomic_get(&armed)||scenario=="timeout")return;
  if(timing::enabled()&&phase){timing::pump();return;}
  if(phase==0){
    assert(fixtureDb->attr_count==6&&fixtureDb->attrs[5].permissions==BT_GATT_PERM_WRITE_ENCRYPT);
    mt::ble::callbacks.connected(&update,0);
    if(scenario=="reconnect"&&advertisements==1){mt::ble::callbacks.disconnected(&update,0x13);phase=8;return;}
    const uint8_t enable[]={1,0};auto *ccc=&fixtureDb->attrs[3];uint8_t out[8]{};
    if(scenario=="android-key-retry"){
      // A stock Android central first tries its cached key on the new OTA link.
      for(int i=0;i<2;++i){
        mt::ble::callbacks.security_changed(&update,BT_SECURITY_L1,BT_SECURITY_ERR_PIN_OR_KEY_MISSING);
        assert(ccc->write(&update,ccc,enable,2,0,0)<0&&data(update,enable,2)<0);
      }
    }
    assert(ccc->write(&update,ccc,enable,2,0,0)<0&&data(update,enable,2)<0);
    secure(update);
    assert(mt::ble::writeTo(&update,nullptr,"x",1,0,0)<0&&mt::ble::readFrom(&update,nullptr,out,sizeof(out),0)<0);
    assert(data(unrelated,enable,2)<0&&data(update,enable,2)<0);
    assert(!parameterRequests);
    assert(ccc->write(&update,ccc,enable,2,0,0)==2);
    assert(ccc->read(&unrelated,ccc,out,2,0)<0&&ccc->read(&update,ccc,out,2,0)==2&&out[0]==1);
    assert(data(update,enable,2,BT_GATT_WRITE_FLAG_PREPARE)<0);
    if(scenario=="first-write")beforeOtaGet=[](){sendCommand("VERSION\r\n");};
    else {
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
      if(scenario=="lease-first"){
        openLease();
        uint8_t command='V';
        assert(!atomic_get(&peerEpoch)&&data(unrelated,&command,1)<0);
        // A successful, encrypted CCC write has authorized this connection.
        // Android aborts the START command on an ATT write error.
        sendCommand("VERSION\r\n");phase=1;return;
      }
#endif
      sendCommand("VERSION\r\n");
    }
    phase=1;
  }else if(phase==8&&advertisements>=2){
    phase=0;
  }else if(phase==1&&replies==std::vector<std::string>{"OK 81 test 1 v2\n"}){
    if(scenario=="overflow"){
      uint8_t ch='X';for(unsigned i=0;i<4;++i)assert(data(update,&ch,1)==1);
      assert(data(update,&ch,1)<0);phase=9;return;
    }
    if(scenario=="syntax-async"){uint8_t invalid=0;assert(data(update,&invalid,1)==1);phase=9;return;}
    sendCommand("OTA 24 "+std::string(64,scenario=="wronghash"?'2':'1')+"\n");phase=2;
  }else if(phase==2&&!replies.empty()&&replies.back()=="ERR Hash Rejected\n"){
    mt::ble::callbacks.disconnected(&update,0x13);phase=9;
  }else if(phase==2&&replies.size()==3){
    assert(replies[1]=="ERASING\n"&&replies[2]=="OK\n");
    uint8_t bytes[20];for(unsigned i=0;i<20;++i)bytes[i]=uint8_t(i);
    assert(data(update,bytes,20)==20);phase=3;
  }else if(phase==3&&replies.size()==4){
    assert(replies.back()=="ACK\n");
    if(scenario=="downgrade"){
      update.security=BT_SECURITY_L1;mt::ble::callbacks.security_changed(&update,BT_SECURITY_L1,BT_SECURITY_ERR_SUCCESS);
      uint8_t bytes[]={20,21,22,23};assert(data(update,bytes,4)<0);
      mt::ble::callbacks.disconnected(&update,0x13);phase=9;return;
    }
    if(scenario=="disconnect"){
      mt::ble::callbacks.disconnected(&update,0x13);mt::ble::callbacks.connected(&unrelated,0);secure(unrelated);
      uint8_t bytes[]={20,21,22,23};assert(data(unrelated,bytes,4)<0);
      mt::ble::callbacks.disconnected(&unrelated,0x13);phase=9;return;
    }
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
    if(scenario.rfind("lease-",0)==0&&scenario!="lease-ack"&&scenario!="lease-first"){
      openLease();uint8_t foreign='F';
      assert(data(unrelated,&foreign,1)<0);
    }
#endif
    uint8_t bytes[]={20,21,22,23};assert(data(update,bytes,4)==4);phase=4;
  }else if(!peerLag()&&phase==4&&replies.size()==5&&replies.back()=="ERR Hash Mismatch\n"){
    mt::ble::callbacks.disconnected(&update,0x13);phase=9;
  }
}
}
int main(int argc,char **argv){
  using namespace mt::update;assert(argc==2);scenario=argv[1];mockMtu=scenario=="signature-full-async"?517:23;
  if(timing::enabled())timing::setSize();
  auto policy=mt::BluetoothSettings::defaults();policy.enabled=scenario!="disabled";
  mockAutoSecurity=false;mockSecurityChanged=mt::ble::callbacks.security_changed;
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
  mt::ble::store::initialize();assert(mt::ble::store::beginBoot(true));
#endif
  assert(!fixtureDb&&mt::ble::init(0x12345678,policy,true)==0);
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
  mt::ble::store::endBoot(0);mt::ble::poll();assert(mt::ble::store::payloadAllowed());
#endif
  const uint8_t hash[32]={0x11};serveSignedBle(hash,pump);assert(!fixtureDb&&!registrations);
  if(scenario=="disabled"){assert(!signedBleAvailable());return 0;}
  assert(signedBleAvailable());mt::ble::callbacks.connected(&original,0);secure(original);
  assert(mt::ble::writeTo(&original,nullptr,"x",1,0,0)==1);
  mt::ble::Packet consumed;assert(mt::ble::receive(consumed));
  mt::ble::setIngressOpen(false);mockDeferWork=true;signedOta::rejectImage=peerLag()||scenario=="signature"||scenario=="signature-async"||scenario=="signature-full-async"||scenario=="terminal-disconnect"||scenario=="terminal-timeout";
  assert(!parameterRequests);parameterPeer=&update;
  if(scenario=="params-refused")parameterResult=-EACCES;
  serveSignedBle(hash,pump);
  assert(parameterRequests==(replies.empty()?0u:1u));
  assert(registrations==1&&removals==1&&!fixtureDb&&pumps>1&&!atomic_get(&armed));
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
  assert(leaseOpens==(scenario=="android-key-retry"?0u:1u));
  if(scenario=="lease-late"){assert(leaseToken);mt::ble::store::settle(leaseToken,0);}
  else assert(!leaseToken);
#endif
  if(scenario=="terminal-local-timeout")mt::ble::callbacks.disconnected(&update,0x13);
  assert(original.references==1&&update.references==1&&unrelated.references==1);
  if(timing::enabled()){timing::verify();return 0;}
  if(scenario=="recycle-busy")assert(pressure>1&&pressure<700);
  if(scenario=="wrong-advertiser")assert(pressure==1);
  if(scenario=="recycle")assert(pressure==4);
  const bool success=scenario=="android-key-retry"||scenario=="recycle"||scenario=="lease-first"||scenario=="lease-rx"||scenario=="lease-ack"||scenario=="first-write"||scenario=="success"||scenario=="params-refused"||scenario=="reconnect"||scenario=="budget";assert(signedOta::selected==success);
  if(success){
    if(scenario=="budget")assert(pressure>=8);
    assert(replies.size()==5&&replies.back()=="OK\n"&&signedOta::stored.size()==24);
    for(unsigned i=0;i<24;++i)assert(signedOta::stored[i]==i);
#if defined(TEST_STOCK_PROCESSOR)
    compareStock(replies);
#endif
  }else if(peerLag()||scenario=="signature"||scenario=="signature-async")assert(replies.back()=="ERR Hash Mismatch\n"&&signedOta::stored.size()==24);
  else if(scenario=="signature-full-async")assert(replies.back()=="ERR Hash Mismatch or Invalid Signed Image\n"&&signedOta::stored.size()==24);
  else if(scenario=="syntax-async")assert(replies.back()=="ERR Invalid Format\n"&&signedOta::stored.empty());
  else if(scenario=="terminal-disconnect")assert(replies.back()=="ACK\n" && k_uptime_get()-errorQueuedAt<500);
  else if(scenario=="terminal-timeout")assert(replies.back()=="ACK\n" && k_uptime_get()-errorQueuedAt>=2000 && k_uptime_get()-errorQueuedAt<2200);
  else if(scenario=="disconnect"||scenario=="downgrade"||scenario.rfind("lease-",0)==0)assert(signedOta::stored.size()==20);
  else assert(signedOta::stored.empty());
  puts(argv[1]);
}
