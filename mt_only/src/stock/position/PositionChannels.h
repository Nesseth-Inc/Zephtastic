// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../../Config.h"
#include "meshtastic/channel.pb.h"

namespace mt {
// Per-call view of the live or staged Config. No global binding or key material is retained.
class PositionChannels {
 public:
    explicit PositionChannels(const Config &config) : config_(config) {}
    uint8_t getNumChannels() const { return 8; }
    bool isEventChannel(uint8_t index) const { return config_.isPositionEventChannel(index); }

    meshtastic_Channel getByIndex(uint8_t index) const {
        meshtastic_Channel out = meshtastic_Channel_init_default;
        ChannelRecord record;
        PrimaryChannel::Selection key;
        if (config_.positionSharing() && config_.positionPotential() &&
            config_.channelRecord(index, record) && config_.channelSelection(index, key)) {
            out.role = static_cast<meshtastic_Channel_Role>(record.role());
            out.has_settings = true;
            out.settings.has_module_settings = true;
            out.settings.module_settings.position_precision = record.precision();
        }
        PrimaryChannel::clear(key);
        return out;
    }

    bool usesPublicKey(uint8_t index) const {
        // Resolve aliases and inherited keys using the same selection as transmission.
        // A short stored key can also expand to the known public family after padding.
        PrimaryChannel::Selection key;
        const bool selected = config_.channelSelection(index, key);
        const auto &family = bench::MtNodeRecord::PublicKey;
        const bool isPublic = !selected || key.licensedPlaintext ||
            (key.length == sizeof(family) && !memcmp(key.bytes, family, sizeof(family) - 1));
        PrimaryChannel::clear(key);
        return isPublic;
    }

 private:
    const Config &config_;
};
}
