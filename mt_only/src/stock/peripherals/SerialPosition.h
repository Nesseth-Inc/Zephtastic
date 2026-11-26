// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, SerialModuleRadio::handleReceived
#pragma once

#include "../../serial/PositionOutput.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/module_config.pb.h"
#include <pb_decode.h>

namespace mt::stock::serial {

inline bool positionMode(uint32_t mode)
{
    return mode == meshtastic_ModuleConfig_SerialConfig_Serial_Mode_NMEA ||
           mode == meshtastic_ModuleConfig_SerialConfig_Serial_Mode_CALTOPO;
}

// Called after the UART owner admits a peer POSITION_APP packet. The Zephyr
// writer uses the configured NMEA/CalTopo sentence format and validated fix.
template <typename Emit>
inline bool handleReceivedPosition(const uint8_t *payload, size_t size, const char *senderName, Emit emit)
{
    meshtastic_Position scratch{};
    auto in = pb_istream_from_buffer(payload, size);
    if (!pb_decode(&in, meshtastic_Position_fields, &scratch) || in.bytes_left ||
        !scratch.has_latitude_i || !scratch.has_longitude_i)
        return false;
    mt_serial::Position fix{};
    fix.valid = true;
    fix.latitude_i = scratch.latitude_i;
    fix.longitude_i = scratch.longitude_i;
    fix.altitude_m = scratch.altitude;
    return emit(fix, senderName ? senderName : "");
}

} // namespace mt::stock::serial
