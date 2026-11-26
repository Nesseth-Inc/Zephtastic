#pragma once
#include <chrono>
#include <thread>
#include <cstdint>
#define K_MSEC(v) v
inline int64_t k_uptime_get(){using namespace std::chrono;return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();}
inline void k_sleep(int ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
