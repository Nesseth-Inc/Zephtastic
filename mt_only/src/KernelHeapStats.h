// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_heap.h>
#include <errno.h>
// Diagnostic-only adapter for pinned Zephyr 1ee3b93134be23c16ab16b9eb4183bfef63447a6.
// k_heap members and _system_heap are internal, not a portable public API.
// Match kernel/mempool.c's lock around its system-heap operations. Copy only;
// never allocate, reset peaks, format output, or call back while locked.
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS) && CONFIG_HEAP_MEM_POOL_SIZE > 0
extern "C" { extern struct k_heap _system_heap; }
#endif
namespace mt {
inline int kernelHeapStats(sys_memory_stats &out) {
  out={};
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS) && CONFIG_HEAP_MEM_POOL_SIZE > 0
  sys_memory_stats snapshot{};
  const auto key=k_spin_lock(&_system_heap.lock);
  const int error=sys_heap_runtime_stats_get(&_system_heap.heap,&snapshot);
  k_spin_unlock(&_system_heap.lock,key);
  if(!error)out=snapshot;
  return error;
#else
  return -ENOTSUP;
#endif
}
}
