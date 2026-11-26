// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Narrow wire-shape helpers for opt-in shared-PSK named-channel Admin. These
// helpers do not authenticate the channel holder or authorize an operation.
#include "RemoteAdminMutationProtocol.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include "meshtastic/mesh.pb.h"

namespace mt::admin::legacy_protocol {

enum class Kind : uint8_t { Invalid = 0, Get, Set };

inline Kind classifyRequest(const uint8_t *data, size_t size) noexcept {
  remote_protocol::Query query{};
  if (remote_protocol::canonicalGetRequest(data, size, query)) return Kind::Get;
  mutation_protocol::Mutation mutation{};
  if (mutation_protocol::canonicalSetRequest(data, size, mutation)) return Kind::Set;
  return Kind::Invalid;
}

// The callback must return one complete canonical stock Admin response for a
// getter, or a canonical correlated Routing error/ack for a setter or failure.
// This enforces only serialization shape and correlation, not operation policy.
inline bool canonicalResponse(const uint8_t *wire, size_t length,
                             uint32_t requestPacketId) noexcept {
  if (!requestPacketId || !wire || !length || length > remote_protocol::MaxDataBytes)
    return false;
  meshtastic_Data data{};
  remote_protocol::Wipe wipeData{&data, sizeof(data)};
  uint8_t canonical[remote_protocol::MaxDataBytes]{};
  remote_protocol::Wipe wipeCanonical{canonical, sizeof(canonical)};
  if (!remote_protocol::decodeCanonicalData(wire, length, data, canonical) ||
      data.dest || data.source || data.reply_id || data.emoji ||
      data.xeddsa_signature.size || data.want_response || !data.has_bitfield ||
      (data.bitfield & ~uint8_t{0x01}) || data.request_id != requestPacketId ||
      data.payload.size == 0 || data.payload.size > remote_protocol::MaxPayloadBytes)
    return false;

  uint32_t port = 0;
  if (!remote_protocol::enumNumber(data.portnum, port)) return false;
  if (port == meshtastic_PortNum_ADMIN_APP) {
    meshtastic_AdminMessage admin{};
    remote_protocol::Wipe wipeAdmin{&admin, sizeof(admin)};
    uint8_t canonicalAdmin[remote_protocol::MaxPayloadBytes]{};
    remote_protocol::Wipe wipeCanonicalAdmin{canonicalAdmin, sizeof(canonicalAdmin)};
    if (!remote_protocol::decodeCanonicalAdmin(data.payload.bytes,
          data.payload.size, admin, canonicalAdmin) ||
        admin.session_passkey.size != remote_protocol::SessionPasskeyBytes)
      return false;
    switch (admin.which_payload_variant) {
    case meshtastic_AdminMessage_get_owner_response_tag:
    case meshtastic_AdminMessage_get_config_response_tag:
    case meshtastic_AdminMessage_get_module_config_response_tag:
    case meshtastic_AdminMessage_get_channel_response_tag:
    case meshtastic_AdminMessage_get_device_metadata_response_tag:
    case meshtastic_AdminMessage_get_device_connection_status_response_tag:
    case meshtastic_AdminMessage_get_node_remote_hardware_pins_response_tag:
    case meshtastic_AdminMessage_get_ui_config_response_tag:
    case meshtastic_AdminMessage_get_canned_message_module_messages_response_tag:
    case meshtastic_AdminMessage_get_ringtone_response_tag:
      return true;
    default:
      return false;
    }
  }
  if (port != meshtastic_PortNum_ROUTING_APP) return false;
  meshtastic_Routing routing{};
  remote_protocol::Wipe wipeRouting{&routing, sizeof(routing)};
  auto input = pb_istream_from_buffer(data.payload.bytes, data.payload.size);
  if (!pb_decode(&input, meshtastic_Routing_fields, &routing) || input.bytes_left)
    return false;
  uint8_t canonicalRouting[remote_protocol::MaxPayloadBytes]{};
  auto output = pb_ostream_from_buffer(canonicalRouting, sizeof(canonicalRouting));
  const bool encoded = pb_encode(&output, meshtastic_Routing_fields, &routing);
  const bool exact = encoded && output.bytes_written == data.payload.size &&
      memcmp(canonicalRouting, data.payload.bytes, data.payload.size) == 0;
  remote_protocol::Wipe wipeCanonicalRouting{canonicalRouting, sizeof(canonicalRouting)};
  if (!exact || routing.which_variant != meshtastic_Routing_error_reason_tag ||
      routing.ack_proof.size != 0) return false;
  uint32_t error = 0;
  return remote_protocol::enumNumber(routing.error_reason, error) &&
      (error <= 9 || (error >= 32 && error <= 39));
}

}  // namespace mt::admin::legacy_protocol
