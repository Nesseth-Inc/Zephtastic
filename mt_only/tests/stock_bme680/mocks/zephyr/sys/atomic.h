#pragma once
using atomic_t=int;
#define ATOMIC_INIT(v) v
inline int atomic_get(const atomic_t *p){return *p;}
inline void atomic_set(atomic_t *p,int v){*p=v;}
