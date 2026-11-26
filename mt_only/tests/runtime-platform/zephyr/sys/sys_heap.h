#pragma once
#include <sys_malloc.h>
#include <zephyr/kernel.h>
extern int kernel_rc,kernel_calls;
inline int sys_heap_runtime_stats_get(sys_heap *h,sys_memory_stats *p){assert(held&&h==&_system_heap.heap);++kernel_calls;*p={200,40,80};return kernel_rc;}
