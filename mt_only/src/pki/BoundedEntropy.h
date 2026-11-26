// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiLedger.h"
#include "PkiSigner.h"

namespace mt::pki {
// Trusted, bounded port. acquire must be nonblocking; read must return at most
// capacity bytes, zero for temporary starvation, or a negative error. No port
// method may use an unbounded wait. ready is checked only while leased.
struct EntropyPort {
  virtual ~EntropyPort()=default;
  virtual bool acquire()=0;
  virtual bool ready()const=0;
  virtual int read(uint8_t *,size_t capacity)=0;
  virtual void pause()=0;
  virtual bool release()=0;
  virtual uint32_t micros()const=0;
};
// No DRBG, key generation, retained random pool, fallback or implicit startup.
// Caller output is unchanged on failure. The elapsed deadline is checked when
// scheduled; scheduler/interrupt latency can extend wall-clock return time.
// The independent attempt cap also terminates with a stuck clock.
class BoundedEntropy final:public EntropySource,public SigningEntropy {
  EntropyPort &port_;
  uint32_t entered_=0;
  bool poisoned_=false;
public:
  static constexpr size_t Capacity=32;
  static constexpr unsigned MaxReads=64;
  static constexpr uint32_t DeadlineUs=20000;
  explicit BoundedEntropy(EntropyPort &port):port_(port){}
  BoundedEntropy(const BoundedEntropy &)=delete;
  BoundedEntropy &operator=(const BoundedEntropy &)=delete;
  // A non-consuming, nonblocking readiness snapshot, never a future lease.
  bool ready();
  bool fill(uint8_t *,size_t)override;
  bool hedge(uint8_t out[32])override{return fill(out,32);}
};
}
