// UsbPeerCommand.h
// SPDX-License-Identifier: GPL-3.0-only
// Pure, strict 166-byte USB peer command decoder (parent API / acceptance
// contract only). Frame:
//   zt_pki_peer_add_v1 <localnode8> <localpub64> <peernode8> <peerpub64>
// No crypto/CRC/self-key/trust/identity/USB-session/write-admission here;
// that is the caller's job. Allocation-free, exception-free, stateless, no
// input mutation. On any failure `out` is left byte-for-byte unchanged.

#ifndef MT_PKI_USB_PEER_COMMAND_H
#define MT_PKI_USB_PEER_COMMAND_H

#include <stddef.h>
#include <stdint.h>

namespace mt {
namespace pki {

struct UsbPeerCommand {
  uint32_t localNode = 0;
  uint8_t localPublic[32]{};
  uint32_t peerNode = 0;
  uint8_t peerPublic[32]{};
};

namespace detail {
constexpr size_t kFrameSize = 166;
constexpr size_t kPrefixLen = 19;
constexpr size_t kNodeHexLen = 8;
constexpr size_t kPubHexLen = 64;
constexpr size_t kPubByteLen = 32;
constexpr size_t kLocalNodeOff = 19;  // bytes 19..26
constexpr size_t kLocalPubOff = 28;   // bytes 28..91
constexpr size_t kPeerNodeOff = 93;   // bytes 93..100
constexpr size_t kPeerPubOff = 102;   // bytes 102..165
constexpr uint32_t kNodeMin = 2u;
constexpr uint32_t kNodeMax = 0xfffffffdU;

inline bool isLowerHex(uint8_t c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}
inline int hexNibble(uint8_t c) {
  return (c >= '0' && c <= '9') ? (c - '0') : (c - 'a' + 10);
}
inline bool decodeNode(const uint8_t *b, size_t off, uint32_t &node) {
  for (size_t i = 0; i < kNodeHexLen; ++i) {
    const uint8_t c = b[off + i];
    if (!isLowerHex(c)) return false;
    node = node * 16u + static_cast<uint32_t>(hexNibble(c));
  }
  return true;
}
inline bool decodePub(const uint8_t *b, size_t off, uint8_t out32[kPubByteLen]) {
  for (size_t i = 0; i < kPubByteLen; ++i) {
    const uint8_t hi = b[off + 2 * i];
    const uint8_t lo = b[off + 2 * i + 1];
    if (!isLowerHex(hi) || !isLowerHex(lo)) return false;
    out32[i] = static_cast<uint8_t>((hexNibble(hi) << 4) | hexNibble(lo));
  }
  return true;
}
}  // namespace detail

struct UsbPeerParser {
  static bool parse(const uint8_t *bytes, size_t size, UsbPeerCommand &out) {
    if (bytes == nullptr || size != detail::kFrameSize) return false;
    static const char kPrefix[] = "zt_pki_peer_add_v1 ";
    for (size_t i = 0; i < detail::kPrefixLen; ++i) {
      if (bytes[i] != static_cast<uint8_t>(kPrefix[i])) return false;
    }
    uint32_t localNode = 0;
    uint32_t peerNode = 0;
    uint8_t localPub[detail::kPubByteLen];
    uint8_t peerPub[detail::kPubByteLen];
    if (!detail::decodeNode(bytes, detail::kLocalNodeOff, localNode)) return false;
    if (bytes[detail::kLocalNodeOff + detail::kNodeHexLen] != ' ') return false;
    if (!detail::decodePub(bytes, detail::kLocalPubOff, localPub)) return false;
    if (bytes[detail::kLocalPubOff + detail::kPubHexLen] != ' ') return false;
    if (!detail::decodeNode(bytes, detail::kPeerNodeOff, peerNode)) return false;
    if (bytes[detail::kPeerNodeOff + detail::kNodeHexLen] != ' ') return false;
    if (!detail::decodePub(bytes, detail::kPeerPubOff, peerPub)) return false;
    if (localNode < detail::kNodeMin || localNode > detail::kNodeMax) return false;
    if (peerNode < detail::kNodeMin || peerNode > detail::kNodeMax) return false;
    if (localNode == peerNode) return false;
    out.localNode = localNode;
    out.peerNode = peerNode;
    for (size_t i = 0; i < detail::kPubByteLen; ++i) {
      out.localPublic[i] = localPub[i];
      out.peerPublic[i] = peerPub[i];
    }
    return true;
  }
};

}  // namespace pki
}  // namespace mt

#endif  // MT_PKI_USB_PEER_COMMAND_H
