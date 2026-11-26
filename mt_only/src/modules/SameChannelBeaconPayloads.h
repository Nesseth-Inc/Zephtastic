// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <string.h>
#include "meshtastic/mesh.pb.h"
#include "meshtastic/mesh_beacon.pb.h"
#include <pb_encode.h>
#include <pb_decode.h>
#include "../stock/modules/MeshBeaconModule.h"

namespace mt { namespace beacon {

// A transport check only: received offers remain advisory and are never applied.
template<class E>inline bool beaconEnum(const E &value,uint32_t maximum){
  using U=__underlying_type(E);static_assert(sizeof(U)<=sizeof(uint32_t));
  U raw{};memcpy(&raw,&value,sizeof(raw));
  return uint64_t(raw)<=maximum; // negative signed representations convert above the bound
}
inline bool canonicalPayload(const uint8_t *bytes,size_t length){
  if(!bytes||!length||length>233)return false;
  meshtastic_MeshBeacon b{};auto input=pb_istream_from_buffer(bytes,length);
  if(!pb_decode(&input,meshtastic_MeshBeacon_fields,&b)||input.bytes_left||
     strnlen(b.message,sizeof(b.message))==sizeof(b.message)||
     !beaconEnum(b.offer_region,uint32_t(_meshtastic_Config_LoRaConfig_RegionCode_MAX))||
     !beaconEnum(b.offer_preset,uint32_t(_meshtastic_Config_LoRaConfig_ModemPreset_MAX)))return false;
  if(b.has_offer_channel){
    const auto &c=b.offer_channel;
    if(strnlen(c.name,sizeof(c.name))==sizeof(c.name)||
       !(c.psk.size==0||c.psk.size==1||c.psk.size==16||c.psk.size==32)||
       (c.has_module_settings&&c.module_settings.position_precision>32))return false;
  }
  if(!b.message[0]&&!b.has_offer_channel&&!b.has_offer_preset&&!b.has_offer_frequency_slot&&b.offer_region==meshtastic_Config_LoRaConfig_RegionCode_UNSET)return false;
  uint8_t encoded[233]{};auto output=pb_ostream_from_buffer(encoded,sizeof(encoded));
  return pb_encode(&output,meshtastic_MeshBeacon_fields,&b)&&output.bytes_written==length&&!memcmp(encoded,bytes,length);
}

using BeaconPayloadBatch = stock::BeaconPayloadBatch;

inline bool composePayloads(const char (&message)[sizeof(meshtastic_MeshBeacon::message)], bool hasOffer, const meshtastic_MeshBeacon &offer,
                            bool legacySplit, BeaconPayloadBatch &out) {
    return stock::MeshBeaconBroadcastModule::sendBeacon(message, hasOffer, offer, legacySplit, out);
}

}} // namespace mt::beacon
