#pragma once
#include <cstddef>
#include <cstdint>
#include <cassert>
struct sys_heap {int marker;}; struct k_spinlock {int marker;};
struct k_heap {sys_heap heap; k_spinlock lock;};
extern "C" {extern k_heap _system_heap;}
extern int calls,stack_rc,locks,unlocks;extern bool held;
inline void *k_current_get(){return nullptr;}
inline int k_thread_stack_space_get(void*,size_t *p){assert(!held);++calls;*p=1234;return stack_rc;}
inline uint32_t k_uptime_get_32(){assert(!held);return 42;}
inline unsigned k_spin_lock(k_spinlock *p){assert(p==&_system_heap.lock&&!held);held=true;++locks;return 71;}
inline void k_spin_unlock(k_spinlock *p,unsigned key){assert(p==&_system_heap.lock&&held&&key==71);held=false;++unlocks;}
