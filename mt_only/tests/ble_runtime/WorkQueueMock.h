// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cassert>
struct k_work {void (*handler)(k_work *);};
#define K_WORK_DEFINE(name,handler) static k_work name{handler}
inline bool mockDeferWork=false;
inline thread_local bool mockSystemWorkqueue=false;
inline k_work *mockPendingWork=nullptr;
inline void mockRunWork(){
  assert(mockPendingWork&&!mockSystemWorkqueue);
  auto *work=mockPendingWork;mockPendingWork=nullptr;
  mockSystemWorkqueue=true;work->handler(work);mockSystemWorkqueue=false;
}
inline int k_work_submit(k_work *work){
  if(mockPendingWork==work)return 0;
  assert(!mockPendingWork);mockPendingWork=work;
  if(!mockDeferWork)mockRunWork();
  return 1;
}
