// SPDX-License-Identifier: GPL-3.0-only
// UsbStorageCommand.h
// USB storage-enable command parser (zt-m70, mt_only).
//
// Parses exactly one 96-byte ASCII frame:
//   zt_pki_store_enable_v1 <localnode:8 lower hex> <localpub:64 lower hex>
//
// Byte layout (0-based):
//   [0,23)   prefix "zt_pki_store_enable_v1 " (23 bytes, ends with a space)
//   [23,31)  local node id, 8 lower-hex chars
//   [31]     single ASCII space
//   [32,96)  local public key, 64 lower-hex chars
//
// Crypto/CRC/public-key validation, USB routing, storage writes and
// admission are caller responsibilities.

#pragma once

#include <cstddef>
#include <cstdint>

#include "UsbPeerCommand.h"

namespace mt {
namespace pki {

namespace detail {
constexpr size_t kStoreFrameSize = 96;  // exact frame length
constexpr size_t kStorePrefixLen = 23;  // bytes [0,23), includes trailing space
constexpr size_t kStoreNodeOff = 23;    // bytes [23,31)
constexpr size_t kStorePubOff = 32;     // bytes [32,96)
}  // namespace detail

struct UsbStorageCommand {
  uint32_t localNode = 0;
  uint8_t localPublic[32]{};
};

struct UsbStorageParser {
  static bool parse(const uint8_t *bytes, size_t size, UsbStorageCommand &out) {
    // Reject null input and any length other than exactly 96 bytes.
    if (bytes == nullptr || size != detail::kStoreFrameSize) return false;
    static const char kPrefix[] = "zt_pki_store_enable_v1 ";
    for (size_t i = 0; i < detail::kStorePrefixLen; ++i) {
      if (bytes[i] != static_cast<uint8_t>(kPrefix[i])) return false;
    }
    // Parse into local candidates; out is touched only after all checks pass.
    uint32_t localNode = 0;
    uint8_t localPub[detail::kPubByteLen];
    if (!detail::decodeNode(bytes, detail::kStoreNodeOff, localNode)) return false;
    if (bytes[detail::kStoreNodeOff + detail::kNodeHexLen] != ' ') return false;
    if (!detail::decodePub(bytes, detail::kStorePubOff, localPub)) return false;
    if (localNode < detail::kNodeMin || localNode > detail::kNodeMax) return false;
    out.localNode = localNode;
    for (size_t i = 0; i < detail::kPubByteLen; ++i) {
      out.localPublic[i] = localPub[i];
    }
    return true;
  }
};

}  // namespace pki
}  // namespace mt
