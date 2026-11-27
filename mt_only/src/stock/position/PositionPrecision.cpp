// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic firmware 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, src/mesh/PositionPrecision.cpp
#include "PositionPrecision.h"
// ZT: take an immutable view of the calling settings object in channel-dependent functions.
#include "PositionChannels.h"
#include <pb_decode.h>
#include <pb_encode.h>

namespace mt::stock {

static_assert(Config::PublicPositionPrecision == MAX_POSITION_PRECISION_PUBLIC_KEY);

uint32_t getPositionPrecisionForChannel(const meshtastic_Channel &channel)
{
    if (channel.settings.has_module_settings) {
        return channel.settings.module_settings.position_precision;
    }
    // No module settings: fail closed. A PRIMARY channel used to default to 32
    // here, leaking an exact position on a sharing-disabled channel. See #10509.
    return 0;
}

uint32_t getPositionPrecisionForChannel(const PositionChannels &channels, uint8_t channelIndex)
{
    // Event-channel privacy takes precedence over every stored precision and key policy.
    if (channels.isEventChannel(channelIndex))
        return 0;

    const meshtastic_Channel &ch = channels.getByIndex(channelIndex);
    if (ch.role == meshtastic_Channel_Role_DISABLED)
        return 0;
    uint32_t precision = getPositionPrecisionForChannel(ch);

    // Never send a precise position on a publicly-decryptable channel (key check is gated on > ceiling).
    if (precision > MAX_POSITION_PRECISION_PUBLIC_KEY && channels.usesPublicKey(channelIndex)) {
        precision = MAX_POSITION_PRECISION_PUBLIC_KEY;
    }
    return precision;
}

bool findPositionChannel(const PositionChannels &channels, uint8_t &channelIndex)
{
    for (uint8_t i = 0; i < channels.getNumChannels(); i++) {
        if (getPositionPrecisionForChannel(channels, i) != 0) {
            channelIndex = i;
            return true;
        }
    }
    return false;
}

int32_t truncateCoordinate(int32_t coordinate, uint32_t precision)
{
    if (precision == 0 || precision >= 32)
        return coordinate;

    uint32_t coordinateBits = static_cast<uint32_t>(coordinate);
    uint32_t truncated = coordinateBits & (UINT32_MAX << (32 - precision));

    // Use the middle of the possible location, not the low edge of the bucket.
    truncated += (1UL << (31 - precision));

    return static_cast<int32_t>(truncated);
}

int32_t truncateCoordinate(int32_t coordinate, uint8_t precision)
{
    return truncateCoordinate(coordinate, static_cast<uint32_t>(precision));
}

void applyPositionPrecision(meshtastic_Position &position, uint32_t precision)
{
    if (precision == 0) {
        uint32_t time = position.time;
        position = meshtastic_Position_init_default;
        position.time = time;
        return;
    }

    uint32_t effectivePrecision = precision > 32 ? 32 : precision;
    position.precision_bits = effectivePrecision;

    if (effectivePrecision < 32) {
        position.latitude_i = truncateCoordinate(position.latitude_i, effectivePrecision);
        position.longitude_i = truncateCoordinate(position.longitude_i, effectivePrecision);
    }
}

bool applyPositionPrecision(meshtastic_MeshPacket &packet, uint32_t precision)
{
    if (packet.which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        packet.decoded.portnum != meshtastic_PortNum_POSITION_APP) {
        return true;
    }

    // ZT: a locally constructed packet also has to fit its nanopb buffer.
    if (packet.decoded.payload.size > sizeof(packet.decoded.payload.bytes)) {
        return false;
    }
    meshtastic_Position position = meshtastic_Position_init_default;
    auto input = pb_istream_from_buffer(packet.decoded.payload.bytes, packet.decoded.payload.size);
    if (!pb_decode(&input, &meshtastic_Position_msg, &position)) {
        return false;
    }

    applyPositionPrecision(position, precision);
    // ZT: an empty encoded Position is valid; distinguish it from a failed encode.
    auto stream = pb_ostream_from_buffer(packet.decoded.payload.bytes, sizeof(packet.decoded.payload.bytes));
    if (!pb_encode(&stream, &meshtastic_Position_msg, &position)) {
        packet.decoded.payload.size = 0;
        return false;
    }
    packet.decoded.payload.size = stream.bytes_written;
    return true;
}

bool applyPositionPrecisionForChannel(meshtastic_MeshPacket &packet, const PositionChannels &channels, uint8_t channelIndex)
{
    if (packet.which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        packet.decoded.portnum != meshtastic_PortNum_POSITION_APP) {
        return true;
    }

    return applyPositionPrecision(packet, getPositionPrecisionForChannel(channels, channelIndex));
}

} // namespace mt::stock

unsigned mt::Config::positionPrecision(unsigned index) const {
    if (index >= 8 || !positionSharing() || !positionPotential()) return 0;
    return stock::getPositionPrecisionForChannel(PositionChannels(*this), static_cast<uint8_t>(index));
}

namespace {
// Channels::isWellKnownChannel checks a preset name and the stored PSK.
// ZT's primary journal expands the single-byte public key indexes on save.
bool wellKnownChannel(const mt::Config &cfg, unsigned index, const mt::ChannelRecord &channel)
{
    const auto &family = bench::MtNodeRecord::PublicKey;
    if (channel.keySize() > 1 &&
        !(index == 0 && channel.keySize() == sizeof(family) &&
          !memcmp(channel.key(), family, sizeof(family) - 1)))
        return false;

    const char *name = channel.name()[0] ? channel.name() : cfg.modemName();
    for (unsigned i = _meshtastic_Config_LoRaConfig_ModemPreset_MIN;
         i <= _meshtastic_Config_LoRaConfig_ModemPreset_MAX; ++i) {
        const char *preset = mt::Config::radioPresetName(i);
        if (name && preset && !strcmp(name, preset)) return true;
    }
    return false;
}

unsigned ingressPrecision(const mt::Config &cfg, unsigned index, const mt::ChannelRecord &channel)
{
    if (cfg.isPositionEventChannel(index)) return 0;
    const unsigned bits = channel.precision();
    return bits > mt::Config::PublicPositionPrecision && mt::PositionChannels(cfg).usesPublicKey(index)
               ? mt::Config::PublicPositionPrecision : bits;
}
}

bool mt::Config::trafficPositionChannel(unsigned index, unsigned &precision) const
{
    precision = 0;
    ChannelRecord channel;
    if (!channelRecord(index, channel) || !channel.role() || !wellKnownChannel(*this, index, channel))
        return false;
    precision = ingressPrecision(*this, index, channel);
    return true;
}

unsigned mt::Config::relayPositionPrecision(unsigned index) const
{
    // ZT suppresses over-precise transits instead of rewriting a sender's bytes.
    // The legacy relay policy exempts licensed, unconfigured and event channels.
    ChannelRecord channel;
    if (licensedOperation() || !channelRecord(index, channel) || !channel.role() ||
        !channel.precision() || isPositionEventChannel(index))
        return 32;
#ifndef USERPREFS_TMM_APPLY_TO_PRIVATE_CHANNELS
    if (!wellKnownChannel(*this, index, channel)) return 32;
#endif
    return ingressPrecision(*this, index, channel);
}
