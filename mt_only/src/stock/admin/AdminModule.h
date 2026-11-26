// SPDX-License-Identifier: GPL-3.0-only
// From Meshtastic 8e6a88d, src/modules/AdminModule.cpp.
#pragma once
#include "meshtastic/admin.pb.h"

namespace mt::stock::admin {
inline pb_size_t responseForRequest(pb_size_t requestVariant)
{
    switch (requestVariant) {
    case meshtastic_AdminMessage_get_channel_request_tag:
        return meshtastic_AdminMessage_get_channel_response_tag;
    case meshtastic_AdminMessage_get_owner_request_tag:
        return meshtastic_AdminMessage_get_owner_response_tag;
    case meshtastic_AdminMessage_get_config_request_tag:
        return meshtastic_AdminMessage_get_config_response_tag;
    case meshtastic_AdminMessage_get_module_config_request_tag:
        return meshtastic_AdminMessage_get_module_config_response_tag;
    case meshtastic_AdminMessage_get_canned_message_module_messages_request_tag:
        return meshtastic_AdminMessage_get_canned_message_module_messages_response_tag;
    case meshtastic_AdminMessage_get_device_metadata_request_tag:
        return meshtastic_AdminMessage_get_device_metadata_response_tag;
    case meshtastic_AdminMessage_get_ringtone_request_tag:
        return meshtastic_AdminMessage_get_ringtone_response_tag;
    case meshtastic_AdminMessage_get_device_connection_status_request_tag:
        return meshtastic_AdminMessage_get_device_connection_status_response_tag;
    case meshtastic_AdminMessage_get_node_remote_hardware_pins_request_tag:
        return meshtastic_AdminMessage_get_node_remote_hardware_pins_response_tag;
    case meshtastic_AdminMessage_get_ui_config_request_tag:
        return meshtastic_AdminMessage_get_ui_config_response_tag;
    default:
        return 0;
    }
}
}
