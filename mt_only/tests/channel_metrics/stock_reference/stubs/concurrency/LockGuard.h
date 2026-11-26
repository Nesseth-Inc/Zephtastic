#pragma once
#include "Lock.h"
namespace concurrency {
class LockGuard {
    Lock* lock;
public:
    explicit LockGuard(Lock* value):lock(value) { lock->lock(); }
    ~LockGuard() { lock->unlock(); }
    LockGuard(const LockGuard&)=delete;
    LockGuard&operator=(const LockGuard&)=delete;
};
}
