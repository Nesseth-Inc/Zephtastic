// SPDX-License-Identifier: GPL-3.0-only
// Pure strict decoder for the explicit PKI history lifecycle-enable command.

#ifndef MT_PKI_USB_LIFECYCLE_COMMAND_H
#define MT_PKI_USB_LIFECYCLE_COMMAND_H

#include <stddef.h>
#include <stdint.h>

#include "UsbPeerCommand.h"

namespace mt {
namespace pki {

struct UsbLifecycleCommand {
  uint32_t localNode = 0;
  uint8_t localPublic[32]{};
};

namespace detail {
constexpr char kLifecyclePrefix[] = "zt_pki_lifecycle_enable_v1 ";
constexpr size_t kLifecyclePrefixLen = sizeof(kLifecyclePrefix) - 1;
constexpr size_t kLifecycleNodeOff = kLifecyclePrefixLen;
constexpr size_t kLifecyclePubOff = kLifecycleNodeOff + kNodeHexLen + 1;
constexpr size_t kLifecycleFrameSize = kLifecyclePubOff + kPubHexLen;
static_assert(kLifecycleFrameSize == 100, "Exact M148 lifecycle command size");
}  // namespace detail

struct UsbLifecycleParser {
  static bool parse(const uint8_t *bytes, size_t size, UsbLifecycleCommand &out) {
    if (!bytes || size != detail::kLifecycleFrameSize) return false;
    for (size_t i = 0; i < detail::kLifecyclePrefixLen; ++i) {
      if (bytes[i] != static_cast<uint8_t>(detail::kLifecyclePrefix[i])) return false;
    }

    uint32_t localNode = 0;
    uint8_t localPublic[detail::kPubByteLen]{};
    if (!detail::decodeNode(bytes, detail::kLifecycleNodeOff, localNode)) return false;
    if (bytes[detail::kLifecycleNodeOff + detail::kNodeHexLen] != ' ') return false;
    if (!detail::decodePub(bytes, detail::kLifecyclePubOff, localPublic)) return false;
    if (localNode < detail::kNodeMin || localNode > detail::kNodeMax) return false;

    out.localNode = localNode;
    for (size_t i = 0; i < detail::kPubByteLen; ++i) out.localPublic[i] = localPublic[i];
    return true;
  }
};

}  // namespace pki
}  // namespace mt

#endif  // MT_PKI_USB_LIFECYCLE_COMMAND_H
