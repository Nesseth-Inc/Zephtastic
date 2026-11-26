// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <zephyr/kernel.h>
#include <sys_malloc.h>
#include <stdio.h>
#include "KernelHeapStats.h"
namespace mt {
// Single main-thread caller only. USB requests a sample; poll runs outside
// serviceLock and interrupt/callback context. No allocator reset or probing.
class RuntimeResources {
  bool pending_=false,valid_=false;
  uint32_t at_=0; int stackError_=-1,heapError_=-1,kernelError_=-1;
  size_t stackFree_=0; sys_memory_stats heap_{},kernel_{};
public:
  void poll() {
    if(!pending_)return;
    pending_=false;
    size_t free=0;sys_memory_stats stats{};
    stackError_=k_thread_stack_space_get(k_current_get(),&free);
    heapError_=malloc_runtime_stats_get(&stats);
    kernelError_=kernelHeapStats(kernel_);
    stackFree_=stackError_?0:free;heap_=heapError_?sys_memory_stats{}:stats;
    at_=k_uptime_get_32();valid_=true;
  }
  size_t request(char *out,size_t capacity) {
    pending_=true;
    if(!valid_||!out||!capacity)return 0;
    const int n=snprintf(out,capacity,
      "ZT-RES at_ms=%u main_stack_err=%d main_stack_free=%zu libc_err=%d libc_free=%zu libc_used=%zu libc_peak=%zu kernel_err=%d kernel_free=%zu kernel_used=%zu kernel_peak=%zu",
      unsigned(at_),stackError_,stackFree_,heapError_,heap_.free_bytes,heap_.allocated_bytes,heap_.max_allocated_bytes,kernelError_,kernel_.free_bytes,kernel_.allocated_bytes,kernel_.max_allocated_bytes);
    return n>0&&size_t(n)<capacity?size_t(n):0;
  }
};
}
