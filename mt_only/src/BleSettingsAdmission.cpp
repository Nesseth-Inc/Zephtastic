// SPDX-License-Identifier: GPL-3.0-only
#include "BleSettingsAdmission.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

namespace mt::ble::store {
namespace {
K_MUTEX_DEFINE(mutex);
K_SEM_DEFINE(signal,0,1);
enum class Phase:uint8_t {Idle,Waiting,Prepared,Running,Done};
Phase phase=Phase::Idle;
k_tid_t mainThread=nullptr,bootOwner=nullptr,owner=nullptr,sessionOwner=nullptr;
uint32_t serial=0,token=0;
uint64_t deadline=0;
unsigned depth=0,sessionDepth=0;
int terminal=0;
bool initialized=false,strictLoad=false,loaded=false,sealed=false;
atomic_t closed=1,poison=0,health=0;
constexpr uint64_t AdmissionMs=3000;
struct Lock {Lock(){k_mutex_lock(&mutex,K_FOREVER);}~Lock(){k_mutex_unlock(&mutex);}};
void publish(){atomic_set(&health,sealed&&!atomic_get(&poison));atomic_set(&closed,atomic_get(&poison)||!sealed||phase!=Phase::Idle);}
void poisonLocked(int result){if(result)atomic_set(&poison,1);publish();}
void retire(){phase=Phase::Idle;owner=nullptr;token=0;depth=0;deadline=0;publish();}
int enter(bool deletion) {
 (void)deletion;
 const auto self=k_current_get();
 const uint64_t started=uint64_t(k_uptime_get());
 if(started>UINT64_MAX-AdmissionMs){fail(-EOVERFLOW);return -EOVERFLOW;}
 const uint64_t expires=started+AdmissionMs;
 // Other native callers wait for the single slot before any Settings lock.
 // Their arguments remain on their own stacks; no native I/O is resubmitted.
 for(;;) {
  {
   Lock lock;
   if(!initialized||atomic_get(&poison))return -EIO;
   if(sessionOwner==self&&sessionDepth){poisonLocked(-EDEADLK);return -EDEADLK;}
   if(self==bootOwner) {
    // Strict load can request invalid-key cleanup under the Settings mutex.
    // Refuse it rather than entering a new wait or modifying uncertain bytes.
    if(strictLoad){poisonLocked(-EACCES);return -EACCES;}
    return 0;
   }
   if(phase==Phase::Running&&owner==self){++depth;return 0;}
   if(self==mainThread){poisonLocked(-EDEADLK);return -EDEADLK;}
   const uint64_t now=uint64_t(k_uptime_get());
   if(now<started||now>=expires){poisonLocked(-ETIMEDOUT);return -ETIMEDOUT;}
   if(phase==Phase::Idle) {
    if(serial==UINT32_MAX){poisonLocked(-EOVERFLOW);return -EOVERFLOW;}
    token=++serial;deadline=expires;owner=self;phase=Phase::Waiting;publish();break;
   }
  }
  (void)k_sem_take(&signal,K_MSEC(5));
 }
 for(;;) {
  {
   Lock lock;
   if(owner!=self)return -EIO;
   // After grant the caller owns the operation; its arguments cannot expire.
   // Admission timeout never cancels an operation already granted.
   if(phase==Phase::Running){depth=1;return 0;}
   if(phase!=Phase::Waiting&&phase!=Phase::Prepared)return -EIO;
   const uint64_t now=uint64_t(k_uptime_get());
   if(atomic_get(&poison)||now>=deadline||now<deadline-AdmissionMs){
    const int result=atomic_get(&poison)?-EIO:-ETIMEDOUT;
    poisonLocked(result);retire();return result;
   }
  }
  (void)k_sem_take(&signal,K_MSEC(5));
 }
}
void leave(int result) {
 Lock lock;
 poisonLocked(result);
 if(k_current_get()==bootOwner)return;
 if(phase!=Phase::Running||owner!=k_current_get()||!depth){poisonLocked(-EIO);return;}
 if(result&&!terminal)terminal=result;
 if(--depth==0){phase=Phase::Done;publish();}
}
}
void initialize(){Lock lock;if(initialized)return;initialized=true;mainThread=k_current_get();publish();}
bool beginBoot(bool safe){Lock lock;if(!initialized||k_current_get()!=mainThread||!safe||bootOwner||phase!=Phase::Idle){poisonLocked(-EIO);return false;}bootOwner=mainThread;strictLoad=true;return true;}
// Pre-stack offline deletion. Payload and advertising stay closed through the
// following cold boot. Runtime grant paths cannot enter the boot owner.
bool beginDeviceResetErase(bool safe){
 Lock lock;
 if(!safe||!initialized||k_current_get()!=mainThread||bootOwner||phase!=Phase::Idle||
    loaded||sealed||atomic_get(&poison))return false;
 bootOwner=mainThread;strictLoad=false;return true;
}
bool prepareBootCommit(){
 Lock lock;
 if(!initialized||k_current_get()!=mainThread||bootOwner!=mainThread||!strictLoad||loaded||sealed||phase!=Phase::Idle||atomic_get(&poison)){
  poisonLocked(-EACCES);return false;
 }
 // Backend reads and h_set validation are complete. Native h_commit may now
 // write the GATT hash/SC state under the existing Settings mutex. Readiness
 // still waits for the whole Settings API and identity work settlement.
 strictLoad=false;return true;
}
void bootLoadComplete(int result){Lock lock;if(bootOwner!=k_current_get()){poisonLocked(-EIO);return;}if(!result&&strictLoad)result=-EACCES;strictLoad=false;loaded=result==0;poisonLocked(result);}
void endBoot(int result){Lock lock;if(bootOwner!=k_current_get()){poisonLocked(-EIO);return;}bootOwner=nullptr;strictLoad=false;poisonLocked(result);}
void sealBootIfQuiet(bool pending){Lock lock;if(k_current_get()==mainThread&&loaded&&!bootOwner&&!pending&&phase==Phase::Idle&&!atomic_get(&poison))sealed=true;publish();}
bool payloadAllowed(){return atomic_get(&closed)==0;}
bool healthy(){return atomic_get(&health)!=0;}
bool failed(){return atomic_get(&poison)!=0;}
bool waiting(){Lock lock;return phase==Phase::Waiting;}
void fail(int result){Lock lock;poisonLocked(result?result:-EIO);}
void sessionEntered(){Lock lock;const auto self=k_current_get();if(sessionDepth&&sessionOwner!=self){poisonLocked(-EDEADLK);return;}sessionOwner=self;++sessionDepth;}
void sessionLeft(){Lock lock;if(sessionOwner!=k_current_get()||!sessionDepth){poisonLocked(-EIO);return;}if(!--sessionDepth)sessionOwner=nullptr;}
int invoke(bool deletion,Operation operation,void *context){
 if(!operation){fail(-EINVAL);return -EINVAL;}
 (void)deletion;
 const int admitted=enter(deletion);if(admitted)return admitted;
 const int result=operation(context);leave(result);return result;
}
uint32_t pendingToken(){Lock lock;return phase==Phase::Waiting?token:0;}
bool prepare(uint32_t requested){Lock lock;const uint64_t now=uint64_t(k_uptime_get());if(!requested||phase!=Phase::Waiting||token!=requested||atomic_get(&poison)||now>=deadline||now<deadline-AdmissionMs)return false;phase=Phase::Prepared;publish();return true;}
bool grant(uint32_t requested){Lock lock;const uint64_t now=uint64_t(k_uptime_get());if(!requested||phase!=Phase::Prepared||token!=requested||atomic_get(&poison)||now>=deadline||now<deadline-AdmissionMs)return false;phase=Phase::Running;depth=0;terminal=0;publish();return true;}
bool cancel(uint32_t requested,int result){
 {Lock lock;if(!requested||token!=requested||(phase!=Phase::Waiting&&phase!=Phase::Prepared))return false;poisonLocked(result?result:-ECANCELED);retire();}
 wake();return true;
}
bool active(){Lock lock;return phase==Phase::Prepared||phase==Phase::Running||phase==Phase::Done;}
bool completed(uint32_t &out,int &result){Lock lock;if(phase!=Phase::Done)return false;out=token;result=terminal;return true;}
void settle(uint32_t requested,int result){Lock lock;if(phase!=Phase::Done||token!=requested){poisonLocked(-EIO);return;}poisonLocked(result);retire();}
void reject(uint32_t requested,int result){Lock lock;if(phase==Phase::Waiting&&token==requested){poisonLocked(result?result:-EIO);} }
void wake(){k_sem_give(&signal);}
}

// GNU --wrap catches host-settings.c's undefined public Settings references.
// An internal settings_delete -> settings_save_one call may bypass wrapping;
// the outer delete already owns admission. If it does wrap, invoke nests.
extern "C" int __real_settings_save_one(const char *,const void *,size_t);
extern "C" int __real_settings_delete(const char *);
namespace {
bool guarded(const char *name){return name&&(!strncmp(name,"bt/",3)||!strncmp(name,"mtble/gatt/",11));}
struct Save {const char *name;const void *value;size_t size;};
int save(void *p){const auto &s=*static_cast<Save *>(p);return __real_settings_save_one(s.name,s.value,s.size);}
int erase(void *p){return __real_settings_delete(static_cast<const char *>(p));}
}
extern "C" int __wrap_settings_save_one(const char *name,const void *value,size_t size){
 // Main excludes all its MT writers for the complete granted interval. This
 // also refuses an unexpected external MT writer before it can take Settings.
 if(!guarded(name))return mt::ble::store::active()?-EBUSY:__real_settings_save_one(name,value,size);
 Save args{name,value,size};return mt::ble::store::invoke(!value||!size,save,&args);
}
extern "C" int __wrap_settings_delete(const char *name){
 if(!guarded(name))return mt::ble::store::active()?-EBUSY:__real_settings_delete(name);
 return mt::ble::store::invoke(true,erase,const_cast<char *>(name));
}

#if defined(CONFIG_MT_BLE_NATIVE_STORE)
// Source-owned Settings seam. Other subtrees retain the weak default behavior.
extern "C" int zt_settings_prepare_commit(const char *subtree){
 if(!subtree||strcmp(subtree,"bt"))return 0;
 return mt::ble::store::prepareBootCommit()?0:-EACCES;
}
#endif
