// SPDX-License-Identifier: GPL-3.0-only
#include <zephyr/kernel.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
_Thread_local bool shim_isr;
static _Thread_local char thread_token;
int shim_fault;
unsigned shim_panic_mode;
unsigned long shim_locks, shim_unlocks, shim_waits;
void *k_current_get(void) { return &thread_token; }
bool k_is_in_isr(void) { return shim_isr; }
_Noreturn void k_panic(void) {
    if (shim_panic_mode) _Exit(73); // expected subprocess containment
    fputs("unexpected kernel panic\n", stderr); abort();
}
int k_mutex_init(struct k_mutex *m) {
    if (shim_fault == F_MUTEX_INIT) return -ENOMEM;
    pthread_mutexattr_t a; assert(!pthread_mutexattr_init(&a));
    assert(!pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE));
    int rc = pthread_mutex_init(&m->native, &a);
    assert(!pthread_mutexattr_destroy(&a)); m->owner = NULL; m->lock_count = 0; return rc ? -rc : 0;
}
int k_mutex_lock(struct k_mutex *m, int64_t timeout) {
    assert(timeout == 1000); // policy argument, NOT a host timing measurement
    if (shim_fault == F_MUTEX_LOCK) return -EAGAIN;
    int rc = pthread_mutex_lock(&m->native); if (rc) return -rc;
    m->owner = k_current_get(); ++m->lock_count;
    __atomic_fetch_add(&shim_locks, 1, __ATOMIC_RELAXED); return 0;
}
int k_mutex_unlock(struct k_mutex *m) {
    if (shim_fault == F_MUTEX_UNLOCK) return -EPERM;
    if (m->owner != k_current_get() || !m->lock_count) return -EPERM;
    if (!--m->lock_count) m->owner = NULL;
    __atomic_fetch_add(&shim_unlocks, 1, __ATOMIC_RELAXED);
    return -pthread_mutex_unlock(&m->native);
}
int k_condvar_init(struct k_condvar *c) {
    return shim_fault == F_COND_INIT ? -ENOMEM : -pthread_cond_init(&c->native, NULL);
}
int k_condvar_signal(struct k_condvar *c) {
    return shim_fault == F_COND_SIGNAL ? -EIO : -pthread_cond_signal(&c->native);
}
int k_condvar_broadcast(struct k_condvar *c) {
    if (shim_fault == F_COND_BROADCAST) return -EIO;
    assert(!pthread_cond_broadcast(&c->native)); return 3; // legal positive wake count
}
int k_condvar_wait(struct k_condvar *c, struct k_mutex *m, int64_t timeout) {
    assert(timeout == K_FOREVER);
    if (shim_fault == F_COND_WAIT) return -EIO;
    assert(m->owner == k_current_get() && m->lock_count == 1);
    m->owner = NULL; m->lock_count = 0;
    __atomic_fetch_add(&shim_waits, 1, __ATOMIC_RELAXED);
    int rc = pthread_cond_wait(&c->native, &m->native); // atomic release and reacquire
    m->owner = k_current_get(); m->lock_count = 1; return -rc;
}
