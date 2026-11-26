// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <errno.h>

namespace mt::pki {
// A dedicated, explicitly reserved flash region. There is deliberately no
// erase, format, mount or repair operation in this interface. Reads/programs
// return zero only after the complete requested range has been transferred.
struct AppendStore {
  virtual ~AppendStore()=default;
  virtual size_t capacity()const=0;
  virtual int read(size_t offset,uint8_t *out,size_t size)=0;
  virtual int program(size_t offset,const uint8_t *data,size_t size)=0;
  // Optional one-way lifecycle selector, never a generic overwrite facility.
  // The caller has verified both nonce records and the exhausted legacy root.
  // Production admits only CMIT -> BMIT at root offset4092, without erasure.
  virtual int selectCounterMode(){return -ENOTSUP;}
};

// The immutable legacy snapshot and reserved-region root are bound by the
// selector written LAST to the alternate existing NVS ledger slot. CRCs detect
// storage faults; they are not an anti-rollback or malicious-storage signature.
struct AppendAnchor {
  uint32_t generation=0,snapshotCrc=0,rootCrc=0,capacity=0;
};
}
