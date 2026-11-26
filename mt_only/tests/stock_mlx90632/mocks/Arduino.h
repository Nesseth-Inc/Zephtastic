// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cmath>
#include <cstdint>
using boolean = bool;
using std::isnan;
#define F(s) s
#define HEX 16
struct Stream {
  template <typename... T> void print(T...) {}
  template <typename... T> void println(T...) {}
};
inline Stream Serial;
inline void delay(unsigned) {}
