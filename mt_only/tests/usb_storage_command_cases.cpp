// SPDX-License-Identifier: GPL-3.0-only
// usb_storage_command_cases.cpp
// Fixture for mt::pki::UsbStorageParser (declared API only; no parser logic here).
// Wire syntax (exactly 96 ASCII bytes): "zt_pki_store_enable_v1 " + 8 lowercase hex
// (node 2..0xfffffffd) + " " + 64 lowercase hex. Failed parses must preserve fields.
#include "UsbStorageCommand.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

using mt::pki::UsbStorageCommand;
using mt::pki::UsbStorageParser;

int g_failures = 0;
#define CHECK(cond) \
  do { if (!(cond)) { ++g_failures; std::printf("CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// Deterministic public test bytes (lowercase hex).
const char kNodeHex[] = "0000000a";  // node 0x0000000a, inside [2, 0xfffffffd].
const char kPubHex[] =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

std::string MakeCommand(const std::string& nodeHex, const std::string& pubHex) {
  std::string cmd = "zt_pki_store_enable_v1 ";
  cmd += nodeHex;
  cmd += ' ';
  cmd += pubHex;
  return cmd;
}

UsbStorageCommand Sentinel() {
  UsbStorageCommand c;
  c.localNode = 0x5a5a5a5aU;
  for (size_t i = 0; i < sizeof(c.localPublic); ++i) c.localPublic[i] = 0x5aU;
  return c;
}

void CheckPreserved(const UsbStorageCommand& c) {
  CHECK(c.localNode == 0x5a5a5a5aU);
  for (size_t i = 0; i < sizeof(c.localPublic); ++i) CHECK(c.localPublic[i] == 0x5aU);
}

void ExpectReject(const std::string& cmd) {
  UsbStorageCommand c = Sentinel();
  CHECK(!UsbStorageParser::parse(
      reinterpret_cast<const uint8_t*>(cmd.data()), cmd.size(), c));
  CheckPreserved(c);
}

void ExpectAccept(const std::string& cmd, uint32_t node) {
  UsbStorageCommand c = Sentinel();
  CHECK(UsbStorageParser::parse(
      reinterpret_cast<const uint8_t*>(cmd.data()), cmd.size(), c));
  CHECK(c.localNode == node);
  for (size_t i = 0; i < sizeof(c.localPublic); ++i) CHECK(c.localPublic[i] == static_cast<uint8_t>(i));
}

}  // namespace

int main() {
  const std::string valid = MakeCommand(kNodeHex, kPubHex);
  CHECK(valid.size() == 96);
  ExpectAccept(valid, 0x0000000aU);  // decodes node 0x0a and key bytes 0x00..0x1f.

  // Wrong prefix (same 96-byte length, prefix altered).
  ExpectReject(std::string("zt_pki_store_enable_v2 ") + kNodeHex + " " + kPubHex);

  // Short input (95 bytes).
  ExpectReject(valid.substr(0, 95));

  // Extra/trailing byte (97 bytes).
  ExpectReject(valid + "x");

  // Invalid node range: below 2 and above 0xfffffffd.
  ExpectReject(MakeCommand("00000000", kPubHex));  // node 0
  ExpectReject(MakeCommand("00000001", kPubHex));  // node 1
  ExpectReject(MakeCommand("fffffffe", kPubHex));  // node 0xfffffffe
  ExpectReject(MakeCommand("ffffffff", kPubHex));  // node 0xffffffff

  // Uppercase hex is rejected (node field, then public-key field).
  ExpectReject(MakeCommand("0000000A", kPubHex));
  ExpectReject(MakeCommand(kNodeHex, "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F"));

  // Non-hex character in node field.
  ExpectReject(MakeCommand("0000000g", kPubHex));

  // Missing separator after prefix (95 bytes).
  ExpectReject(std::string("zt_pki_store_enable_v1") + kNodeHex + " " + kPubHex);

  // Missing separator between node and public key (95 bytes).
  ExpectReject(std::string("zt_pki_store_enable_v1 ") + kNodeHex + kPubHex);

  // Incorrect separator character (96 bytes, second space replaced by ':').
  ExpectReject(std::string("zt_pki_store_enable_v1 ") + kNodeHex + ":" + kPubHex);

  // Boundary values and null/truncation must never change the sentinel output.
  ExpectAccept(MakeCommand("00000002", kPubHex), 2u);
  ExpectAccept(MakeCommand("fffffffd", kPubHex), 0xfffffffdu);
  auto sentinel=Sentinel();
  CHECK(!UsbStorageParser::parse(nullptr, valid.size(), sentinel));
  CheckPreserved(sentinel);
  for(size_t n=0;n<valid.size();++n)ExpectReject(valid.substr(0,n));
  if(!g_failures)std::puts("PASS strict storage-enable syntax bounds and unchanged failure outputs");
  return g_failures == 0 ? 0 : 1;
}
