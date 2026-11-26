// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../stock/mqtt/RegionRoot.h"
#include "../RegionPolicy.h"

#include "MqttDefaultServerPolicy.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt { namespace network {

// Stock changes only an automatically-managed root when a validated region
// transition is accepted. This helper is deliberately policy-only: callers
// own admission, journaling, batching, and runtime reconnect behavior.
class MqttRegionRootPolicy {
 public:
  enum class Result : uint8_t { Unchanged, Changed, Invalid };
  static constexpr const char *kDefaultRoot = "msh";
  static constexpr size_t kDefaultRootLength = 3;
  static constexpr size_t kMaximumRootLength = 31;

  // These are the RegionInfo names in the pinned stock RadioInterface.cpp
  // regions table. Stock recognizes exactly these suffixes as generated
  // regional roots; arbitrary msh/<user-value> roots remain user-owned.
  static bool stockRegionName(const char *name, size_t length) {
    if (!name || !length) return false;
    for (size_t index = 0; index < radio::RegionPolicy::kRegionCount; ++index) {
      const char *candidate = radio::RegionPolicy::regionAt(index)->name;
      if (strlen(candidate) == length && !memcmp(name, candidate, length))
        return true;
    }
    return false;
  }

  static bool stockDefaultRoot(const char *root, size_t length) {
    if (length == 0) return true;
    if (!root || length > kMaximumRootLength) return false;
    if (length == kDefaultRootLength &&
        !memcmp(root, kDefaultRoot, kDefaultRootLength))
      return true;
    constexpr size_t prefixLength = kDefaultRootLength + 1;  // "msh/"
    if (length <= prefixLength ||
        memcmp(root, "msh/", prefixLength) != 0)
      return false;
    return stockRegionName(root + prefixLength, length - prefixLength);
  }

  // Returns Changed only when a new generated root should be saved. On
  // Unchanged or Invalid, output bytes are untouched.
  static Result apply(const char *address, size_t addressLength,
                      const char *root, size_t rootLength,
                      const char *regionName, size_t regionLength,
                      char *output, size_t outputCapacity) {
    if ((!address && addressLength) || (!root && rootLength) || !output ||
        rootLength > kMaximumRootLength ||
        !stockRegionName(regionName, regionLength))
      return Result::Invalid;
    return stock::MQTT::applyRegionRootTopic<Result>(
        MqttDefaultServerPolicy::isDefaultAddress(address, addressLength),
        stockDefaultRoot(root, rootLength), root, rootLength, regionName, regionLength,
        output, outputCapacity, kMaximumRootLength);
  }
};

}}  // namespace mt::network
