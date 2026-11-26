// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cassert>
struct k_spinlock { bool locked=false; };
using k_spinlock_key_t=bool;
inline bool k_spin_lock(k_spinlock *l){assert(!l->locked);l->locked=true;return false;}
inline void k_spin_unlock(k_spinlock *l,bool){assert(l->locked);l->locked=false;}
