// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace zt {
// Allocation-free availability for the scalar float/uint64 telemetry values.
// A present zero is distinct from absence. Read the value only after checking
// availability; no exception, dynamic lifetime or standard-library dependency.
template<class T> class Observed {
 public:
  constexpr Observed()=default;
  constexpr Observed(T value):value_(value),available_(true){}
  constexpr bool has_value()const{return available_;}
  constexpr explicit operator bool()const{return available_;}
  constexpr const T &operator*()const{return value_;}
 private:
  T value_{};
  bool available_=false;
};
}
