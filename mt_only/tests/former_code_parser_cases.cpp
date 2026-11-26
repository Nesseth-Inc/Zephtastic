// Adapted from Qwen A proposal0ecaec0d; root fixed64+NUL array capacity, explicit includes and extended mutation coverage.
// SPDX-License-Identifier: GPL-3.0-only
// Assertion test for mt::pki::UsbTailPrepareParser::parse (114-byte frame).
#include "UsbTailPrepareCommand.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <initializer_list>
#include <algorithm>

namespace {
using mt::pki::UsbTailPrepareCommand;
using mt::pki::UsbTailPrepareParser;
const char kPrefix[] = "zt_pki_tail_prepare_v1 ";
const char kPub[] =
    "00112233445566778899aabbccddeeff"
    "00112233445566778899aabbccddeeff";

void frame(uint8_t *f, const char *node, const char *gen, const char *crc) {
  std::memset(f, '0', 114);
  std::memcpy(f, kPrefix, 23);
  std::memcpy(f + 23, node, 8);
  f[31] = ' ';
  std::memcpy(f + 32, kPub, 64);
  f[96] = ' ';
  std::memcpy(f + 97, gen, 8);
  f[105] = ' ';
  std::memcpy(f + 106, crc, 8);
}
}  // namespace

int main() {
  uint8_t f[114];
  UsbTailPrepareCommand out, tmp;
  // Valid 114-byte frame, lowercase hex, node=2, generation=1, crc=0.
  frame(f, "00000002", "00000001", "00000000");
  assert(UsbTailPrepareParser::parse(f, 114, out));
  assert(out.localNode == 2u && out.generation == 1u && out.crc == 0u);
  assert(out.localPublic[0] == 0x00 && out.localPublic[1] == 0x11);
  assert(out.localPublic[10] == 0xaa && out.localPublic[11] == 0xbb);
  assert(out.localPublic[15] == 0xff && out.localPublic[31] == 0xff);

  // Failure leaves out byte-for-byte unchanged.
  UsbTailPrepareCommand s;
  s.localNode = 0xDEADu;
  s.generation = 0xBEEFu;
  s.crc = 0xCAFEu;
  std::memset(s.localPublic, 0xAB, 32);
  frame(f, "00000000", "00000001", "00000000");
  assert(!UsbTailPrepareParser::parse(f, 114, s));
  assert(s.localNode == 0xDEADu && s.generation == 0xBEEFu &&
         s.crc == 0xCAFEu);
  for (int i = 0; i < 32; ++i) assert(s.localPublic[i] == 0xAB);

  // Length gates: 113/115 rejected, 114 accepted, nullptr rejected.
  frame(f, "00000002", "00000001", "00000000");
  assert(!UsbTailPrepareParser::parse(f, 113, tmp));
  assert(!UsbTailPrepareParser::parse(f, 115, tmp));
  assert(UsbTailPrepareParser::parse(f, 114, tmp));
  assert(!UsbTailPrepareParser::parse(nullptr, 114, tmp));

  // Separators at 31/96/105 and prefix byte 0.
  for (size_t sep : {31, 96, 105, 0}) {
    frame(f, "00000002", "00000001", "00000000");
    f[sep] = 'x';
    assert(!UsbTailPrepareParser::parse(f, 114, tmp));
  }

  // Node boundaries: reject 0, 1, 0xfffffffe, 0xffffffff; accept 0xfffffffd.
  for (const char *n : {"00000000", "00000001", "fffffffe", "ffffffff"}) {
    frame(f, n, "00000001", "00000000");
    assert(!UsbTailPrepareParser::parse(f, 114, tmp));
  }
  frame(f, "fffffffd", "00000001", "00000000");
  assert(UsbTailPrepareParser::parse(f, 114, tmp));
  assert(tmp.localNode == 0xfffffffdU);

  // Generation: reject 0, accept 1 and UINT32_MAX.
  frame(f, "00000002", "00000000", "00000000");
  assert(!UsbTailPrepareParser::parse(f, 114, tmp));
  frame(f, "00000002", "ffffffff", "00000000");
  assert(UsbTailPrepareParser::parse(f, 114, tmp));
  assert(tmp.generation == 0xffffffffU && tmp.crc == 0u);

  // Uppercase hex and non-hex nibble rejected.
  frame(f, "00000002", "00000001", "00000000");
  f[32] = 'A';
  assert(!UsbTailPrepareParser::parse(f, 114, tmp));
  frame(f, "0000000g", "00000001", "00000000");
  assert(!UsbTailPrepareParser::parse(f, 114, tmp));
  // Root additions: all114 single-byte NUL mutations preserve the entire output.
  for (size_t at=0;at<114;++at) {
    frame(f, "00000002", "00000001", "00000000"); f[at]=0;
    auto *representation=reinterpret_cast<unsigned char *>(&out);
    std::fill_n(representation,sizeof(out),0xa5); uint8_t before[sizeof(out)];
    std::memcpy(before,&out,sizeof(out));
    assert(!UsbTailPrepareParser::parse(f,114,out));
    assert(!std::memcmp(before,&out,sizeof(out)));
  }
  for (size_t n=0;n<=233;++n) if(n!=114) assert(!UsbTailPrepareParser::parse(f,n,tmp));
  frame(f, "00000002", "ffffffff", "ffffffff");
  assert(UsbTailPrepareParser::parse(f,114,tmp)&&tmp.crc==0xffffffffU);
  std::puts("PASS exact114B parser, every byte mutation/output preservation and numeric/length boundaries");
  return 0;
}
