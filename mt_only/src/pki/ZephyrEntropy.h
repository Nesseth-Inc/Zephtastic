// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "BoundedEntropy.h"
namespace mt::pki {
// Exact pinned nRF52840 / ESP32-S3 implementation. No constructor I/O. The port
// is an implementation detail; production callers use ZephyrEntropy below.
class ZephyrEntropyPort final:public EntropyPort {
  bool held_=false;
  void *phyLock_=nullptr;
  uint32_t cpuHz_=0,apbHz_=0;
public:
  ZephyrEntropyPort()=default;
  ZephyrEntropyPort(const ZephyrEntropyPort &)=delete;
  ZephyrEntropyPort &operator=(const ZephyrEntropyPort &)=delete;
  bool acquire()override;
  bool ready()const override;
  int read(uint8_t *,size_t)override;
  void pause()override;
  bool release()override;
  uint32_t micros()const override;
};
class ZephyrEntropy final:public EntropySource,public SigningEntropy {
  mutable ZephyrEntropyPort port_;
  mutable BoundedEntropy source_{port_};
public:
  ZephyrEntropy()=default;
  ZephyrEntropy(const ZephyrEntropy &)=delete;
  ZephyrEntropy &operator=(const ZephyrEntropy &)=delete;
  bool ready()const{return source_.ready();}
  bool fill(uint8_t *out,size_t n)override{return source_.fill(out,n);}
  bool hedge(uint8_t out[32])override{return source_.hedge(out);}
};
}
