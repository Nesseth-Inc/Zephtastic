#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <sys/types.h>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include "ZephyrSettings.h"
#define __maybe_unused __attribute__((unused))
#define MIN(a,b) std::min(a,b)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define CONTAINER_OF(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define LOG_DBG(...) do{}while(0)
#define NVS_NAMECNT_ID 0x8000
#define NVS_NAME_ID_OFFSET 0x4000
#define SETTINGS_MAX_NAME_LEN 80
#define SETTINGS_EXTRA_LEN 16
#define CONFIG_SETTINGS_NVS_NAME_CACHE_SIZE 8
#define SETTINGS_NVS_CACHE_OVFL(cf) ((cf)->cache_total > ARRAY_SIZE((cf)->cache))
using settings_read_cb=ssize_t (*)(void *,void *,size_t);
struct settings_load_arg {const char *subtree;int (*cb)(const char*,size_t,settings_read_cb,void*,void*);void *param;};
struct settings_store;
struct settings_store_itf {
 int (*csi_load)(settings_store*,const settings_load_arg*);
 ssize_t (*csi_load_one)(settings_store*,const char*,char*,size_t);
 ssize_t (*csi_get_val_len)(settings_store*,const char*);
 int (*csi_save)(settings_store*,const char*,const char*,size_t);
};
struct settings_store {int cs_next;const settings_store_itf *cs_itf;};
struct nvs_fs {const void *flash_device;};
struct settings_nvs {settings_store cf_store;nvs_fs cf_nvs;uint16_t last_name_id;const void *flash_dev;struct {uint16_t name_hash,name_id;}cache[8];uint16_t cache_next,cache_total;bool loaded;};
struct settings_nvs_read_fn_arg {nvs_fs *fs;uint16_t id;};
static std::map<uint16_t,std::vector<uint8_t>> records;
static std::map<uint16_t,int> readErrors;
static int writes,deletes,mountError,writeError,deleteError;static bool writeAfterCommit;
static uint16_t shortId;static int shortCount=-1;
ssize_t nvs_read(nvs_fs *,uint16_t id,void *dst,size_t cap) {
 if(readErrors.count(id))return readErrors[id];
 auto it=records.find(id);if(it==records.end())return -ENOENT;
 size_t n=it->second.size();if(shortCount>=0&&id==shortId)n=shortCount;
 const size_t copy=std::min(cap,n);if(copy)memcpy(dst,it->second.data(),copy);return n;
}
ssize_t nvs_write(nvs_fs *,uint16_t id,const void *data,size_t n) {++writes;if(writeError&&!writeAfterCommit)return writeError;const auto *p=(const uint8_t*)data;records[id]={p,p+n};return writeError?writeError:ssize_t(n);}
int nvs_delete(nvs_fs *,uint16_t id){++deletes;if(deleteError)return deleteError;records.erase(id);return 0;}
int nvs_mount(nvs_fs *){return mountError;}
uint16_t crc16_ccitt(uint16_t c,const uint8_t *p,size_t n){while(n--)c=uint16_t(c*31+*p++);return c;}
uint16_t crc16_ccitt(uint16_t c,const char *p,size_t n){return crc16_ccitt(c,(const uint8_t*)p,n);}
size_t settings_name_next(const char *name,const char **next){*next=nullptr;if(!name)return 0;return strlen(name);}
int settings_call_set_handler(const char *name,size_t n,settings_read_cb cb,void *ctx,const void *opaque){
 const auto *arg=static_cast<const settings_load_arg *>(opaque);
 size_t length=strlen(arg->subtree);if(strncmp(name,arg->subtree,length)|| (name[length]&&name[length]!='/'))return 0;
 return arg->cb(name[length]?name+length+1:nullptr,n,cb,ctx,arg->param);
}
void settings_lock_take(){}void settings_lock_release(){}
static settings_store *source;static settings_store *settings_save_dst;
#define SYS_SLIST_FOR_EACH_CONTAINER(list,item,field) for(item=source;item;item=nullptr)

// Rename C identifier that is a C++ keyword; source control flow is unchanged.
#define delete nvs_delete_request
