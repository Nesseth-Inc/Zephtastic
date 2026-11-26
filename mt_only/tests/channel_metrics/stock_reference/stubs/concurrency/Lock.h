#pragma once
#include <mutex>
namespace concurrency {
class Lock {
    std::mutex mutex;
public:
    void lock() { mutex.lock(); }
    void unlock() { mutex.unlock(); }
};
}
