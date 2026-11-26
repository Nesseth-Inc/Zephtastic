#include "BleSettingsAdmission.h"
#include "BleSettingsServiceWindow.h"
#include "Service.h"
#include "known_channel_fixture.h"
#include <zephyr/kernel.h>
#include <cassert>
#include <future>
#include <string>
#include <atomic>
#include <iostream>
using namespace mt::ble;
extern "C" int __wrap_settings_save_one(const char*,const void*,size_t);
extern "C" int __wrap_settings_delete(const char*);
struct Radio:mt::Radio {
 int state=0,pauseError=0,restoreError=0;bool busyNow=false,late=false;unsigned pauses=0;
 bool ready()override{return true;}int start()override{if(restoreError)return restoreError;state=1;return 0;}
 int stop()override{state=0;return 0;}int restore()override{state=1;return 0;}
 int stopForStorage()override{++pauses;if(late)clockOffset+=4000;return pauseError?pauseError:stop();}
 int health()override{return state;}bool busy()override{return busyNow;}uint32_t airtime(size_t)override{return 100;}
 int send(uint8_t*,size_t)override{return 0;}bool completion(int&)override{return false;}
};
static std::recursive_mutex settingsMutex;
static std::atomic<bool> blockActual{false},ioStarted{false};
static std::atomic<unsigned> calls{0};static int writeResult=0;static Radio *activeRadio=nullptr;static bool allowRf=false;
extern "C" int __real_settings_save_one(const char*,const void*,size_t){std::lock_guard<std::recursive_mutex> l(settingsMutex);assert(allowRf||!activeRadio||activeRadio->state==0);++calls;ioStarted=true;while(blockActual)std::this_thread::sleep_for(std::chrono::milliseconds(1));return writeResult;}
extern "C" int __real_settings_delete(const char*n){std::lock_guard<std::recursive_mutex> l(settingsMutex);return __wrap_settings_save_one(n,nullptr,0);}
struct Fixture {
 Radio radio;mt::Service service;mt::test::KnownChannelFixtureClock clock;
 BleSettingsServiceWindow<mt::Service,Radio> window;
 Fixture(bool armed=true):service(radio,true,0x12345678,127,true,false),window(service,radio){
  clock.bind(service);mt::Config c;c.bytes[4]=1;c.setTxEnabled(true);auto node=c.node();node.setPublic();c.node(node);
  assert(service.apply(c)&&service.start(false,0)&&service.armEndpoint(armed));activeRadio=&radio;
 }
};
static void boot(){store::initialize();assert(store::beginBoot(true));assert(store::prepareBootCommit());store::bootLoadComplete(0);store::endBoot(0);store::sealBootIfQuiet(false);assert(store::payloadAllowed());}
static uint32_t waitToken(){for(unsigned i=0;i<1000;++i){if(auto t=store::pendingToken())return t;std::this_thread::sleep_for(std::chrono::milliseconds(1));}assert(false);return 0;}
int main(int argc,char **argv){assert(argc==2);std::string test=argv[1];
 if(test=="session"){boot();store::sessionEntered();assert(__wrap_settings_save_one("bt/keys/synthetic",nullptr,0)==-EDEADLK);store::sessionLeft();assert(!calls&&store::failed()&&!store::payloadAllowed());}
 else if(test=="strict"){store::initialize();assert(store::beginBoot(true));assert(__wrap_settings_delete("bt/keys/synthetic")==-EACCES);store::bootLoadComplete(-EINVAL);store::endBoot(-EINVAL);store::sealBootIfQuiet(false);assert(!calls&&store::failed()&&!store::payloadAllowed());}
 else if(test=="device_reset"){
  store::initialize();assert(!store::beginDeviceResetErase(false)&&!store::payloadAllowed());
  assert(store::beginDeviceResetErase(true)&&!store::beginDeviceResetErase(true));
  assert(!__wrap_settings_delete("bt/keys/synthetic")&&!__wrap_settings_delete("mtble/gatt/descriptor"));
  store::endBoot(0);assert(calls==2&&!store::failed()&&!store::payloadAllowed());
  assert(store::beginDeviceResetErase(true));store::endBoot(0);
  assert(store::beginBoot(true)&&store::prepareBootCommit());store::bootLoadComplete(0);store::endBoot(0);
  store::sealBootIfQuiet(false);assert(store::payloadAllowed()&&!store::beginDeviceResetErase(true));
 }
 else if(test=="identity"){store::initialize();assert(store::beginBoot(true));assert(store::prepareBootCommit());store::bootLoadComplete(0);assert(!__wrap_settings_save_one("bt/id",nullptr,0));store::endBoot(0);store::sealBootIfQuiet(true);assert(!store::payloadAllowed());store::sealBootIfQuiet(false);assert(store::payloadAllowed()&&calls==1);}
 else if(test=="timeout"){boot();auto job=std::async(std::launch::async,[]{return __wrap_settings_save_one("bt/keys/synthetic",nullptr,0);});auto old=waitToken();assert(job.get()==-ETIMEDOUT);assert(!store::grant(old)&&!calls&&store::failed());store::sealBootIfQuiet(false);assert(!store::payloadAllowed());}
 else if(test=="overlap"||test=="slot_timeout") {
  boot();Fixture f;blockActual=true;
  auto first=std::async(std::launch::async,[]{return __wrap_settings_save_one("bt/keys/synthetic",nullptr,0);});
  auto firstToken=waitToken();assert(f.window.pollLocked(uint64_t(k_uptime_get())));f.window.grantUnlocked();
  while(!ioStarted)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  auto second=std::async(std::launch::async,[]{return __wrap_settings_save_one("bt/ccc/synthetic",nullptr,0);});
  std::this_thread::sleep_for(std::chrono::milliseconds(20));assert(calls==1&&store::active()&&!store::failed());
  if(test=="slot_timeout"){assert(second.get()==-ETIMEDOUT&&calls==1&&store::active()&&store::failed());}
  blockActual=false;assert(!first.get());assert(!f.window.pollLocked(uint64_t(k_uptime_get()))&&f.service.receiving());
  if(test=="overlap") {const auto next=waitToken();assert(next!=firstToken&&calls==1);assert(f.window.pollLocked(uint64_t(k_uptime_get())));f.window.grantUnlocked();assert(!second.get());assert(!f.window.pollLocked(uint64_t(k_uptime_get()))&&calls==2&&store::payloadAllowed());}
  else assert(!store::payloadAllowed());
 }
 else {
  boot();Fixture f(test!="unarmed");
  if(test=="native_busy")f.radio.busyNow=true;
  if(test=="late_grant")f.radio.late=true;
  if(test=="raced_rx")f.radio.pauseError=-EAGAIN;
  if(test=="hard_pause")f.radio.pauseError=-ENODEV;
  if(test=="restore_failure")f.radio.restoreError=-EIO;
  if(test=="cancel_running")blockActual=true;
  if(test.rfind("io_",0)==0)writeResult=-std::stoi(test.substr(3));
  auto job=std::async(std::launch::async,[&]{return test=="nested_delete"?__wrap_settings_delete("bt/ccc/synthetic"):__wrap_settings_save_one("bt/keys/synthetic",nullptr,0);});
  auto token=waitToken();assert(!store::payloadAllowed());
  if(test=="native_busy"||test=="raced_rx"){
   assert(!f.window.pollLocked(uint64_t(k_uptime_get()))&&!calls&&f.service.receiving());
   f.radio.busyNow=false;f.radio.pauseError=0;
  }
  if(test=="mt_before_grant"){
   allowRf=true;assert(!__wrap_settings_save_one("mtble/state0",nullptr,0));allowRf=false;assert(calls==1);
  }
  bool granted=f.window.pollLocked(uint64_t(k_uptime_get()));if(granted){std::this_thread::sleep_for(std::chrono::milliseconds(15));assert(!calls||test=="mt_before_grant");}if(granted)assert(__wrap_settings_save_one("mtble/state0",nullptr,0)==-EBUSY);if(test=="cancel_prepared_immediate")store::fail(-EIO);if(test=="cancel_prepared"){clockOffset+=4000;std::this_thread::sleep_for(std::chrono::milliseconds(10));}f.window.grantUnlocked();store::wake();
  if(test=="cancel_running"){while(!ioStarted)std::this_thread::sleep_for(std::chrono::milliseconds(1));assert(!store::cancel(token,-ECANCELED)&&store::active()&&f.window.pollLocked(uint64_t(k_uptime_get())));blockActual=false;}
  if(test=="cancel_prepared_immediate"){assert(granted&&!store::active()&&!store::pendingToken()&&!calls);assert(!f.window.pollLocked(uint64_t(k_uptime_get()))&&f.service.receiving());assert(job.get()!=0);allowRf=true;assert(!__wrap_settings_save_one("mtble/state0",nullptr,0)&&calls==1);allowRf=false;}
  else if(test=="cancel_prepared"){assert(granted&&job.get()!=0&&!calls);assert(!f.window.pollLocked(uint64_t(k_uptime_get()))&&f.service.receiving()&&f.service.endpointArmed());assert(store::failed());}
  else if(test=="late_grant"||test=="hard_pause"){
   assert(!granted&&job.get()!=0&&!calls&&store::failed());assert(!store::grant(token));
   if(test=="late_grant")assert(f.service.receiving()&&f.service.endpointArmed());
  }else{
   assert(granted);assert(job.get()==writeResult);assert(store::active());
   // Every main writer is held until the real result and exact RF restoration.
   assert(f.window.pollLocked(uint64_t(k_uptime_get()))==false);
   assert(!store::active());
   if(test=="restore_failure"){assert(store::failed()&&!f.service.receiving());}
   else{assert(f.service.receiving()&&f.service.endpointArmed()==(test!="unarmed"));}
   if(writeResult){assert(store::failed());store::sealBootIfQuiet(false);assert(!store::payloadAllowed());}
   else if(test!="restore_failure")assert(store::payloadAllowed());
   assert(calls==(test=="mt_before_grant"?2u:1u));
  }
 }
 std::cout<<"PASS "<<test<<"\n";
}
