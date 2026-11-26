#pragma once
#include <atomic>
using atomic_t=std::atomic<int>;
inline bool atomic_cas(atomic_t*p,int old,int value){return p->compare_exchange_strong(old,value);}
inline int atomic_get(const atomic_t*p){return p->load();}
inline int atomic_set(atomic_t*p,int n){return p->exchange(n);}
inline int atomic_inc(atomic_t*p){return p->fetch_add(1);}
