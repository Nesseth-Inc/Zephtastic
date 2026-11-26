// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cassert>

struct k_spinlock { bool held = false; };
using k_spinlock_key_t = unsigned;
inline k_spinlock_key_t k_spin_lock(k_spinlock *lock) {
    assert(!lock->held);
    lock->held = true;
    return 1;
}
inline void k_spin_unlock(k_spinlock *lock, k_spinlock_key_t key) {
    assert(lock->held && key == 1);
    lock->held = false;
}
