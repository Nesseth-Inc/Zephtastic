// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

namespace mt::modules {
// Wire pin IDs belong to an explicit board export table. A capability bit
// never implies that the same numeric controller offset may be accessed.
struct GpioCapabilities {
  uint64_t exported=0,readable=0,writable=0,pullup=0;
};
class GpioModuleBackend {
public:
  virtual ~GpioModuleBackend()=default;
  virtual GpioCapabilities capabilities()const=0;
  virtual int configureInput(uint32_t wirePin,bool pullup)=0;
  // Validate the complete mask before any I/O. Negative results are failures,
  // including driver errors; they never establish a completed operation.
  virtual int read(uint64_t mask,bool pullup,uint64_t &value)=0;
  virtual int write(uint64_t mask,uint64_t value)=0;
};
// Current boards export no pins. A later reviewed board overlay may opt in
// through the dedicated GPIO-module binding, independently of built-in aliases.
GpioModuleBackend &zephyrGpioModuleBackend();
}
