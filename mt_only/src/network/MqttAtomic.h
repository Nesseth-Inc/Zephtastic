// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

#if defined(__ZEPHYR__)
#include <zephyr/sys/atomic.h>
#else
#include <atomic>
#endif

namespace mt::mqtt {

enum class AtomicOrder : uint8_t { Relaxed, Acquire, Release, Sequential };

// The transport only needs atomic loads and stores. Zephyr's atomic_get/set
// provide full barriers, which are stronger than the acquire/release orders
// used here. The host implementation preserves the requested order for tests.
template <typename T> class Atomic {
 public:
#if defined(__ZEPHYR__)
  static_assert(sizeof(T) <= sizeof(atomic_val_t),
                "Zephyr atomic adapter supports values no wider than atomic_val_t");
#endif
  constexpr Atomic(T value = T{})
#if defined(__ZEPHYR__)
      : value_(static_cast<atomic_t>(value)) {}
#else
      : value_(value) {}
#endif
  Atomic(const Atomic &) = delete;
  Atomic &operator=(const Atomic &) = delete;

  T load(AtomicOrder order = AtomicOrder::Sequential) const {
#if defined(__ZEPHYR__)
    (void)order;
    return static_cast<T>(atomic_get(&value_));
#else
    return value_.load(order == AtomicOrder::Relaxed ? std::memory_order_relaxed :
                       order == AtomicOrder::Acquire ? std::memory_order_acquire :
                                                       std::memory_order_seq_cst);
#endif
  }

  void store(T value, AtomicOrder order = AtomicOrder::Sequential) {
#if defined(__ZEPHYR__)
    (void)order;
    (void)atomic_set(&value_, static_cast<atomic_val_t>(value));
#else
    value_.store(value, order == AtomicOrder::Relaxed ? std::memory_order_relaxed :
                        order == AtomicOrder::Release ? std::memory_order_release :
                                                        std::memory_order_seq_cst);
#endif
  }

  Atomic &operator=(T value) { store(value); return *this; }
  operator T() const { return load(); }

 private:
#if defined(__ZEPHYR__)
  atomic_t value_;
#else
  std::atomic<T> value_;
#endif
};

}  // namespace mt::mqtt
