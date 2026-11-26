#include "storage/FixedPartitionBlankScan.h"

#include <assert.h>
#include <algorithm>
#include <stdint.h>
#include <stdio.h>

namespace {
using namespace mt::m151::blank_scan;

struct ReaderState {
  uint32_t max_read = 0u;
  uint32_t calls = 0u;
  uint32_t non_ff_offset = kPartitionBytes;
  uint32_t error_offset = kPartitionBytes;
  size_t max_return = kChunkBytes;
  int32_t read_error = -17;
};

int32_t read_fixture(void *context, uint32_t offset, uint8_t *out,
                     size_t requested, size_t *actual) {
  ReaderState *state = static_cast<ReaderState *>(context);
  ++state->calls;
  state->max_read = std::max(state->max_read,
                             static_cast<uint32_t>(requested));
  if (offset >= state->error_offset) {
    *actual = 0u;
    return state->read_error;
  }
  const size_t amount = std::min(requested, state->max_return);
  if (offset + amount > state->error_offset) {
    *actual = 0u;
    return state->read_error;
  }
  for (size_t i = 0u; i < amount; ++i) {
    const uint32_t at = offset + static_cast<uint32_t>(i);
    out[i] = at == state->non_ff_offset ? 0x00u : 0xffu;
  }
  *actual = amount;
  return 0;
}

int32_t no_progress(void *, uint32_t, uint8_t *, size_t, size_t *actual) {
  *actual = 0u;
  return 0;
}

int32_t overreport(void *, uint32_t, uint8_t *, size_t requested,
                   size_t *actual) {
  *actual = requested + 1u;
  return 0;
}

void test_partial_reads_complete_blank() {
  ReaderState state;
  state.max_return = 13u;
  const Result r = scan(read_fixture, &state);
  assert(r.complete && r.error == 0 && r.all_ff());
  assert(r.partition_bytes == kPartitionBytes);
  assert(r.bytes_scanned == kPartitionBytes);
  assert(r.ff_bytes == kPartitionBytes && r.non_ff_bytes == 0u);
  assert(state.max_read <= kChunkBytes && state.calls > kPartitionBytes / kChunkBytes);
}

void test_late_non_ff_is_not_blank() {
  ReaderState state;
  state.non_ff_offset = kPartitionBytes - 1u;
  const Result r = scan(read_fixture, &state);
  assert(r.complete && r.error == 0 && !r.all_ff());
  assert(r.ff_bytes == kPartitionBytes - 1u && r.non_ff_bytes == 1u);
}

void test_late_read_error_is_incomplete_not_blank() {
  ReaderState state;
  state.error_offset = kPartitionBytes - 11u;
  state.max_return = 7u;
  const Result r = scan(read_fixture, &state);
  assert(!r.complete && !r.all_ff() && r.error == state.read_error);
  assert(r.bytes_scanned == state.error_offset - (state.error_offset % 7u));
  assert(r.ff_bytes == r.bytes_scanned && r.non_ff_bytes == 0u);
}

void test_no_reader_and_bad_callback_counts_fail_closed() {
  const Result missing = scan(nullptr, nullptr);
  assert(!missing.complete && !missing.all_ff() && missing.error == kNoReader);
  const Result stalled = scan(no_progress, nullptr);
  assert(!stalled.complete && !stalled.all_ff() && stalled.error == kNoProgress);
  const Result invalid = scan(overreport, nullptr);
  assert(!invalid.complete && !invalid.all_ff() &&
         invalid.error == kReaderCountOutOfRange);
}
}  // namespace

int main() {
  test_partial_reads_complete_blank();
  test_late_non_ff_is_not_blank();
  test_late_read_error_is_incomplete_not_blank();
  test_no_reader_and_bad_callback_counts_fail_closed();
  puts("PASS fixed partition numeric blank scan bounds and fail-closed behavior");
  return 0;
}
