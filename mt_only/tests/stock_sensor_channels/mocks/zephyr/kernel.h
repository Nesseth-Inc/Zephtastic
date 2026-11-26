#pragma once
#include <cassert>
namespace mockKernel {inline unsigned timerTicks=0;inline unsigned long long ms=0;inline void (*accessoryThread)(void*,void*,void*)=nullptr;inline void (*scd4xAdminThread)(void*,void*,void*)=nullptr;inline void (*oneWireThread)(void*,void*,void*)=nullptr;struct NoJob{};}
#define K_MUTEX_DEFINE(name) bool name=false
#define K_SEM_DEFINE(name,n,m) unsigned name=n
#define K_THREAD_DEFINE(name,size,fn,a,b,c,p,o,d) const int name=(mockKernel::name=fn,0)
#define K_FOREVER -1
#define K_NO_WAIT 0
using atomic_t=int;using atomic_val_t=int;
#define ATOMIC_INIT(n) n
inline int atomic_inc(atomic_t *v){return (*v)++;}
inline int atomic_get(const atomic_t *v){return *v;}
inline int atomic_set(atomic_t *v,int n){auto old=*v;*v=n;return old;}
inline bool atomic_cas(atomic_t *v,int expected,int n){if(*v!=expected)return false;*v=n;return true;}
inline void k_sem_give(unsigned *s){++*s;}
inline int k_sem_take(unsigned *s,int wait){if(*s){--*s;return 0;}if(wait>0&&mockKernel::timerTicks){--mockKernel::timerTicks;mockKernel::ms+=wait*1000;return -1;}throw mockKernel::NoJob{};}
#define K_SECONDS(x) (x)
inline void k_msleep(unsigned ms){mockKernel::ms+=ms;}
inline unsigned long long k_uptime_get(){return mockKernel::ms;}
inline int k_mutex_lock(bool *m,int wait){if(*m){assert(wait==K_NO_WAIT);return -1;}*m=true;return 0;}
inline void k_mutex_unlock(bool *m){assert(*m);*m=false;}
