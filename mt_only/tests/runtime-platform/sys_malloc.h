#pragma once
#include <cstddef>
#include <cassert>
struct sys_memory_stats {size_t free_bytes,allocated_bytes,max_allocated_bytes;};
extern int calls,heap_rc;extern bool held;
inline int malloc_runtime_stats_get(sys_memory_stats *p){assert(!held);++calls;*p={100,20,30};return heap_rc;}
