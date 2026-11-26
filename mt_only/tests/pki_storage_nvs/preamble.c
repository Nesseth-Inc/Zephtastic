// Compiled with the complete pinned NVS engine and exact settings functions.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/drivers/flash.h>
#include "bridge.h"
#define CONTAINER_OF(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define NVS_NAMECNT_ID 0x8000
#define NVS_NAME_ID_OFFSET 0x4000
#define SETTINGS_MAX_NAME_LEN 80
#define SETTINGS_EXTRA_LEN 16
#define __maybe_unused __attribute__((unused))
typedef ssize_t (*settings_read_cb)(void *,void *,size_t);
typedef int (*settings_load_direct_cb)(const char *,size_t,settings_read_cb,void *,void *);
struct settings_load_arg {const char *subtree;int (*cb)(const char*,size_t,settings_read_cb,void*,void*);void *param;};
struct settings_store;
struct settings_store_itf {
 int (*csi_load)(struct settings_store*,const struct settings_load_arg*);
 ssize_t (*csi_load_one)(struct settings_store*,const char*,char*,size_t);
 ssize_t (*csi_get_val_len)(struct settings_store*,const char*);
 int (*csi_save)(struct settings_store*,const char*,const char*,size_t);
 void *(*csi_storage_get)(struct settings_store*);
};
struct settings_store {int cs_next;const struct settings_store_itf *cs_itf;};
struct settings_nvs {struct settings_store cf_store;struct nvs_fs cf_nvs;uint16_t last_name_id;const struct device *flash_dev;};
struct settings_nvs_read_fn_arg {struct nvs_fs *fs;uint16_t id;};
struct default_param {void *buf;size_t buf_len;size_t *val_len;};
static struct settings_store *source,*settings_save_dst;
#define SYS_SLIST_FOR_EACH_CONTAINER(list,item,field) for(item=source;item;item=NULL)
static struct settings_nvs backend;
static struct k_mutex settings_mutex;
int model_thread=1;
int model_mutex_lock(struct k_mutex *m,int timeout){
 if(m->depth && m->owner!=model_thread){assert(timeout==K_NO_WAIT);return -EBUSY;}
 m->owner=model_thread;++m->depth;return 0;
}
int model_mutex_unlock(struct k_mutex *m){assert(m->owner==model_thread&&m->depth>0);if(!--m->depth)m->owner=0;return 0;}
static void settings_lock_take(void){assert(!backend.cf_nvs.nvs_lock.depth);assert(!model_mutex_lock(&settings_mutex,K_FOREVER));}
static void settings_lock_release(void){assert(!model_mutex_unlock(&settings_mutex));}
static size_t settings_name_next(const char *name,const char **next){*next=NULL;return name?strlen(name):0;}
static int settings_call_set_handler(const char *name,size_t n,settings_read_cb cb,void *ctx,const void *opaque){
 const struct settings_load_arg *arg=opaque;size_t length=strlen(arg->subtree);
 if(strncmp(name,arg->subtree,length)||(name[length]&&name[length]!='/'))return 0;
 return arg->cb(name[length]?name+length+1:NULL,n,cb,ctx,arg->param);
}
static uint8_t flash_bytes[32768],snapshot[32768];
static const struct device dev={0};
static const struct flash_parameters parameters={4,255};
static unsigned sectors,write_calls,read_calls,flash_read_calls,events,cut_at,erase_calls;
static int cut_phase,read_error;static uint16_t read_id;
const struct flash_parameters *flash_get_parameters(const struct device *d){assert(d==&dev);return &parameters;}
size_t flash_get_write_block_size(const struct device *d){assert(d==&dev);return 4;}
int flash_get_page_info_by_offs(const struct device *d,off_t o,struct flash_pages_info *i){assert(d==&dev);*i=(struct flash_pages_info){o-o%4096,4096,(size_t)o/4096};return 0;}
static void bound(off_t o,size_t n){assert(o>=0&&(size_t)o+n<=sectors*4096);}
int flash_read(const struct device *d,off_t o,void *p,size_t n){assert(d==&dev);bound(o,n);++flash_read_calls;memcpy(p,flash_bytes+o,n);return 0;}
int flash_write(const struct device *d,off_t o,const void *p,size_t n){
 assert(d==&dev);bound(o,n);assert(!(o%4)&&!(n%4));++events;++write_calls;
 int hit=cut_at&&events==cut_at;if(hit&&cut_phase==0)return -EIO;
 size_t count=hit&&cut_phase==1?(n/2/4)*4:n;const uint8_t *b=p;
 for(size_t i=0;i<count;++i){assert((flash_bytes[o+i]&b[i])==b[i]);flash_bytes[o+i]&=b[i];}
 return hit?-EIO:0;
}
int flash_flatten(const struct device *d,off_t o,size_t n){
 assert(d==&dev);bound(o,n);assert(!(o%4096)&&!(n%4096));++events;++write_calls;++erase_calls;
 int hit=cut_at&&events==cut_at;if(hit&&cut_phase==0)return -EIO;
 memset(flash_bytes+o,255,hit&&cut_phase==1?n/2:n);return hit?-EIO:0;
}
