#include "BleTransport.cpp"
#include <future>
#include <thread>
#include <string>
#include <cstdio>
extern "C" int __wrap_settings_save_one(const char*,const void*,size_t);
extern "C" int __wrap_settings_delete(const char*);
static int nativeWriteResult=0;static unsigned nativeWrites=0;
extern "C" int __real_settings_save_one(const char*,const void*,size_t){++nativeWrites;return nativeWriteResult;}
extern "C" int __real_settings_delete(const char*n){return __wrap_settings_save_one(n,nullptr,0);}
extern "C" bool zt_bt_settings_identity_work_pending(){return mockIdentityPending;}
struct bt_keys {uint8_t id=0;bt_addr_le_t addr=mockPeer;uint8_t storage_start[1]{};unsigned state=0;};
#define BT_KEYS_STORAGE_LEN 1
#define BT_KEYS_ID_ADDED 1
static void bt_id_del(bt_keys*){}
static int bt_settings_delete_keys(uint8_t,const bt_addr_le_t*){return __wrap_settings_delete("bt/keys/synthetic");}
#define __ASSERT_NO_MSG(v) assert(v)
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
static int bt_settings_store_keys(uint8_t,const bt_addr_le_t*,const void*p,size_t n){return __wrap_settings_save_one("bt/keys/synthetic",p,n);}
// This function body is extracted unchanged from proposed SDK keys.c by run_tests.py.
#include "actual_key_store.inc"
using namespace mt::ble;
static void initialize(bool pin){store::initialize();assert(store::beginBoot(true));mt::BluetoothSettings cfg;if(pin){cfg.mode=mt::BluetoothSettings::FixedPin;cfg.fixedPin=0;}const int result=init(123,cfg,true);store::endBoot(result);}
static void attach(bt_conn&c){onConnected(&c,0);}
static void secure(bt_conn&c,bt_security_t level){c.level=level;onSecurityChanged(&c,level,BT_SECURITY_ERR_SUCCESS);}
static void detach(bt_conn&c){onDisconnected(&c,19);assert(c.refs==1);}
static int write(bt_conn&c){return writeTo(&c,nullptr,"x",1,0,0);}
static int nativeStore(bt_keys &keys){auto job=std::async(std::launch::async,[&]{return bt_keys_store(&keys);});uint32_t token=0;for(unsigned i=0;i<1000&&!token;++i){token=store::pendingToken();if(!token)std::this_thread::sleep_for(std::chrono::milliseconds(1));}assert(token&&store::prepare(token)&&store::grant(token));store::wake();int result=job.get();uint32_t done=0;int terminal=0;assert(store::completed(done,terminal)&&done==token);store::settle(done,0);return result;}
static void nativeErase(bt_keys &keys){auto job=std::async(std::launch::async,[&]{bt_keys_clear(&keys);});uint32_t token=0;for(unsigned i=0;i<1000&&!token;++i){token=store::pendingToken();if(!token)std::this_thread::sleep_for(std::chrono::milliseconds(1));}assert(token&&store::prepare(token)&&store::grant(token));store::wake();job.get();uint32_t done=0;int terminal=0;assert(store::completed(done,terminal)&&done==token);store::settle(done,0);}
#include "gatt_reconnect.h"
int main(int argc,char **argv){(void)&readFrom;(void)&readNum;(void)&ccc;assert(argc==2);std::string test=argv[1];
 if(test=="restored_ccc_peripheral" || test=="restored_ccc_central" ||
    test=="restored_ccc_absent" || test=="restored_ccc_missing_key"){
  gatt_reconnect::check(test);
 }
 else if(test=="nopin_ephemeral_clear"){
  initialize(false);poll();bt_conn c;attach(c);bt_keys temporary;nativeErase(temporary);
  assert(sessionAuthorized&&!disconnectRequested&&bondAcceptance.allowed()&&!store::failed());
  secure(c,BT_SECURITY_L2);onPairingComplete(&c,false);poll();assert(write(c)==1&&!mockDisconnects);
 }
 else if(test=="nopin_known_clear"){
  mockBootBond=true;initialize(false);poll();bt_conn c;attach(c);secure(c,BT_SECURITY_L2);assert(durablePeerKnown(&c.peer));bt_keys old;nativeErase(old);
  assert(!durablePeerKnown(&c.peer)&&sessionAuthorized&&!disconnectRequested&&write(c)==1);poll();assert(!mockDisconnects);
 }
 else if(test=="fresh_pin_ephemeral_clear"){
  initialize(true);poll();bt_conn c;attach(c);bt_keys temporary;nativeErase(temporary);
  assert(sessionAuthorized&&!disconnectRequested&&!bondAcceptance.allowed()&&!store::failed());
  secure(c,BT_SECURITY_L3);onPairingComplete(&c,true);assert(write(c)<0);bt_keys replacement;assert(!nativeStore(replacement));poll();assert(write(c)==1&&!mockDisconnects);
 }
 else if(test=="nopin_clear_failure"){
  initialize(false);poll();bt_conn c;attach(c);secure(c,BT_SECURITY_L2);nativeWriteResult=-EIO;bt_keys temporary;nativeErase(temporary);
  assert(store::failed()&&write(c)<0);poll();assert(mockDisconnects>0);
 }
 else if(test=="five_peers"){mockBootBondCount=5;initialize(true);poll();assert(ready());bt_conn c;c.peer.bytes[0]=5;attach(c);secure(c,BT_SECURITY_L3);assert(write(c)==1);}
 else if(test=="overflow_peers"){mockBootBondCount=6;initialize(true);poll();assert(!ready()&&store::failed()&&!mockAdvertising);}
 else if(test=="boot_failure"){mockLoadResult=-EIO;initialize(true);poll();assert(!ready()&&!mockAdvertising&&store::failed());}
 else if(test=="queued_identity"){mockIdentityPending=true;initialize(true);poll();assert(!ready()&&!mockAdvertising);mockIdentityPending=false;poll();assert(ready()&&mockAdvertising);}
 else if(test=="admin_healthy_store"){
  initialize(false);poll();bt_conn c;attach(c);secure(c,BT_SECURITY_L2);const auto epoch=session();
  const uint8_t framed[]={0x94,0xc3,0,1,0x42};assert(beginAdmin(epoch)&&stageAdminResult(framed,sizeof(framed),epoch));
  assert(finishAdminConfiguration(mt::BluetoothSettings{},true,epoch,true));
  auto job=std::async(std::launch::async,[]{return __wrap_settings_save_one("bt/ccc/synthetic",nullptr,0);});
  uint32_t token=0;for(unsigned i=0;i<1000&&!token;++i){token=store::pendingToken();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  assert(token&&!store::payloadAllowed()&&store::healthy());uint8_t out[4]{};
  assert(readFrom(&c,nullptr,out,sizeof(out),0)==1&&out[0]==0x42&&!adminSecurityInvalid);
  assert(store::prepare(token)&&store::grant(token));store::wake();assert(!job.get());uint32_t done=0;int result=0;assert(store::completed(done,result));store::settle(done,0);assert(!adminSecurityInvalid);
 }
 else if(test=="nopin"){initialize(false);poll();bt_conn c;attach(c);assert(c.requested==BT_SECURITY_L2&&!c.bondable&&!c.auth->passkey_display);secure(c,BT_SECURITY_L2);assert(write(c)==1&&!nativeWrites);}
 else if(test=="resolved_after_connect"){mockBootBond=true;initialize(true);poll();bt_conn c;c.peer.bytes[0]=99;attach(c);secure(c,BT_SECURITY_L3);assert(write(c)<0);c.peer=mockPeer;onIdentityResolved(&c,nullptr,&c.peer);assert(write(c)==1&&!nativeWrites);}
 else if(test=="deleted_peer_repair"){
  mockBootBond=true;initialize(true);poll();bt_conn c;attach(c);secure(c,BT_SECURITY_L3);assert(write(c)==1);bt_keys keys;nativeErase(keys);assert(write(c)<0);detach(c);poll();bt_conn next;attach(next);secure(next,BT_SECURITY_L3);assert(write(next)<0);onPairingComplete(&next,true);assert(write(next)<0);bt_keys replacement;assert(!nativeStore(replacement));assert(write(next)==1);
 }
 else if(test=="delete_failure"){
  mockBootBond=true;initialize(true);poll();bt_conn c;attach(c);secure(c,BT_SECURITY_L3);bt_keys keys;nativeWriteResult=-EIO;nativeErase(keys);assert(store::failed()&&write(c)<0);detach(c);poll();bt_conn next;attach(next);secure(next,BT_SECURITY_L3);onPairingComplete(&next,true);assert(write(next)<0&&!ready());
 }
 else if(test=="loaded_reconnect"){mockBootBond=true;initialize(true);poll();bt_conn c;attach(c);assert(c.bondable&&c.requested==BT_SECURITY_L3);secure(c,BT_SECURITY_L3);assert(write(c)==1&&!nativeWrites);detach(c);poll();bt_conn next;attach(next);secure(next,BT_SECURITY_L3);assert(write(next)==1&&!nativeWrites);}
 else {
  initialize(true);poll();bt_conn c;attach(c);secure(c,BT_SECURITY_L3);assert(write(c)<0);bt_keys keys;
  if(test=="unbonded_pair"){onPairingComplete(&c,false);assert(write(c)<0);}
  else if(test=="new_pair"){onPairingComplete(&c,true);assert(write(c)<0);assert(!nativeStore(keys));assert(write(c)==1&&nativeWrites==1);detach(c);poll();bt_conn next;attach(next);secure(next,BT_SECURITY_L3);assert(write(next)==1&&nativeWrites==1);}
  else if(test=="store_before_pair"){assert(!nativeStore(keys));assert(write(c)<0);onPairingComplete(&c,true);assert(write(c)==1);}
  else if(test=="wrong_peer"){keys.addr.bytes[0]^=1;assert(!nativeStore(keys));onPairingComplete(&c,true);assert(write(c)<0);}
  else if(test=="session_store_rejected"){SessionLock lock;assert(bt_keys_store(&keys)==-EDEADLK);assert(!nativeWrites&&store::failed());}
  else if(test=="failed_store_reconnect"){
   const auto old=session();nativeWriteResult=-EIO;assert(nativeStore(keys)==-EIO);onPairingComplete(&c,true);assert(write(c)<0);detach(c);poll();bt_conn next;attach(next);secure(next,BT_SECURITY_L3);onPairingComplete(&next,true);zt_bt_key_store_end(0,&next.peer,old,0);assert(write(next)<0&&store::failed()&&!ready());
  } else assert(false);
 }
 std::printf("PASS actual transport %s\n",test.c_str());
}
