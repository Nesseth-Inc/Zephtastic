// UsbProvisionCommand.h
// Proposal artifact (outcome m75-usb-provision-command-20261022, attempt 1).
// Parent will run adversarial parser tests against this header.
//
// Isolated proposal code, not qualification. The parser is a pure,
// zero-allocation, no-I/O, non-mutating byte-string decoder. It does not
// implement or weaken any of: crypto admissibility, CRC/node binding,
// identity self checks, caller permission, authorized USB session, radio
// quiescence, storage/NVS gates, timeout, or commit/persistence semantics;
// those remain the core caller's responsibility. Private-key bytes are
// absent from this protocol.
//
// ASSUMPTIONS (source not accessed per task boundary; verify against
// PkiProvisioning.h): IdentityBinding{version,algorithm,generation:
// uint32_t, node: uint32_t, publicKey: uint8_t[32]}; ProvisionPeer{node:
// uint32_t, publicKey: uint8_t[32]}.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "PkiProvisioning.h"

namespace mt {
namespace pki {

struct UsbProvisionCommand {
  enum class Op : uint8_t { Stage, Commit, Status, Cancel };
  Op op = Op::Status;
  IdentityBinding expected{};
  ProvisionPeer peer{};
};

struct UsbProvisionParser {
  static bool handles(const uint8_t* in, size_t n) {
    if (!in) return false;
    if (n >= 7 && memcmp(in, "zt_pki_", 7) == 0) return true;
    static constexpr char spareStatus[] = "zt_storage_spare_status_v1";
    static constexpr char capacityStatus[] = "zt_storage_capacity_status_v1";
    return (n == sizeof(spareStatus) - 1 &&
            memcmp(in, spareStatus, sizeof(spareStatus) - 1) == 0) ||
           (n == sizeof(capacityStatus) - 1 &&
            memcmp(in, capacityStatus, sizeof(capacityStatus) - 1) == 0);
  }

  static bool parse(const uint8_t* in, size_t n, UsbProvisionCommand& out) {
    if (!in) return false;
    if (n == 15 && memcmp(in, "zt_pki_stage_v1", 15) == 0) {
      UsbProvisionCommand c; c.op = UsbProvisionCommand::Op::Stage; out = c; return true;
    }
    if (n == 16 && memcmp(in, "zt_pki_status_v1", 16) == 0) {
      UsbProvisionCommand c; c.op = UsbProvisionCommand::Op::Status; out = c; return true;
    }
    if (n == 16 && memcmp(in, "zt_pki_cancel_v1", 16) == 0) {
      UsbProvisionCommand c; c.op = UsbProvisionCommand::Op::Cancel; out = c; return true;
    }
    if (n != 164) return false;  // commit: 16+1+8+1+64+1+8+1+64
    if (memcmp(in, "zt_pki_commit_v1 ", 17) != 0) return false;
    const uint8_t* p = in + 17;
    uint32_t localNode = 0, peerNode = 0;
    uint8_t localKey[32] = {}, peerKey[32] = {};
    if (!decodeNode(p, localNode)) return false;
    if (p[8] != ' ') return false;
    if (!decodeKey(p + 9, localKey)) return false;
    if (p[73] != ' ') return false;
    if (!decodeNode(p + 74, peerNode)) return false;
    if (p[82] != ' ') return false;
    if (!decodeKey(p + 83, peerKey)) return false;
    if (localNode == peerNode) return false;
    UsbProvisionCommand c;
    c.op = UsbProvisionCommand::Op::Commit;
    c.expected.version = 1;
    c.expected.algorithm = 1;
    c.expected.generation = 1;
    c.expected.node = localNode;
    memcpy(c.expected.publicKey, localKey, 32);
    c.peer.node = peerNode;
    memcpy(c.peer.publicKey, peerKey, 32);
    out = c;
    return true;
  }

 private:
  static bool hexVal(uint8_t c, uint32_t& v) {
    if (c >= '0' && c <= '9') { v = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
    return false;
  }
  static bool decodeNode(const uint8_t* p, uint32_t& out) {
    uint32_t v = 0;
    for (int i = 0; i < 8; ++i) {
      uint32_t d;
      if (!hexVal(p[i], d)) return false;
      v = (v << 4) | d;
    }
    if (v < 2 || v > 0xfffffffd) return false;
    out = v;
    return true;
  }
  static bool decodeKey(const uint8_t* p, uint8_t* out32) {
    for (int i = 0; i < 32; ++i) {
      uint32_t hi, lo;
      if (!hexVal(p[2 * i], hi) || !hexVal(p[2 * i + 1], lo)) return false;
      out32[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
  }
};

}  // namespace pki
}  // namespace mt
