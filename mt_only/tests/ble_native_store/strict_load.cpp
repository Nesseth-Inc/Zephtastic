#include <cassert>
#include <cstring>
#include <cerrno>
#include <vector>
#include <cstddef>
using settings_read_cb=int(*)(void*,void*,size_t);
using settings_load_direct_cb=int(*)(const char*,size_t,settings_read_cb,void*,void*);
struct settings_load_arg {const char *subtree=nullptr;settings_load_direct_cb cb=nullptr;void *param=nullptr;};
struct settings_handler_static {int(*h_set)(const char*,size_t,settings_read_cb,void*);};
struct settings_store;
struct settings_store_itf {int(*csi_load)(settings_store*,const settings_load_arg*);};
struct settings_store {settings_store_itf *cs_itf;int cs_next;int result;};
static std::vector<settings_store*> settings_load_srcs;
#define SYS_SLIST_FOR_EACH_CONTAINER(list,cs,next) for(size_t index=0;index<(list)->size()&&((cs)=(*(list))[index],true);++index)
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
static unsigned locks=0,commits=0,loads=0,prepares=0;static int handlerError=0,prepareError=0;
extern "C" int zt_settings_prepare_commit(const char*){assert(locks==1&&loads==2);++prepares;return prepareError;}
static void settings_lock_take(){++locks;}
static void settings_lock_release(){assert(locks);--locks;}
static int settings_commit_subtree(const char*){++commits;return 0;}
static bool settings_name_steq(const char*n,const char*p,const char**next){auto len=strlen(p);bool same=!strncmp(n,p,len)&&(n[len]=='/'||!n[len]);if(next)*next=same?n+len+(n[len]=='/'):n;return same;}
static int handler(const char*,size_t,settings_read_cb,void*){return handlerError;}
static settings_handler_static h{handler};
static settings_handler_static *settings_parse_and_lookup(const char*,const char**next){*next="keys/synthetic";return &h;}
// Three complete proposed SDK function bodies, extracted unchanged.
#include "actual_strict_load.inc"
static int load(settings_store*cs,const settings_load_arg*arg){++loads;if(cs->result)return cs->result;return settings_call_set_handler("bt/keys/synthetic",1,nullptr,nullptr,arg);}
int main(){settings_store_itf itf{load};settings_store a{&itf,0,0},b{&itf,0,0};settings_load_srcs={&a,&b};
 assert(!settings_load_subtree("bt")&&loads==2&&commits==1&&!locks);
 for(int error:{-EIO,-ENOSPC,-ENOMEM,-EINVAL}){
  commits=loads=prepares=0;a.result=error;handlerError=0;assert(settings_load_subtree("bt")==error&&commits==0&&loads==1&&!locks&&!prepares);
  a.result=0;handlerError=error;commits=loads=prepares=0;assert(settings_load_subtree("bt")==error&&commits==0&&loads==1&&!locks&&!prepares);
 }
 handlerError=0;a.result=-EIO;prepares=0;commits=loads=0;assert(settings_load_subtree_direct("bt",nullptr,nullptr)==-EIO&&!commits&&loads==1&&!locks);
 a.result=handlerError=0;prepareError=-EACCES;commits=loads=prepares=0;assert(settings_load_subtree("bt")==-EACCES&&loads==2&&prepares==1&&!commits&&!locks);
}
