// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic firmware 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, src/mesh/PositionPrecision.h
#pragma once

#include "meshtastic/channel.pb.h"
#include "meshtastic/mesh.pb.h"
#include <stdint.h>

namespace mt { class PositionChannels; }
namespace mt::stock {

// Max precision on a publicly-decryptable channel. CCPA "precise geolocation" = within a ~564m (1,850ft) radius.
// Precision is bit-truncation of latitude_i/longitude_i: the latitude cell stays ~constant in meters worldwide
// (~700m at 15 bits), while only the longitude cell varies - widest at the equator, narrowing toward the poles.
// 15 also matches the MQTT map-report public precision ceiling.
#define MAX_POSITION_PRECISION_PUBLIC_KEY 15

// Configured precision as-is; does NOT apply the public-key clamp -- use the channelIndex overload for the on-wire value.
uint32_t getPositionPrecisionForChannel(const meshtastic_Channel &channel);

// Configured precision, clamped to MAX_POSITION_PRECISION_PUBLIC_KEY when the channel's effective key is publicly decryptable.
uint32_t getPositionPrecisionForChannel(const PositionChannels &channels, uint8_t channelIndex);

// The channel our position goes out on: the lowest index with a non-zero on-wire precision (disabled and event
// channels never qualify). Returns false when position sharing is off on every channel.
bool findPositionChannel(const PositionChannels &channels, uint8_t &channelIndex);

// Truncate a single latitude_i/longitude_i to `precision` significant bits, centered in the
// resulting grid cell (stable under GPS jitter). precision 0 or >=32 returns the value unchanged.
// The return is the coordinate (int32_t); the uint8_t overload only narrows the precision arg.
int32_t truncateCoordinate(int32_t coordinate, uint32_t precision);
int32_t truncateCoordinate(int32_t coordinate, uint8_t precision);
void applyPositionPrecision(meshtastic_Position &position, uint32_t precision);
bool applyPositionPrecision(meshtastic_MeshPacket &packet, uint32_t precision);
bool applyPositionPrecisionForChannel(meshtastic_MeshPacket &packet, const PositionChannels &channels, uint8_t channelIndex);

} // namespace mt::stock
