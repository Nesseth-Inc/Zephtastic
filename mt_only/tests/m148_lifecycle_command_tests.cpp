// SPDX-License-Identifier: GPL-3.0-only
#include "pki/UsbLifecycleCommand.h"
#include "pki/UsbStorageCommand.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
constexpr char Prefix[] = "zt_pki_lifecycle_enable_v1 ";
constexpr size_t PrefixSize = sizeof(Prefix) - 1;
constexpr size_t FrameSize = 100;

void makeFrame(char out[FrameSize], const char node[8] = "12345678") {
  memcpy(out, Prefix, PrefixSize);
  memcpy(out + PrefixSize, node, 8);
  out[PrefixSize + 8] = ' ';
  static constexpr char Hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; ++i) {
    out[PrefixSize + 9 + 2 * i] = Hex[i >> 4];
    out[PrefixSize + 10 + 2 * i] = Hex[i & 15];
  }
}

void requireFailureUnchanged(const uint8_t *bytes, size_t length) {
  mt::pki::UsbLifecycleCommand out;
  memset(&out, 0xa5, sizeof(out));
  uint8_t before[sizeof(out)];
  memcpy(before, &out, sizeof(out));
  assert(!mt::pki::UsbLifecycleParser::parse(bytes, length, out));
  assert(memcmp(before, &out, sizeof(out)) == 0);
}

void validAndExactFrame() {
  char frame[FrameSize];
  makeFrame(frame);
  mt::pki::UsbLifecycleCommand out{};
  assert(mt::pki::UsbLifecycleParser::parse(
      reinterpret_cast<const uint8_t *>(frame), sizeof(frame), out));
  assert(out.localNode == 0x12345678u);
  for (size_t i = 0; i < 32; ++i) assert(out.localPublic[i] == i);
  requireFailureUnchanged(reinterpret_cast<const uint8_t *>(frame), sizeof(frame) - 1);
  char longer[FrameSize + 1];
  memcpy(longer, frame, sizeof(frame));
  longer[FrameSize] = ' ';
  requireFailureUnchanged(reinterpret_cast<const uint8_t *>(longer), sizeof(longer));
  requireFailureUnchanged(nullptr, sizeof(frame));
  requireFailureUnchanged(nullptr, 0);
}

void rejectsWhitespaceCaseNulAndPrefixChanges() {
  char frame[FrameSize];
  makeFrame(frame);
  frame[0] = ' ';
  requireFailureUnchanged(reinterpret_cast<const uint8_t *>(frame), sizeof(frame));
  makeFrame(frame);
  frame[PrefixSize + 8] = '\t';
  requireFailureUnchanged(reinterpret_cast<const uint8_t *>(frame), sizeof(frame));
  makeFrame(frame);
  frame[FrameSize - 1] = ' ';
  requireFailureUnchanged(reinterpret_cast<const uint8_t *>(frame), sizeof(frame));
  makeFrame(frame);
  frame[5] = 'P';
  requireFailureUnchanged(reinterpret_cast<const uint8_t *>(frame), sizeof(frame));
  makeFrame(frame);
  frame[PrefixSize + 9] = 'A';
  requireFailureUnchanged(reinterpret_cast<const uint8_t *>(frame), sizeof(frame));
  makeFrame(frame);
  frame[PrefixSize + 12] = '\0';
  requireFailureUnchanged(reinterpret_cast<const uint8_t *>(frame), sizeof(frame));
}

void rejectsReservedAndOutOfRangeNodes() {
  char frame[FrameSize];
  for (const char *node : {"00000000", "00000001", "fffffffe", "ffffffff"}) {
    makeFrame(frame, node);
    requireFailureUnchanged(reinterpret_cast<const uint8_t *>(frame), sizeof(frame));
  }
  makeFrame(frame, "fffffffd");
  mt::pki::UsbLifecycleCommand out{};
  assert(mt::pki::UsbLifecycleParser::parse(
      reinterpret_cast<const uint8_t *>(frame), sizeof(frame), out));
  assert(out.localNode == 0xfffffffdu);
}

void legacyStorageCommandRemainsSeparate() {
  static constexpr char Old[] = "zt_pki_store_enable_v1 ";
  static constexpr size_t OldPrefix = sizeof(Old) - 1;
  char storage[96];
  memcpy(storage, Old, OldPrefix);
  memcpy(storage + OldPrefix, "12345678", 8);
  storage[OldPrefix + 8] = ' ';
  memset(storage + OldPrefix + 9, 'a', 64);
  mt::pki::UsbStorageCommand oldOut{};
  assert(mt::pki::UsbStorageParser::parse(
      reinterpret_cast<const uint8_t *>(storage), sizeof(storage), oldOut));
  assert(oldOut.localNode == 0x12345678u);
  assert(!mt::pki::UsbStorageParser::parse(
      reinterpret_cast<const uint8_t *>(storage), sizeof(storage) - 1, oldOut));
  assert(!mt::pki::UsbStorageParser::parse(
      reinterpret_cast<const uint8_t *>(storage), sizeof(storage) + 1, oldOut));

  char lifecycle[FrameSize];
  makeFrame(lifecycle);
  assert(!mt::pki::UsbStorageParser::parse(
      reinterpret_cast<const uint8_t *>(lifecycle), sizeof(lifecycle), oldOut));
  storage[0] = 'Z';
  assert(!mt::pki::UsbStorageParser::parse(
      reinterpret_cast<const uint8_t *>(storage), sizeof(storage), oldOut));
}
}  // namespace

int main() {
  static_assert(FrameSize == sizeof(Prefix) - 1 + 8 + 1 + 64,
                "The lifecycle command is exactly 100 bytes");
  validAndExactFrame();
  rejectsWhitespaceCaseNulAndPrefixChanges();
  rejectsReservedAndOutOfRangeNodes();
  legacyStorageCommandRemainsSeparate();
  std::puts("PASS exact 100-byte lifecycle parse, unchanged output on rejection, and isolated legacy 96-byte storage parser");
}
