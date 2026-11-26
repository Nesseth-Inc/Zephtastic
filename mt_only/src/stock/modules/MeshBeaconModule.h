// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic d5f16191ca28b01b5024f590f04a67f25d66f1b1, src/modules/MeshBeaconModule.cpp
#pragma once
#include "meshtastic/mesh.pb.h"
#include "meshtastic/mesh_beacon.pb.h"
#include "meshtastic/module_config.pb.h"
#include "../../Config.h"
#include <pb_encode.h>
#include <string.h>

namespace mt::stock {

struct BeaconPayloadBatch {
    meshtastic_Data packets[2];
    uint8_t count = 0;
};

struct MeshBeaconBroadcastModule {
    // MeshBeaconBroadcastModule::runOnce. The owner supplies the active radio's
    // airtime decision and reboot-safe send history, then persists before enqueue.
    static bool enabled(const meshtastic_ModuleConfig_MeshBeaconConfig &cfg, unsigned role)
    {
        return (cfg.flags & meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_BROADCAST_ENABLED) &&
               role != meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN;
    }

    template<class History>
    static bool broadcastDue(const meshtastic_ModuleConfig_MeshBeaconConfig &cfg, unsigned role,
                             bool airTimeAllowed, History checkHistory)
    {
        return enabled(cfg,role) && airTimeAllowed && checkHistory();
    }

    // Target retuning, send-history persistence and the control queue remain with the Zephyr owner.
    static bool sendBeacon(const meshtastic_ModuleConfig_MeshBeaconConfig &bcfg, BeaconPayloadBatch &out, uint32_t homeRegion)
    {
        meshtastic_MeshBeacon offer = meshtastic_MeshBeacon_init_zero;
        if(bcfg.has_broadcast_offer_frequency_slot && bcfg.broadcast_offer_frequency_slot){
            RadioSettings::Request probe{};probe.channel=0;
            probe.regionCode=bcfg.broadcast_offer_region?uint32_t(bcfg.broadcast_offer_region):homeRegion;
            const auto *region=radio::RegionPolicy::lookup(probe.regionCode);
            probe.preset=bcfg.has_broadcast_offer_preset?uint32_t(bcfg.broadcast_offer_preset):(region?region->defaultPreset:0);
            const char *display=Config::radioPresetName(probe.preset);
            const char *name=bcfg.has_broadcast_offer_channel && bcfg.broadcast_offer_channel.name[0]?
                                bcfg.broadcast_offer_channel.name:display;
            LoRaPresets::Profile modem{};USChannelSelection derived{};
            if(!display || !RadioSettings::resolve(probe,modem,derived,name,display) ||
               bcfg.broadcast_offer_frequency_slot>derived.slotCount)
                return sendBeacon(bcfg.broadcast_message,false,offer,false,out);
            if(bcfg.broadcast_offer_frequency_slot!=derived.slot+1u){
                offer.has_offer_frequency_slot=true;
                offer.offer_frequency_slot=bcfg.broadcast_offer_frequency_slot;
            }
        }
        const bool hasRadioContent=offer.has_offer_frequency_slot || bcfg.has_broadcast_offer_preset ||
            bcfg.has_broadcast_offer_channel || bcfg.broadcast_offer_region!=meshtastic_Config_LoRaConfig_RegionCode_UNSET;
        if (bcfg.has_broadcast_offer_channel) {
            offer.has_offer_channel = true;
            offer.offer_channel = bcfg.broadcast_offer_channel;
        }
        offer.has_offer_preset = bcfg.has_broadcast_offer_preset;
        offer.offer_preset = bcfg.broadcast_offer_preset;
        offer.offer_region = bcfg.broadcast_offer_region;
        return sendBeacon(bcfg.broadcast_message, hasRadioContent, offer,
                          bcfg.flags & meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_LEGACY_SPLIT, out);
    }

    static bool sendBeacon(const char (&message)[sizeof(meshtastic_MeshBeacon::message)], bool hasRadioContent, const meshtastic_MeshBeacon &offer,
                           bool legacySplit, BeaconPayloadBatch &out)
    {
        if (message[sizeof(message) - 1] != '\0')
            return false;
        const bool hasText = message[0] != '\0';
        if (!hasText && !hasRadioContent)
            return false;

        const bool splitBoth = legacySplit && hasRadioContent && hasText;
        const bool sendOfferOnly = splitBoth || (hasRadioContent && !hasText);
        const bool sendTextOnly = splitBoth || (!hasRadioContent && hasText);
        const bool sendCombined = !legacySplit && hasRadioContent && hasText;

        // Encode the only fallible payload first, so a rejected batch leaves out unchanged.
        meshtastic_MeshBeacon beacon;
        meshtastic_Data offerPacket;
        if (sendOfferOnly || sendCombined) {
            beacon = offer;
            beacon.message[0] = '\0';
            if (sendCombined)
                memcpy(beacon.message, message, strlen(message) + 1);
            offerPacket = meshtastic_Data_init_zero;
            offerPacket.portnum = meshtastic_PortNum_MESH_BEACON_APP;
            auto stream = pb_ostream_from_buffer(offerPacket.payload.bytes, sizeof(offerPacket.payload.bytes));
            if (!pb_encode(&stream, &meshtastic_MeshBeacon_msg, &beacon))
                return false;
            offerPacket.payload.size = stream.bytes_written;
        }

        out.count = 0;
        if (sendOfferOnly || sendCombined)
            out.packets[out.count++] = offerPacket;
        if (sendTextOnly) {
            auto &p = out.packets[out.count++];
            p = meshtastic_Data_init_zero;
            p.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
            p.payload.size = strlen(message);
            memcpy(p.payload.bytes, message, p.payload.size);
        }
        return true;
    }
};

struct MeshBeaconListenerModule {
    static bool wantPacket(bool configuredToListen, unsigned port)
    {
        return configuredToListen && port == meshtastic_PortNum_MESH_BEACON_APP;
    }

    template <class Store>
    static bool handleReceivedProtobuf(meshtastic_MeshBeacon *b, Store storeOffer)
    {
        const bool hasOfferContent = b && (b->has_offer_channel || b->has_offer_frequency_slot ||
            b->offer_region != meshtastic_Config_LoRaConfig_RegionCode_UNSET || b->has_offer_preset);
        // Companion forwards the original message to the client. Only the offer enters the native cache.
        if (hasOfferContent) {
            b->message[0] = '\0';
            return storeOffer(*b);
        }
        return false;
    }
};
}
