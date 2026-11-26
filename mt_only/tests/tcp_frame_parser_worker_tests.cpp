// SPDX-License-Identifier: GPL-3.0-only
// mt_tcp_frame_parser_fixture.cpp — proposal assert-based C++17 fixture for
// mt::tcp::FrameParser. Contract: 94 c3 | length BE16 | body (1..512);
// push() consumes one byte at a time; only FrameReady may modify out (raw
// protobuf body); session change resets partial state; partial input expires
// only when unsigned elapsed >2000ms (USB-parser predicate); sync bytes in a
// payload are data. The inline implementation is a reference implementation of the locked contract for this fixture only; not production evidence.
#include "network/MtTcpFrameParser.h"
#include <cassert>
#include <cstdint>



namespace {
using mt::tcp::Frame;
using mt::tcp::FrameParser;
using R = FrameParser::Result;

void group1_fragmented_one_byte_body() {
  FrameParser p; Frame out{};
  const uint8_t stream[5] = {0x94, 0xc3, 0x00, 0x01, 0xAB};
  for (int i = 0; i < 4; ++i) assert(p.push(stream[i], 1000, 7, out) == R::NeedMore);
  assert(out.length == 0);
  assert(p.push(stream[4], 1000, 7, out) == R::FrameReady && out.length == 1 && out.payload[0] == 0xAB);
}

void group2_coalesced_frames_sync_is_data() {
  FrameParser p; Frame out{};
  const uint8_t stream[13] = {0x94, 0xc3, 0x00, 0x04, 0x94, 0xc3, 0x00, 0x11,
                               0x94, 0xc3, 0x00, 0x01, 0x22};
  for (int i = 0; i < 7; ++i) assert(p.push(stream[i], 2000, 1, out) == R::NeedMore);
  assert(p.push(stream[7], 2000, 1, out) == R::FrameReady && out.length == 4 && out.payload[0] == 0x94 && out.payload[1] == 0xc3 && out.payload[2] == 0x00 && out.payload[3] == 0x11);
  for (int i = 8; i < 12; ++i) assert(p.push(stream[i], 2000, 1, out) == R::NeedMore);
  assert(p.push(stream[12], 2000, 1, out) == R::FrameReady && out.length == 1 && out.payload[0] == 0x22);
}

void group3_length_boundaries() {
  FrameParser p; Frame out{};
  const uint8_t hdr[4] = {0x94, 0xc3, 0x02, 0x00};
  uint8_t body[512];
  for (int i = 0; i < 512; ++i) body[i] = uint8_t(i & 0xFF);
  for (int i = 0; i < 4; ++i) assert(p.push(hdr[i], 3000, 2, out) == R::NeedMore);
  for (int i = 0; i < 511; ++i) assert(p.push(body[i], 3000, 2, out) == R::NeedMore);
  assert(p.push(body[511], 3000, 2, out) == R::FrameReady && out.length == 512);
  for (int i = 0; i < 512; ++i) assert(out.payload[i] == uint8_t(i & 0xFF));
  const uint8_t len0[4] = {0x94, 0xc3, 0x00, 0x00};
  const uint8_t len513[4] = {0x94, 0xc3, 0x02, 0x01};
  for (int i = 0; i < 3; ++i) assert(p.push(len0[i], 3000, 2, out) == R::NeedMore);
  assert(p.push(len0[3], 3000, 2, out) == R::Invalid);
  for (int i = 0; i < 3; ++i) assert(p.push(len513[i], 3000, 2, out) == R::NeedMore);
  assert(p.push(len513[3], 3000, 2, out) == R::Invalid);
  assert(out.length == 512 && out.payload[0] == 0x00 && out.payload[511] == 0xFF);
}

void group4_session_change_and_timeout() {
  FrameParser p; Frame out{};
  const uint8_t partial[4] = {0x94, 0xc3, 0x00, 0x05};
  for (int i = 0; i < 4; ++i) assert(p.push(partial[i], 1000, 1, out) == R::NeedMore);
  assert(p.push(0x94, 1050, 2, out) == R::NeedMore);  // session change discards partial
  assert(p.push(0xc3, 1050, 2, out) == R::NeedMore && p.push(0x00, 1050, 2, out) == R::NeedMore && p.push(0x01, 1050, 2, out) == R::NeedMore);
  assert(p.push(0x77, 1050, 2, out) == R::FrameReady && out.length == 1 && out.payload[0] == 0x77);
  assert(p.push(0x94, 2000, 3, out) == R::NeedMore && p.push(0xc3, 2000, 3, out) == R::NeedMore && p.push(0x00, 2000, 3, out) == R::NeedMore && p.push(0x02, 2000, 3, out) == R::NeedMore);
  assert(p.push(0x11, 4000, 3, out) == R::NeedMore);  // elapsed == 2000ms: survives
  assert(p.push(0x94, 6002, 3, out) == R::NeedMore);  // elapsed 2002ms > 2000: discarded
  assert(p.push(0xc3, 6002, 3, out) == R::NeedMore && p.push(0x00, 6002, 3, out) == R::NeedMore && p.push(0x01, 6002, 3, out) == R::NeedMore);
  assert(p.push(0x33, 6002, 3, out) == R::FrameReady && out.length == 1 && out.payload[0] == 0x33);
}
}  // namespace

int main() {
  group1_fragmented_one_byte_body(); group2_coalesced_frames_sync_is_data(); group3_length_boundaries(); group4_session_change_and_timeout();
  return 0;
}
