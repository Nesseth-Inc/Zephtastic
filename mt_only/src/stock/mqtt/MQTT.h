// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d src/mqtt/MQTT.cpp. ZT supplies its admitted channel
// snapshot and publishes the returned packet via its bounded envelope queue.
#pragma once
#include <meshtastic/mesh.pb.h>
#include <string.h>

namespace mt::stock::MQTT {
inline bool suppressesUplinkPort(bool isConfiguredForDefaultServer, uint32_t port)
{
    return isConfiguredForDefaultServer && (port == meshtastic_PortNum_RANGE_TEST_APP ||
                                             port == meshtastic_PortNum_DETECTION_SENSOR_APP);
}

template<class Channel>
const meshtastic_MeshPacket *onSend(const meshtastic_MeshPacket &mp_encrypted,
                                    const meshtastic_MeshPacket &mp_decoded, const Channel &ch)
{
    if (mp_encrypted.via_mqtt || !ch.anyChannelUplinkEnabled)
        return nullptr;

    if (mp_decoded.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        bool dontUplink = !mp_decoded.decoded.has_bitfield || !(mp_decoded.decoded.bitfield & 1u);
        // ZT applies remote opt-out to every broker. Local upload also needs consent.
        if (ch.sourceIsLocal ? !ch.localUploadConsent : dontUplink)
            return nullptr;
        if (suppressesUplinkPort(ch.defaultServer, mp_decoded.decoded.portnum))
            return nullptr;
    }

    bool isPKIEncrypted = mp_encrypted.pki_encrypted || mp_decoded.pki_encrypted;
    if (!(ch.thisChannelUplinkEnabled || isPKIEncrypted))
        return nullptr;
    if (ch.encryptionEnabled)
        return &mp_encrypted;
    if (mp_decoded.which_payload_variant == meshtastic_MeshPacket_decoded_tag)
        return &mp_decoded;
    return nullptr;
}
}
