// Actual full SDK GATT/Settings bodies are generated into exact_*.inc by run.py.
#include "BleSettingsAdmission.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <string>
#include <vector>
#include <future>
#define CONFIG_BT_SETTINGS 1
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define LOG_HEXDUMP_DBG(...) ((void)0)
#ifdef __weak
#undef __weak
#endif
#define __weak __attribute__((weak))
using settings_read_cb=int(*)(void*,void*,size_t);
using settings_load_direct_cb=int(*)(const char*,size_t,settings_read_cb,void*,void*);
struct settings_load_arg {const char *subtree=nullptr;settings_load_direct_cb cb=nullptr;void *param=nullptr;};
struct settings_handler_static {int(*h_set)(const char*,size_t,settings_read_cb,void*);};
struct settings_store;
struct settings_store_itf {int(*csi_load)(settings_store*,const settings_load_arg*);};
struct settings_store {settings_store_itf *cs_itf;int cs_next;int result;};
static std::vector<settings_store*> settings_load_srcs;
#define SYS_SLIST_FOR_EACH_CONTAINER(list,cs,next) for(size_t index=0;index<(list)->size()&&((cs)=(*(list))[index],true);++index)
static thread_local unsigned settingsDepth=0;
static unsigned backendReads=0,handlerReads=0,commits=0,writes=0,writeAttempts=0,directCallbacks=0;
static int handlerError=0,writeError=0,commitError=0;
static bool cleanupAttempt=false,ignoreCleanup=false,validated=false;
static void settings_lock_take(){assert(settingsDepth==0);++settingsDepth;}
static void settings_lock_release(){assert(settingsDepth==1);--settingsDepth;}
static int settings_commit_subtree(const char*);
static bool settings_name_steq(const char*n,const char*p,const char**next){auto len=strlen(p);bool same=!strncmp(n,p,len)&&(n[len]=='/'||!n[len]);if(next)*next=same?n+len+(n[len]=='/'):n;return same;}
extern "C" int __wrap_settings_save_one(const char*,const void*,size_t);
extern "C" int __wrap_settings_delete(const char*);
extern "C" int zt_settings_prepare_commit(const char*);
extern "C" int __real_settings_save_one(const char *name,const void *p,size_t n){
 assert(!strcmp(name,"bt/hash")&&p&&n==16);assert(settingsDepth==1&&backendReads==2&&handlerReads==2&&validated&&commits==1);
 assert(!mt::ble::store::payloadAllowed()&&!mt::ble::store::healthy());++writeAttempts;if(writeError)return writeError;++writes;return 0;
}
extern "C" int __real_settings_delete(const char*){assert(false);return -EIO;}
static int handler(const char*,size_t,settings_read_cb,void*){
 assert(settingsDepth==1);++handlerReads;if(handlerError)return handlerError;
 if(cleanupAttempt){const int result=__wrap_settings_delete("bt/keys/synthetic");assert(result==-EACCES||result==-EIO);return ignoreCleanup?0:result;}
 return 0;
}
static settings_handler_static h{handler};
static settings_handler_static *settings_parse_and_lookup(const char*,const char**next){*next="hash";return &h;}
#if defined(BASELINE)
// The unmodified pre-fix loader has no hook. This definition is never called
// by that loader; it only satisfies deliberately unreachable test branches.
extern "C" int zt_settings_prepare_commit(const char*){return 0;}
#endif
#include "exact_settings.inc"
static int load(settings_store*cs,const settings_load_arg*arg){assert(settingsDepth==1);++backendReads;if(cs->result)return cs->result;const int result=settings_call_set_handler("bt/hash",16,nullptr,nullptr,arg);if(backendReads==2&&!result)validated=true;return result;}
enum {DB_HASH_VALID, DB_HASH_LOAD, DB_HASH_LOAD_PROC, SC_RANGE_CHANGED, SC_LOAD, SC_INDICATE_PENDING};
struct {atomic_t flags[1];int work;} gatt_sc;
struct {uint8_t hash[16],stored_hash[16];} db_hash;
static bool atomic_test_bit(atomic_t*p,unsigned b){return (p->load()&(1u<<b))!=0;}
static void atomic_set_bit(atomic_t*p,unsigned b){p->fetch_or(1u<<b);}
static void atomic_clear_bit(atomic_t*p,unsigned b){p->fetch_and(~(1u<<b));}
static int k_work_cancel_delayable(int*){return 0;}
#define SC_TIMEOUT 1
static void sc_indicate(uint16_t,uint16_t){}
static void set_all_change_unaware(){}
static void db_hash_gen(){memset(db_hash.hash,0x11,sizeof(db_hash.hash));atomic_set_bit(gatt_sc.flags,DB_HASH_VALID);}
static int bt_settings_store_hash(const void*p,size_t n){return __wrap_settings_save_one("bt/hash",p,n);}
#include "exact_gatt.inc"
static int settings_commit_subtree(const char *subtree){assert(settingsDepth==1&&validated&&backendReads==2);++commits;if(commitError)return commitError;return subtree&&!strcmp(subtree,"bt")?db_hash_commit():0;}
static int direct(const char*,size_t,settings_read_cb,void*,void*){++directCallbacks;return 0;}
int main(int argc,char**argv){assert(argc==2);const std::string test=argv[1];using namespace mt::ble;
 settings_store_itf itf{load};settings_store a{&itf,0,0},b{&itf,0,0};settings_load_srcs={&a,&b};store::initialize();
 if(test=="runtime_hook"){assert(settings_load_subtree("bt")==-EACCES&&!commits&&!writes&&store::failed());}
 else if(test=="direct_no_commit"){
  assert(store::beginBoot(true));assert(!settings_load_subtree_direct("bt",direct,nullptr)&&directCallbacks==2&&!commits&&!writes);assert(!zt_settings_prepare_commit("bt"));assert(!store::payloadAllowed()&&!store::healthy());store::bootLoadComplete(0);store::endBoot(0);store::sealBootIfQuiet(false);assert(store::payloadAllowed());
 }else if(test=="wrong_subtree"){
  assert(store::beginBoot(true));assert(!zt_settings_prepare_commit(nullptr)&&!zt_settings_prepare_commit("mtble")&&!zt_settings_prepare_commit("bt/keys"));assert(!zt_settings_prepare_commit("bt"));assert(zt_settings_prepare_commit("bt")==-EACCES&&store::failed()&&!writes);
 }else if(test=="missing_prepare"){
  assert(store::beginBoot(true));store::bootLoadComplete(0);store::endBoot(0);store::sealBootIfQuiet(false);assert(store::failed()&&!store::payloadAllowed()&&!writes);
 }else{
  assert(store::beginBoot(true));
  if(test=="matching")memset(db_hash.stored_hash,0x11,sizeof(db_hash.stored_hash));
  if(test=="changed")memset(db_hash.stored_hash,0x22,sizeof(db_hash.stored_hash));
  if(test=="backend_first")a.result=-EIO;
  if(test=="backend_second")b.result=-EIO;
  if(test=="handler_failure")handlerError=-EINVAL;
  if(test=="cleanup_failure"||test=="cleanup_ignored"){cleanupAttempt=true;ignoreCleanup=test=="cleanup_ignored";}
  if(test=="commit_failure")commitError=-EIO;
  if(test=="write_failure")writeError=-EIO;
  int result=0;
  if(test=="worker_hook"){auto job=std::async(std::launch::async,[]{return settings_load_subtree("bt");});result=job.get();}
  else result=settings_load_subtree("bt");
  assert(settingsDepth==0&&!store::payloadAllowed()&&!store::healthy());
  const bool noCommit=test=="backend_first"||test=="backend_second"||test=="handler_failure"||test=="cleanup_failure"||test=="cleanup_ignored"||test=="worker_hook";
  if(noCommit)assert(result!=0&&!commits&&!writeAttempts&&!writes);
  else if(test=="commit_failure")assert(result==-EIO&&commits==1&&!writeAttempts&&!writes);
#if defined(BASELINE)
  else assert(!result&&commits==1&&!writeAttempts&&!writes&&store::failed());
#else
  else if(test=="write_failure")assert(!result&&commits==1&&writeAttempts==1&&!writes&&store::failed());
  else assert(!result&&commits==1&&writes==(test=="matching"?0u:1u)&&!store::failed());
#endif
  store::bootLoadComplete(result);assert(!store::payloadAllowed()&&!store::healthy());store::endBoot(result);store::sealBootIfQuiet(true);assert(!store::payloadAllowed());store::sealBootIfQuiet(false);
  const bool failure=noCommit||test=="commit_failure"||test=="write_failure"
#if defined(BASELINE)
   ||true
#endif
   ;assert(store::payloadAllowed()==!failure&&store::healthy()==!failure&&store::failed()==failure);
 }
 printf("PASS actual-settings-gatt %s reads=%u commit=%u writes=%u blocked=%u\n",test.c_str(),backendReads,commits,writes,unsigned(store::failed()));
}
