#ifndef MT_M151_FIXED_PARTITION_BLANK_SCAN_H
#define MT_M151_FIXED_PARTITION_BLANK_SCAN_H

#include <stddef.h>
#include <stdint.h>

namespace mt {
namespace m151 {
namespace blank_scan {

constexpr uint32_t kPartitionBytes = 131072u;
constexpr size_t kChunkBytes = 64u;

enum ScanError : int32_t {
  kNoReader = 1,
  kNoProgress = 2,
  kReaderCountOutOfRange = 3,
};

// Callback reports a byte count in `actual` on success. Short positive reads
// are accepted and the scanner resumes at the next byte. A nonzero status
// aborts; `actual` is ignored when status is nonzero.
using ReadFn = int32_t (*)(void *context, uint32_t offset, uint8_t *out,
                           size_t requested, size_t *actual);

struct Result {
  uint32_t partition_bytes;
  uint32_t bytes_scanned;
  uint32_t ff_bytes;
  uint32_t non_ff_bytes;
  int32_t error;
  bool complete;

  // True means only that every observed byte in the complete fixed range was
  // 0xFF. It is not a filesystem free-space, wear, or reuse claim.
  bool all_ff() const {
    return complete && error == 0 && bytes_scanned == kPartitionBytes &&
           ff_bytes == kPartitionBytes && non_ff_bytes == 0;
  }
};

inline void wipe(uint8_t *bytes, size_t count) {
  volatile uint8_t *p = bytes;
  while (count-- != 0u) *p++ = 0;
}

// Reads exactly the fixed-size partition through one read-only callback.
// The bounded scratch buffer is wiped after each callback. This function has
// no write, erase, logging, hashing, or raw-byte return path.
inline Result scan(ReadFn reader, void *context) {
  Result result{kPartitionBytes, 0u, 0u, 0u, 0, false};
  uint8_t scratch[kChunkBytes];

  if (reader == nullptr) {
    result.error = kNoReader;
    wipe(scratch, sizeof(scratch));
    return result;
  }

  while (result.bytes_scanned < kPartitionBytes) {
    const uint32_t remaining = kPartitionBytes - result.bytes_scanned;
    const size_t requested = remaining < kChunkBytes
                                 ? static_cast<size_t>(remaining)
                                 : kChunkBytes;
    size_t actual = 0u;
    const int32_t status = reader(context, result.bytes_scanned, scratch,
                                  requested, &actual);
    if (status != 0) {
      wipe(scratch, sizeof(scratch));
      result.error = status;
      return result;
    }
    if (actual > requested) {
      wipe(scratch, sizeof(scratch));
      result.error = kReaderCountOutOfRange;
      return result;
    }
    if (actual == 0u) {
      wipe(scratch, sizeof(scratch));
      result.error = kNoProgress;
      return result;
    }

    for (size_t i = 0u; i < actual; ++i) {
      if (scratch[i] == 0xffu) {
        ++result.ff_bytes;
      } else {
        ++result.non_ff_bytes;
      }
    }
    result.bytes_scanned += static_cast<uint32_t>(actual);
    wipe(scratch, sizeof(scratch));
  }

  result.complete = result.bytes_scanned == kPartitionBytes;
  return result;
}

}  // namespace blank_scan
}  // namespace m151
}  // namespace mt

#endif  // MT_M151_FIXED_PARTITION_BLANK_SCAN_H
