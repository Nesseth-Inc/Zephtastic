// SPDX-License-Identifier: GPL-3.0-only
// peer_command_cases.cpp - standalone assertion suite for UsbPeerCommand.h.
// Synthetic literals only; no implementation, integration, compile or run claims.
#include "UsbPeerCommand.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using namespace mt::pki;
namespace {
constexpr std::size_t kLen = 166;
const char kPrefix[] = "zt_pki_peer_add_v1 "; // 19 bytes incl. trailing space
int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
enum { kLNode = 19, kSp1 = 27, kLPub = 28, kSp2 = 92, kPNode = 93, kSp3 = 101, kPPub = 102 };
void put(std::uint8_t (&b)[kLen], int off, const char *s, int n) {
  for (int i = 0; i < n; ++i) b[off + i] = static_cast<std::uint8_t>(s[i]);
}
void build(std::uint8_t (&b)[kLen], const char *ln, const char *lp, const char *pn, const char *pp) {
  std::memset(b, 0, kLen);
  std::memcpy(b, kPrefix, 19);
  put(b, kLNode, ln, 8); b[kSp1] = ' ';
  put(b, kLPub, lp, 64); b[kSp2] = ' ';
  put(b, kPNode, pn, 8); b[kSp3] = ' ';
  put(b, kPPub, pp, 64);
}
void hexToBytes(const char *hex, std::uint8_t out[32]) {
  auto v = [](char c) { return (c >= '0' && c <= '9') ? c - '0' : c - 'a' + 10; };
  for (int i = 0; i < 32; ++i) out[i] = static_cast<std::uint8_t>(v(hex[2 * i]) * 16 + v(hex[2 * i + 1]));
}
UsbPeerCommand prefill() {
  UsbPeerCommand c;
  c.localNode = 0x11223344u;
  c.peerNode = 0x55667788u;
  for (int i = 0; i < 32; ++i) { c.localPublic[i] = static_cast<std::uint8_t>(0xA0 + i); c.peerPublic[i] = static_cast<std::uint8_t>(0xB0 + i); }
  return c;
}
bool same(const std::uint8_t *a, const std::uint8_t *b, std::size_t n) { return std::memcmp(a, b, n) == 0; }
bool equal(const UsbPeerCommand &a, const UsbPeerCommand &b) {
  return a.localNode==b.localNode && a.peerNode==b.peerNode &&
    same(a.localPublic,b.localPublic,32) && same(a.peerPublic,b.peerPublic,32);
}
void neg(const char *name, const std::uint8_t (&in)[kLen], std::size_t size) {
  std::uint8_t saved[kLen];
  std::memcpy(saved, in, kLen);
  UsbPeerCommand out = prefill();
  UsbPeerCommand before = out;
  bool r = UsbPeerParser::parse(in, size, out);
  if (r) { ++g_fail; std::printf("FAIL %s: parse returned true\n", name); }
  if (!same(in, saved, kLen)) { ++g_fail; std::printf("FAIL %s: input mutated\n", name); }
  if (out.localNode != before.localNode || out.peerNode != before.peerNode ||
      !same(out.localPublic, before.localPublic, 32) || !same(out.peerPublic, before.peerPublic, 32)) {
    ++g_fail; std::printf("FAIL %s: output not preserved\n", name);
  }
}
} // namespace
int main() {
  const char *LP = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
  const char *PP = "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";
  const char *Z64 = "0000000000000000000000000000000000000000000000000000000000000000";
  const char *F64 = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
  // Valid: boundary nodes 2 and 0xfffffffd, exact public decode.
  {
    std::uint8_t b[kLen];
    build(b, "00000002", LP, "fffffffd", PP);
    UsbPeerCommand c;
    CHECK(UsbPeerParser::parse(b, kLen, c));
    CHECK(c.localNode == 2u);
    CHECK(c.peerNode == 0xfffffffdU);
    std::uint8_t el[32], ep[32];
    hexToBytes(LP, el); hexToBytes(PP, ep);
    CHECK(same(c.localPublic, el, 32));
    CHECK(same(c.peerPublic, ep, 32));
  }
  {
    std::uint8_t b[kLen];
    build(b, "fffffffd", PP, "00000002", LP);
    UsbPeerCommand c;
    CHECK(UsbPeerParser::parse(b, kLen, c));
    CHECK(c.localNode == 0xfffffffdU);
    CHECK(c.peerNode == 2u);
  }
  // Valid: all-zero and all-ff synthetic lexical keys.
  {
    std::uint8_t b[kLen];
    build(b, "00000002", Z64, "00000003", Z64);
    UsbPeerCommand c;
    CHECK(UsbPeerParser::parse(b, kLen, c));
    CHECK(c.localNode == 2u && c.peerNode == 3u);
    for (int i = 0; i < 32; ++i) CHECK(c.localPublic[i] == 0x00 && c.peerPublic[i] == 0x00);
  }
  {
    std::uint8_t b[kLen];
    build(b, "00000002", F64, "00000003", F64);
    UsbPeerCommand c;
    CHECK(UsbPeerParser::parse(b, kLen, c));
    for (int i = 0; i < 32; ++i) CHECK(c.localPublic[i] == 0xff && c.peerPublic[i] == 0xff);
  }
  // Negative: null and every truncation length.
  {
    std::uint8_t b[kLen];
    build(b, "00000002", LP, "00000003", PP);
    std::uint8_t saved[kLen];
    std::memcpy(saved, b, kLen);
    UsbPeerCommand c = prefill();
    const auto nullBefore=c;
    CHECK(!UsbPeerParser::parse(nullptr, 0, c)); CHECK(equal(c,nullBefore));
    CHECK(!UsbPeerParser::parse(nullptr, kLen, c)); CHECK(equal(c,nullBefore));
    for (std::size_t sz = 0; sz < kLen; ++sz) {
      UsbPeerCommand o = prefill();
      UsbPeerCommand before = o;
      CHECK(!UsbPeerParser::parse(b, sz, o));
      CHECK(same(b, saved, kLen));
      CHECK(o.localNode == before.localNode && o.peerNode == before.peerNode);
      CHECK(same(o.localPublic, before.localPublic, 32));
      CHECK(same(o.peerPublic, before.peerPublic, 32));
    }
  }
  // Negative: single-byte mutations of a valid frame.
  std::uint8_t base[kLen];
  build(base, "00000002", LP, "00000003", PP);
  auto mut = [&](const char *name, int off, std::uint8_t val) {
    std::uint8_t b[kLen];
    std::memcpy(b, base, kLen);
    b[off] = val;
    neg(name, b, kLen);
  };
  // Extra NUL / newline / space inside a field.
  mut("nul", kPPub + 63, '\0');
  mut("newline", kPPub + 63, '\n');
  mut("space", kPPub + 63, ' ');
  // Extra trailing byte (167-byte frame).
  for (const auto suffix : {'\0','\n',' '}) {
    std::uint8_t b[167];
    std::memcpy(b, base, kLen);
    b[166] = suffix;
    std::uint8_t saved[167];
    std::memcpy(saved, b, 167);
    UsbPeerCommand c = prefill();
    UsbPeerCommand before = c;
    CHECK(!UsbPeerParser::parse(b, 167, c));
    CHECK(same(b, saved, 167));
    CHECK(equal(c,before));
  }
  // Wrong prefix.
  mut("prefix_version", 17, '2');
  mut("prefix_lead", 0, 'Z');
  // Uppercase in each field.
  mut("upper_lnode", kLNode + 7, 'A');
  mut("upper_lpub", kLPub + 16, 'A');
  mut("upper_pnode", kPNode + 7, 'A');
  mut("upper_ppub", kPPub + 16, 'A');
  // Non-hex in each field.
  mut("nonhex_lnode", kLNode + 7, 'g');
  mut("nonhex_lpub", kLPub + 16, 'g');
  mut("nonhex_pnode", kPNode + 7, 'g');
  mut("nonhex_ppub", kPPub + 16, 'g');
  // Non-ASCII in each field.
  mut("nonascii_lnode", kLNode + 7, 0xC3);
  mut("nonascii_lpub", kLPub + 16, 0xC3);
  mut("nonascii_pnode", kPNode + 7, 0xC3);
  mut("nonascii_ppub", kPPub + 16, 0xC3);
  // Incorrect separators / space positions.
  mut("sep_colon", kSp1, ':');
  mut("space_in_lpub", kLPub, ' ');
  mut("sep2_hex", kSp2, '0');
  // Invalid node values (local invalid, peer valid and distinct).
  const char *badNodes[4] = {"00000000", "00000001", "fffffffe", "ffffffff"};
  for (int i = 0; i < 4; ++i) {
    std::uint8_t b[kLen];
    build(b, badNodes[i], LP, "00000003", PP);
    neg("badnode", b, kLen);
    build(b, "00000002", LP, badNodes[i], PP); neg("bad-peer-node", b, kLen);
  }
  // Equal local and peer nodes.
  {
    std::uint8_t b[kLen];
    build(b, "00000002", LP, "00000002", PP);
    neg("equalnodes", b, kLen);
  }
  std::printf("peer_command_cases: %s (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  if (!g_fail) std::printf("PASS strict-peer-command\n");
  return g_fail ? 1 : 0;
}
