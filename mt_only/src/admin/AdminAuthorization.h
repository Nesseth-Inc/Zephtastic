// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../pki/PrivateRuntime.h"
#include "meshtastic/config.pb.h"
#include <stddef.h>
#include <stdint.h>

namespace mt::admin {

// Internal result only. An RF caller should not expose distinctions between
// malformed configuration, malformed input, and an ordinary denied key.
enum class Authorization : uint8_t {
  Authorized,
  InvalidEvent,
  InvalidAdminKeyConfig,
  Denied,
};

class AdminAuthorization {
 public:
  static constexpr size_t AdminKeyBytes = 32;
  static constexpr size_t MaximumAdminKeys = 3;
  static constexpr uint32_t FirstUnicastNode = 2;
  static constexpr uint32_t ReservedNodeBoundary = 0xfffffffeu;

  // This predicate does not authenticate, timestamp, or consume `event`.
  // Call it synchronously with the current value returned by
  // Service::takePrivateEvent(), in the existing private receive dispatcher,
  // immediately after successful authenticated decryption. Never call it on
  // a caller-created event, decoded ToRadio input, cached event, or pinned-peer
  // record. `security` must be a fresh snapshot of the current
  // meshtastic_Config_SecurityConfig for this decision; do not cache grants.
  // Admin payload/request parsing remains a separate caller responsibility.
  static Authorization authorizeAuthenticatedEvent(
      const pki::PrivateEvent &event, uint32_t localNode,
      const meshtastic_Config_SecurityConfig &security) {
    return authorizeCurrentIngress(event.from,event.to,event.id,event.publicKey,
                                   event.size,localNode,security);
  }

  // Production callbacks borrow these fields from authenticated, Fresh,
  // replay-committed PkiRuntime ingress: directly for a GET snapshot, or from
  // Service's bounded wiped owner-loop request for a deferred mutation. Only
  // those trusted paths establish provenance; copied fields alone are never a
  // grant. The mutation caller rechecks the current peer pin, allowlist and
  // session token immediately before application. This predicate cannot
  // authenticate caller-constructed/local packet metadata.
  static Authorization authorizeCurrentIngress(
      uint32_t from,uint32_t to,uint32_t id,const uint8_t *publicKey,
      size_t authenticatedSize,uint32_t localNode,
      const meshtastic_Config_SecurityConfig &security) {
    if (!isUnicastNode(localNode) || !isUnicastNode(from) ||
        from == localNode || to != localNode || id == 0 || !publicKey ||
        authenticatedSize == 0 || authenticatedSize > sizeof(pki::PrivateEvent::data) ||
        allZero(publicKey, AdminKeyBytes)) {
      return Authorization::InvalidEvent;
    }

    if (security.admin_key_count == 0 ||
        security.admin_key_count > MaximumAdminKeys) {
      return Authorization::InvalidAdminKeyConfig;
    }

    // Validate the entire active allowlist before matching, so a malformed
    // later entry cannot be hidden by an earlier matching entry.
    const size_t count = static_cast<size_t>(security.admin_key_count);
    for (size_t i = 0; i < count; ++i) {
      const auto &entry = security.admin_key[i];
      if (entry.size != AdminKeyBytes || allZero(entry.bytes, AdminKeyBytes))
        return Authorization::InvalidAdminKeyConfig;
    }

    uint8_t matched = 0;
    for (size_t i = 0; i < count; ++i)
      matched |= equalKey(publicKey, security.admin_key[i].bytes);
    return matched ? Authorization::Authorized : Authorization::Denied;
  }

 private:
  static constexpr bool isUnicastNode(uint32_t node) {
    return node >= FirstUnicastNode && node < ReservedNodeBoundary;
  }

  static bool allZero(const uint8_t *bytes, size_t size) {
    uint8_t aggregate = 0;
    for (size_t i = 0; i < size; ++i) aggregate |= bytes[i];
    return aggregate == 0;
  }

  // Avoid a prefix-dependent key comparison. Keys are public identities, but
  // processing all bytes and slots keeps authorization timing less revealing.
  static uint8_t equalKey(const uint8_t left[AdminKeyBytes],
                          const uint8_t right[AdminKeyBytes]) {
    uint8_t difference = 0;
    for (size_t i = 0; i < AdminKeyBytes; ++i) difference |= left[i] ^ right[i];
    return static_cast<uint8_t>(difference == 0);
  }
};

}  // namespace mt::admin
