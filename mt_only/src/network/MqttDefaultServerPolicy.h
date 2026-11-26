// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt { namespace network {

// Narrow stock MQTT server policy for decoded uplink application ports.
// The host literal is pinned from Meshtastic 2.8.1 src/mesh/Default.h.
class MqttDefaultServerPolicy {
 public:
  static constexpr const char *kDefaultHost = "mqtt.meshtastic.org";
  static constexpr size_t kMaximumAddressLength = 63;
  static constexpr uint32_t kDetectionSensorPort = 10;
  static constexpr uint32_t kRangeTestPort = 66;

  // Mirrors stock parseHostAndPort() + isDefaultServer(): split at the first
  // colon only when it occurs after at least one host character; then match an
  // empty host or the exact case-sensitive default host. The settings caller
  // supplies the bounded stored address length, excluding its trailing NUL.
  static bool isDefaultAddress(const char *address, size_t length) {
    if (length > kMaximumAddressLength) return false;
    if (length == 0) return true;
    if (!address) return false;

    size_t hostLength = length;
    for (size_t i = 0; i < length; ++i) {
      if (address[i] == '\0') return false;
      if (address[i] == ':' && i > 0) {
        hostLength = i;
        break;
      }
    }
    return hostLength == sizeof("mqtt.meshtastic.org") - 1 &&
           memcmp(address, kDefaultHost, hostLength) == 0;
  }

  // Stock accepts only its standard plaintext/TLS ports when the parsed host
  // is the default server. The TLS flag selects the implicit port, but an
  // explicit 1883 or 8883 is accepted either way, matching isValidConfig().
  static bool allowsBrokerPort(bool defaultServer, uint32_t port) {
    return !defaultServer || port == 1883u || port == 8883u;
  }

  // Validate the stored address at config-admission time as well as the
  // resolved port at snapshot time. This keeps invalid default-server custom
  // ports from being persisted and still leaves custom brokers unrestricted.
  static bool allowsConfiguredAddress(const char *address, size_t length,
                                      bool tls) {
    if (!isDefaultAddress(address, length)) return true;

    uint32_t port = tls ? 8883u : 1883u;
    if (length == 0) return allowsBrokerPort(true, port);
    if (!address) return false;

    size_t colon = length;
    for (size_t i = 1; i < length; ++i) {
      if (address[i] == ':') {
        colon = i;
        break;
      }
    }
    if (colon < length) {
      if (colon + 1 >= length) return false;
      port = 0;
      for (size_t i = colon + 1; i < length; ++i) {
        const char c = address[i];
        if (c < '0' || c > '9') return false;
        const uint32_t digit = uint32_t(c - '0');
        if (port > (65535u - digit) / 10u) return false;
        port = port * 10u + digit;
      }
      if (port == 0) return false;
    }
    return allowsBrokerPort(true, port);
  }

};

}}  // namespace mt::network
