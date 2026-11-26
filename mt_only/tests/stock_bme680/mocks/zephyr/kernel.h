#pragma once
#include <cstdint>
#define K_NO_WAIT 0
#define K_FOREVER (-1LL)
#define K_MSEC(v) (int64_t(v))
#define K_SEM_DEFINE(n,a,b) int n
#define K_MUTEX_DEFINE(n) int n
#define K_THREAD_DEFINE(...)
inline int k_mutex_lock(int*,int64_t){return 0;}
inline void k_mutex_unlock(int*){}
inline void k_sem_give(int*){}
inline void k_sem_take(int*,int64_t){}
inline int64_t k_uptime_get(){return 1000;}
extern unsigned initWait;
inline void k_msleep(unsigned ms){initWait+=ms;}
