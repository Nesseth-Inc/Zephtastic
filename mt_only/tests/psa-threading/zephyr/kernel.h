// SPDX-License-Identifier: GPL-3.0-only
// Host-only POSIX semantic double. Not a target port or timing qualification.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stddef.h>
#define BUILD_ASSERT(c, m) _Static_assert(c, m)
#define K_MSEC(n) ((int64_t)(n))
#define K_FOREVER INT64_C(-1)
struct k_mutex { pthread_mutex_t native; void *owner; unsigned lock_count; };
struct k_condvar { pthread_cond_t native; };
enum { F_NONE, F_MUTEX_INIT, F_MUTEX_LOCK, F_MUTEX_UNLOCK, F_COND_INIT,
       F_COND_SIGNAL, F_COND_BROADCAST, F_COND_WAIT };
extern _Thread_local bool shim_isr;
extern int shim_fault;
extern unsigned shim_panic_mode;
extern unsigned long shim_locks, shim_unlocks, shim_waits;
void *k_current_get(void);
bool k_is_in_isr(void);
_Noreturn void k_panic(void);
int k_mutex_init(struct k_mutex *);
int k_mutex_lock(struct k_mutex *, int64_t);
int k_mutex_unlock(struct k_mutex *);
int k_condvar_init(struct k_condvar *);
int k_condvar_signal(struct k_condvar *);
int k_condvar_broadcast(struct k_condvar *);
int k_condvar_wait(struct k_condvar *, struct k_mutex *, int64_t);
