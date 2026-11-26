#include <cassert>
#include <cstddef>
#include <vector>
#ifdef __weak
#undef __weak
#endif
#define __weak __attribute__((weak))
struct settings_load_arg{const char *subtree;};
struct settings_store;
struct settings_store_itf{int(*csi_load)(settings_store*,const settings_load_arg*);};
struct settings_store{settings_store_itf *cs_itf;int cs_next;};
static std::vector<settings_store*> settings_load_srcs;
#define SYS_SLIST_FOR_EACH_CONTAINER(list,cs,next) for(size_t index=0;index<(list)->size()&&((cs)=(*(list))[index],true);++index)
static unsigned locks=0,commits=0,reads=0;
static void settings_lock_take(){assert(!locks);++locks;}
static void settings_lock_release(){assert(locks==1);--locks;}
static int settings_commit_subtree(const char*){assert(locks==1&&reads>0);++commits;return 0;}
#include "exact_weak.inc"
static int load(settings_store*,const settings_load_arg*){++reads;return 0;}
int main(){settings_store_itf itf{load};settings_store s{&itf,0};settings_load_srcs={&s};assert(!zt_settings_prepare_commit(nullptr)&&!zt_settings_prepare_commit("bt")&&!zt_settings_prepare_commit("mtble"));assert(!settings_load_subtree("bt")&&!settings_load_subtree("mtble")&&!settings_load_subtree(nullptr)&&commits==3&&reads==3&&!locks);}
