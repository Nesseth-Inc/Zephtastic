// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrPsaThreading.h"
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <mbedtls/threading.h>

#if !defined(MBEDTLS_THREADING_C) || !defined(MBEDTLS_THREADING_ALT)
#error "Every PSA translation unit must use the shared ALT configuration"
#endif
#if !defined(CONFIG_MT_SHARED_MBEDTLS_THREADING) || !defined(CONFIG_MULTITHREADING) || !defined(CONFIG_MBEDTLS_INIT)
#error "This integration requires shared Mbed TLS threading and ordered Zephyr initialization"
#endif
BUILD_ASSERT(CONFIG_KERNEL_INIT_PRIORITY_DEFAULT > 39,
             "Threading must install before Zephyr's automatic PSA initialization");

static unsigned installed;

static void require(bool condition)
{
    // Several pinned PSA getters ignore mutex return codes. Returning an error
    // here would allow unlocked shared-state access; never continue on failure.
    if (!condition) {
        k_panic();
    }
}

static void context(const void *object)
{
    require(object != NULL && !k_is_in_isr());
}

static int mutex_init(mbedtls_platform_mutex_t *mutex)
{
    context(mutex);
    require(k_mutex_init(mutex) == 0);
    return 0;
}

static void mutex_destroy(mbedtls_platform_mutex_t *mutex)
{
    context(mutex);
    // Zephyr mutexes have no dynamically allocated backing to destroy. The
    // library owns the lifetime contract; no free while another user is live.
    require(mutex->owner == NULL && mutex->lock_count == 0);
}

static int mutex_lock(mbedtls_platform_mutex_t *mutex)
{
    context(mutex);
    // A finite failed acquisition terminally stops. Scheduler delay can extend
    // wall-clock return time; this is not cancellation of a PSA operation.
    require(k_mutex_lock(mutex, K_MSEC(1000)) == 0);
    require(mutex->owner == k_current_get() && mutex->lock_count == 1);
    return 0;
}

static int mutex_unlock(mbedtls_platform_mutex_t *mutex)
{
    context(mutex);
    require(k_mutex_unlock(mutex) == 0);
    return 0;
}

static int cond_init(mbedtls_platform_condition_variable_t *cond)
{
    context(cond);
    require(k_condvar_init(cond) == 0);
    return 0;
}

static void cond_destroy(mbedtls_platform_condition_variable_t *cond)
{
    context(cond);
    // No heap backing. The Mbed TLS caller must ensure no remaining waiters.
}

static int cond_signal(mbedtls_platform_condition_variable_t *cond)
{
    context(cond);
    require(k_condvar_signal(cond) == 0);
    return 0;
}

static int cond_broadcast(mbedtls_platform_condition_variable_t *cond)
{
    context(cond);
    // Zephyr returns the number of woken threads, not just zero.
    require(k_condvar_broadcast(cond) >= 0);
    return 0;
}

static int cond_wait(mbedtls_platform_condition_variable_t *cond,
                     mbedtls_platform_mutex_t *mutex)
{
    context(cond);
    context(mutex);
    // Native atomic release/wait/reacquire semantics. No hard wall-clock bound:
    // Zephyr's reacquisition itself uses K_FOREVER, even with finite wait input.
    // The pinned configured PSA algorithms currently do not call this API.
    require(mutex->owner == k_current_get() && mutex->lock_count == 1);
    require(k_condvar_wait(cond, mutex, K_FOREVER) == 0);
    require(mutex->owner == k_current_get() && mutex->lock_count == 1);
    return 0;
}

static int mt_psa_threading_install(void)
{
    unsigned expected = 0;
    require(__atomic_compare_exchange_n(&installed, &expected, 1, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
    mbedtls_threading_set_alt(mutex_init, mutex_destroy, mutex_lock, mutex_unlock,
                             cond_init, cond_destroy, cond_signal, cond_broadcast,
                             cond_wait);
    __atomic_store_n(&installed, 2, __ATOMIC_RELEASE);
    return 0;
}

bool mt_psa_threading_ready(void)
{
    return __atomic_load_n(&installed, __ATOMIC_ACQUIRE) == 2;
}

// PSA auto-init is POST_KERNEL40 in both pinned profiles. Run with the kernel
// available, before PSA or Bluetooth consumers. No reinstallation or teardown.
SYS_INIT(mt_psa_threading_install, POST_KERNEL, 39);
