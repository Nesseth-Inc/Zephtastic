// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <pb_encode.h>
#include "meshtastic/admin.pb.h"

namespace mt::admin {

// Stock AdminModule reserves at most twelve entries for pins learned from
// remote nodes and four for this node's configured RemoteHardware exports.
// Inputs are snapshots supplied by the owner; this helper performs no I/O,
// discovery, cache mutation, or pin-number inference.
class HardwarePinInventory {
public:
  static constexpr size_t ResponseCapacity = 16;
  static constexpr size_t RemoteCapacity = 12;
  static constexpr size_t LocalCapacity = 4;
  static constexpr size_t MaxEncodedBytes =
      meshtastic_NodeRemoteHardwarePinsResponse_size;

private:
  static bool validPin(const meshtastic_RemoteHardwarePin &pin) {
    const size_t length = strnlen(pin.name, sizeof(pin.name));
    if (length == sizeof(pin.name)) return false;
    for (size_t i = length + 1; i < sizeof(pin.name); ++i)
      if (pin.name[i] != 0) return false;
    if (pin.gpio_pin >= 64) return false;
    using RawType = __underlying_type(meshtastic_RemoteHardwarePinType);
    RawType type{};
    memcpy(&type, &pin.type, sizeof(type));
    return type >= static_cast<RawType>(meshtastic_RemoteHardwarePinType_UNKNOWN) &&
           type <= static_cast<RawType>(meshtastic_RemoteHardwarePinType_DIGITAL_WRITE);
  }

  static bool duplicate(const meshtastic_NodeRemoteHardwarePinsResponse &out,
                        uint32_t node, uint8_t gpio) {
    for (pb_size_t i = 0; i < out.node_remote_hardware_pins_count; ++i) {
      const auto &prior = out.node_remote_hardware_pins[i];
      if (prior.node_num == node && prior.has_pin && prior.pin.gpio_pin == gpio)
        return true;
    }
    return false;
  }

public:
  // Returns false for malformed or over-capacity snapshots. On failure, out
  // is left empty. Entries are compacted remote-first then local; all pin
  // descriptor fields (GPIO number, name, and type) are copied unchanged.
  // A zero local-node ID is valid only when there are no local exports.
  static bool build(uint32_t localNode,
                    const meshtastic_NodeRemoteHardwarePin *remote,
                    size_t remoteCount,
                    const meshtastic_RemoteHardwarePin *local,
                    size_t localCount,
                    meshtastic_NodeRemoteHardwarePinsResponse &out) {
    out = {};
    if (remoteCount > RemoteCapacity || localCount > LocalCapacity ||
        remoteCount + localCount > ResponseCapacity ||
        (remoteCount && !remote) || (localCount && (!local || !localNode)))
      return false;

    for (size_t i = 0; i < remoteCount; ++i) {
      const auto &entry = remote[i];
      if (!entry.node_num || !entry.has_pin || !validPin(entry.pin) ||
          duplicate(out, entry.node_num, entry.pin.gpio_pin)) {
        out = {};
        return false;
      }
      out.node_remote_hardware_pins[out.node_remote_hardware_pins_count++] = entry;
    }

    for (size_t i = 0; i < localCount; ++i) {
      if (!validPin(local[i]) || duplicate(out, localNode, local[i].gpio_pin)) {
        out = {};
        return false;
      }
      auto &entry = out.node_remote_hardware_pins[out.node_remote_hardware_pins_count++];
      entry = {};
      entry.node_num = localNode;
      entry.has_pin = true;
      entry.pin = local[i];
    }
    return out.node_remote_hardware_pins_count <= ResponseCapacity;
  }

  // Encodes the stock NodeRemoteHardwarePinsResponse protobuf with its exact
  // generated maximum-size bound. Short output buffers fail without exposing
  // a partial length to the caller.
  static bool encode(uint32_t localNode,
                     const meshtastic_NodeRemoteHardwarePin *remote,
                     size_t remoteCount,
                     const meshtastic_RemoteHardwarePin *local,
                     size_t localCount,
                     uint8_t *output, size_t capacity, size_t &length) {
    length = 0;
    if (!output || !capacity) return false;
    meshtastic_NodeRemoteHardwarePinsResponse response{};
    if (!build(localNode, remote, remoteCount, local, localCount, response))
      return false;
    const size_t boundedCapacity = capacity < MaxEncodedBytes ? capacity : MaxEncodedBytes;
    auto stream = pb_ostream_from_buffer(output, boundedCapacity);
    if (!pb_encode(&stream, meshtastic_NodeRemoteHardwarePinsResponse_fields,
                   &response) || stream.bytes_written > MaxEncodedBytes)
      return false;
    length = stream.bytes_written;
    return true;
  }
};

static_assert(sizeof(((meshtastic_NodeRemoteHardwarePinsResponse *)nullptr)
                         ->node_remote_hardware_pins) /
                  sizeof(meshtastic_NodeRemoteHardwarePin) ==
              HardwarePinInventory::ResponseCapacity,
              "stock pin response capacity changed");

} // namespace mt::admin
