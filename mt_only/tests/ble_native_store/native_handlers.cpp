// Host-only actual SDK function composition; synthetic bytes only, no device I/O.
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <limits>
#include <sys/types.h>
#ifndef CONFIG_BT_ID_MAX
#define CONFIG_BT_ID_MAX 1
#endif
#define CONFIG_BT_DEVICE_NAME_DYNAMIC 1
#ifndef CONFIG_BT_DEVICE_NAME_MAX
#define CONFIG_BT_DEVICE_NAME_MAX 29
#endif
#define CONFIG_BT_DEVICE_NAME "Default"
#define CONFIG_BT_DEVICE_APPEARANCE_DYNAMIC 1
#define CONFIG_BT_PRIVACY 1
#define CONFIG_BT_CLASSIC 0
#define IS_ENABLED(c) (c)
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define LOG_HEXDUMP_DBG(...) ((void)0)
#define BT_DEV_OPEN 0
#define BT_DEV_PRESET_ID 1
#define BT_DEV_STORE_ID 2
#define BT_ADDR_LE_PUBLIC 0
#define BT_ADDR_LE_RANDOM 1
using settings_read_cb=ssize_t(*)(void*,void*,size_t);
using settings_load_direct_cb=int(*)(const char*,size_t,settings_read_cb,void*,void*);
struct bt_addr_t {uint8_t val[6];};
struct bt_addr_le_t {uint8_t type;bt_addr_t a;};
static_assert(sizeof(bt_addr_le_t)==7,"Native public address shape used by source");
struct Device {unsigned flags[1];bt_addr_le_t id_addr[CONFIG_BT_ID_MAX];uint8_t id_count;char name[CONFIG_BT_DEVICE_NAME_MAX+1];uint16_t appearance;uint8_t irk[CONFIG_BT_ID_MAX][16];};
static Device bt_dev;
static unsigned generated=0,idStores=0,irkStores=0,commits=0,finalized=0,reads=0,dispatches=0,locks=0,checks=0;
static bool atomic_test_bit(unsigned *p,unsigned bit){return (*p&(1u<<bit))!=0;}
static bool atomic_test_and_clear_bit(unsigned*p,unsigned bit){bool old=atomic_test_bit(p,bit);*p&=~(1u<<bit);return old;}
static int settings_name_next(const char*n,const char**next){const auto p=strchr(n,'/');if(next)*next=p?p+1:nullptr;return p?int(p-n):int(strlen(n));}
static int bt_br_write_local_name(const char*){return 0;}
static int bt_setup_public_id_addr(){++generated;bt_dev.id_count=1;bt_dev.id_addr[0]={{},{{}}};bt_dev.flags[0]|=1u<<BT_DEV_STORE_ID;return 0;}
static int bt_setup_random_id_addr(){return 0;}
static int bt_settings_store_id(){++idStores;return 0;}
static int bt_settings_store_irk(){++irkStores;return 0;}
static void bt_finalize_init(int){++finalized;}
struct settings_load_arg {const char *subtree=nullptr;settings_load_direct_cb cb=nullptr;void *param=nullptr;};
struct settings_handler_static {int(*h_set)(const char*,size_t,settings_read_cb,void*);};
struct settings_store;
struct settings_store_itf {int(*csi_load)(settings_store*,const settings_load_arg*);};
struct settings_store {settings_store_itf *cs_itf;int cs_next=0;};
static std::vector<settings_store*> settings_load_srcs;
#define SYS_SLIST_FOR_EACH_CONTAINER(list,cs,next) for(size_t index=0;index<(list)->size()&&((cs)=(*(list))[index],true);++index)
static void settings_lock_take(){++locks;}
static void settings_lock_release(){assert(locks);--locks;}
static bool settings_name_steq(const char*n,const char*p,const char**next){auto len=strlen(p);bool same=!strncmp(n,p,len)&&(n[len]=='/'||!n[len]);if(next)*next=same?n+len+(n[len]=='/'):n;return same;}
static int set_setting(const char*,size_t,settings_read_cb,void*);
static int commit_settings();
static settings_handler_static handler{set_setting};
static settings_handler_static *settings_parse_and_lookup(const char *name,const char **next){if(!strncmp(name,"bt/",3)){*next=name+3;return &handler;}return nullptr;}
static int settings_commit_subtree(const char*){++commits;return commit_settings();}
// Actual complete native handler, commit, decode, hex conversion and strict
// dispatcher/load functions extracted unchanged by run.py.
// Actual native handler checks use the successful-read commit hook double.
int zt_settings_prepare_commit(const char*){return 0;}
#include "actual_native_handlers.inc"
struct Record {std::string key;size_t declared;std::vector<uint8_t> bytes;ssize_t result;};
static std::vector<Record> records;
static ssize_t readRecord(void *ctx,void *dest,size_t cap){++reads;auto &r=*static_cast<Record*>(ctx);size_t count=cap<r.bytes.size()?cap:r.bytes.size();if(count)memcpy(dest,r.bytes.data(),count);return r.result;}
static int sourceLoad(settings_store*,const settings_load_arg*arg){for(auto &r:records){++dispatches;int result=settings_call_set_handler(r.key.c_str(),r.declared,readRecord,&r,arg);if(result)return result;}return 0;}
static settings_store_itf itf{sourceLoad};static settings_store store{&itf};
static void reset(bool existing=true){memset(&bt_dev,0x51,sizeof(bt_dev));bt_dev.flags[0]=1u<<BT_DEV_OPEN;bt_dev.id_count=existing?1:0;if(!existing){memset(bt_dev.id_addr,0,sizeof(bt_dev.id_addr));memset(bt_dev.name,0,sizeof(bt_dev.name));memset(bt_dev.irk,0,sizeof(bt_dev.irk));}bt_dev.name[sizeof(bt_dev.name)-1]=0;generated=idStores=irkStores=commits=finalized=reads=dispatches=locks=0;records.clear();settings_load_srcs={&store};}
static void rejected(const char *key,size_t declared,ssize_t result,int expected,bool shouldRead){for(bool existing:{false,true}){reset(existing);Record r{key,declared,std::vector<uint8_t>(512,0x42),result};records={r};const Device before=bt_dev;const auto disk=records[0].bytes;const int actual=settings_load_subtree("bt");assert(actual==expected);assert(!memcmp(&before,&bt_dev,sizeof(bt_dev)));assert(!commits&&!generated&&!idStores&&!irkStores&&!finalized&&!locks);assert(reads==unsigned(shouldRead));assert(records[0].bytes==disk);++checks;}}

static void valid(const char*key,size_t length){reset();std::vector<uint8_t> bytes(length,0x42);records={{key,length,bytes,ssize_t(length)}};assert(!settings_load_subtree("bt"));assert(commits==1&&!generated&&!idStores&&!irkStores&&finalized==1&&!locks&&reads==1);if(!strcmp(key,"bt/id")){assert(bt_dev.id_count==length/sizeof(bt_addr_le_t));assert(!memcmp(bt_dev.id_addr,bytes.data(),length));}else if(!strcmp(key,"bt/irk")){assert(!memcmp(bt_dev.irk,bytes.data(),length));}else if(!strcmp(key,"bt/name")){assert(!memcmp(bt_dev.name,bytes.data(),length)&&!bt_dev.name[length]);}else{assert(!memcmp(&bt_dev.appearance,bytes.data(),length));}assert(records[0].bytes==bytes);++checks;}
static void fieldCases(){
 const size_t addr=sizeof(bt_addr_le_t),idCap=sizeof(bt_dev.id_addr),irkCap=sizeof(bt_dev.irk),nameCap=sizeof(bt_dev.name)-1;
 for(auto key:{"bt/id","bt/name","bt/irk","bt/appearance"}){
  size_t exact=!strcmp(key,"bt/id")?addr:!strcmp(key,"bt/name")?size_t(5):!strcmp(key,"bt/irk")?size_t(16):size_t(2);
  for(int error:{-EIO,-ENOSPC,-EACCES,-EINVAL})rejected(key,exact,error,error,true);
  for(size_t count=0;count<exact;++count)rejected(key,exact,ssize_t(count),-EIO,true);
  rejected(key,exact,ssize_t(exact+1),-EIO,true);
 }
 for(size_t length=0;length<=idCap+addr;++length)if(!length||length>idCap||length%addr)rejected("bt/id",length,ssize_t(length),-EINVAL,false);else valid("bt/id",length);
 for(size_t length=0;length<=irkCap+16;++length)if(!length||length>irkCap||length%16)rejected("bt/irk",length,ssize_t(length),-EINVAL,false);else valid("bt/irk",length);
 for(size_t length:{nameCap+1,nameCap+2,std::numeric_limits<size_t>::max()})rejected("bt/name",length,0,-EINVAL,false);
 for(size_t length:{size_t(0),size_t(1),size_t(3),std::numeric_limits<size_t>::max()})rejected("bt/appearance",length,0,-EINVAL,false);
 valid("bt/name",1);valid("bt/name",nameCap);valid("bt/appearance",2);
 // Native empty-name semantics are retained. An empty stored name is valid,
 // and commit chooses the configured default; it never creates an identity.
 reset();records={{"bt/name",0,{},0}};assert(!settings_load_subtree("bt")&&commits==1&&!generated&&!idStores&&!irkStores);assert(!strcmp(bt_dev.name,CONFIG_BT_DEVICE_NAME));++checks;
 // Genuine absence does not invoke the handler. Native fresh-identity creation
 // is allowed only through this missing-record path, not invalid-present data.
 reset(false);assert(!settings_load_subtree("bt"));assert(!dispatches&&!reads&&commits==1&&generated==1&&idStores==1&&irkStores==1);++checks;
 // A present valid identity with absent optional keys does not queue replacement.
 reset(false);records={{"bt/id",addr,std::vector<uint8_t>(addr,0x42),ssize_t(addr)}};assert(!settings_load_subtree("bt")&&commits==1&&dispatches==1&&!generated&&!idStores&&!irkStores);++checks;
 // Native IRK and ID records can arrive in either backend handle order.
 for(bool reverse:{false,true}){reset(false);Record id{"bt/id",idCap,std::vector<uint8_t>(idCap,0x42),ssize_t(idCap)};Record irk{"bt/irk",irkCap,std::vector<uint8_t>(irkCap,0x73),ssize_t(irkCap)};Record name{"bt/name",4,std::vector<uint8_t>(4,0x55),4};records=reverse?std::vector<Record>{irk,name,id}:std::vector<Record>{id,name,irk};const auto disk=records;assert(!settings_load_subtree("bt")&&commits==1&&reads==3&&!generated&&!idStores&&!irkStores);assert(bt_dev.id_count==CONFIG_BT_ID_MAX&&!memcmp(bt_dev.id_addr,id.bytes.data(),idCap)&&!memcmp(bt_dev.irk,irk.bytes.data(),irkCap)&&!memcmp(bt_dev.name,name.bytes.data(),4));for(size_t i=0;i<records.size();++i)assert(records[i].bytes==disk[i].bytes);++checks;}
 // Actual backend errors also remain terminal before native commit.
 reset();const auto before=bt_dev;settings_store_itf failItf{[](settings_store*,const settings_load_arg*){return -EIO;}};settings_store bad{&failItf};settings_load_srcs={&bad};assert(settings_load_subtree("bt")==-EIO&&!commits&&!memcmp(&before,&bt_dev,sizeof(before)));++checks;
}
static void decodeCases(){
 bt_addr_le_t addr{};
 for(const char *key:{"0011223344550","0011223344551","a1b2c3d4e5f60","A1B2C3D4E5F60","0011223344550/0"}){assert(!bt_settings_decode_key(key,&addr));++checks;}
 assert(addr.type==0&&addr.a.val[0]==0x55&&addr.a.val[5]==0x00);
 for(size_t i=0;i<12;++i)for(char invalid:{'g','?',char(0x80)}){std::string key="0011223344550";key[i]=invalid;assert(bt_settings_decode_key(key.c_str(),&addr)==-EINVAL);++checks;}
 for(size_t length=0;length<13;++length){std::string key="0011223344550";key.resize(length);assert(bt_settings_decode_key(key.c_str(),&addr)==-EINVAL);++checks;}
 for(const char *key:{"0011223344552","001122334455a","00112233445500","001122/344550","00112233445/0"}){assert(bt_settings_decode_key(key,&addr)==-EINVAL);++checks;}
}
int main(){fieldCases();decodeCases();printf("PASS native handler/decode/strict commit cases=%u id_slots=%u name_max=%u\n",checks,unsigned(CONFIG_BT_ID_MAX),unsigned(CONFIG_BT_DEVICE_NAME_MAX));}
