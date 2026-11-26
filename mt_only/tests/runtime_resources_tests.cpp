#include "RuntimeResources.h"
#include <cassert>
#include <cstring>
#include <cstdio>
int calls=0,stack_rc=0,heap_rc=0,kernel_rc=0,kernel_calls=0,locks=0,unlocks=0;bool held=false;
extern "C" {k_heap _system_heap{};}
int main(){
 mt::RuntimeResources r;char b[384];r.poll();assert(calls==0&&locks==0);
 assert(r.request(b,sizeof(b))==0&&r.request(b,sizeof(b))==0&&calls==0);
 r.poll();assert(calls==2);assert(r.request(b,sizeof(b))>0);assert(strstr(b,"main_stack_free=1234"));assert(strstr(b,"libc_free=100 libc_used=20 libc_peak=30"));
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS) && CONFIG_HEAP_MEM_POOL_SIZE > 0
 assert(locks==1&&unlocks==1&&kernel_calls==1&&!held);assert(strstr(b,"kernel_err=0 kernel_free=200 kernel_used=40 kernel_peak=80"));
 kernel_rc=-22;r.poll();assert(locks==2&&unlocks==2&&!held);assert(r.request(b,sizeof(b))>0);assert(strstr(b,"kernel_err=-22 kernel_free=0 kernel_used=0 kernel_peak=0"));assert(strstr(b,"libc_peak=30"));
 kernel_rc=0;stack_rc=-95;heap_rc=-22;r.poll();assert(r.request(b,sizeof(b))>0);assert(strstr(b,"main_stack_err=-95 main_stack_free=0"));assert(strstr(b,"libc_err=-22 libc_free=0 libc_used=0 libc_peak=0"));assert(strstr(b,"kernel_peak=80"));
#else
 assert(locks==0&&unlocks==0&&kernel_calls==0);assert(strstr(b,"kernel_err=-95 kernel_free=0 kernel_used=0 kernel_peak=0"));
#endif
 assert(r.request(b,3)==0);assert(r.request(nullptr,0)==0);assert(!held);
 puts("PASS deferred/coalesced requests, lock balance, distinct allocator values, errors cleared independently, unsupported mode and bounded wire output");
}
