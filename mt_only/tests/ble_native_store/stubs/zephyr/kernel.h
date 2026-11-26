// Test-only kernel contract doubles. Real threads; no Zephyr scheduler/hardware.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <cerrno>
using k_tid_t=void*;
inline thread_local int threadMarker=0;
inline thread_local unsigned kernelMutexDepth=0;
inline k_tid_t k_current_get(){return &threadMarker;}
inline std::atomic<int64_t> clockOffset{0};
inline int64_t k_uptime_get(){static const auto boot=std::chrono::steady_clock::now();return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-boot).count()+clockOffset.load();}
inline uint32_t k_uptime_get_32(){return uint32_t(k_uptime_get());}
struct k_mutex {std::recursive_mutex value;};
#define K_MUTEX_DEFINE(name) k_mutex name
#define K_FOREVER (-1)
#define K_NO_WAIT 0
#define K_MSEC(ms) (ms)
inline int k_mutex_lock(k_mutex*p,int timeout){
 if(timeout==K_NO_WAIT){if(!p->value.try_lock())return -EBUSY;}else p->value.lock();
 ++kernelMutexDepth;return 0;
}
inline int k_mutex_unlock(k_mutex*p){--kernelMutexDepth;p->value.unlock();return 0;}
struct k_sem {std::mutex lock;std::condition_variable cv;unsigned count=0;};
#define K_SEM_DEFINE(name,initial,limit) k_sem name
inline int k_sem_take(k_sem*p,int ms){std::unique_lock<std::mutex> l(p->lock);if(!p->cv.wait_for(l,std::chrono::milliseconds(ms),[&]{return p->count;}))return -EAGAIN;--p->count;return 0;}
inline void k_sem_give(k_sem*p){std::lock_guard<std::mutex> l(p->lock);p->count=1;p->cv.notify_one();}
