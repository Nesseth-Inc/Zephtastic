#include "RemoteAdminRedaction.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <type_traits>

using namespace mt::admin::remote_redaction;

namespace {

void fillBytes(pb_byte_t *bytes, std::size_t size, pb_byte_t value) {
    std::memset(bytes, value, size);
}

void testNetworkRedactionPreservesNonSecretFields() {
    meshtastic_Config config{};
    config.which_payload_variant = meshtastic_Config_network_tag;
    auto &network = config.payload_variant.network;
    network.wifi_enabled = true;
    std::strcpy(network.wifi_ssid, "synthetic-network");
    std::strcpy(network.wifi_psk, "synthetic-wifi-secret");
    std::strcpy(network.ntp_server, "synthetic.ntp.invalid");
    network.eth_enabled = true;

    meshtastic_Config expected = config;
    std::memset(expected.payload_variant.network.wifi_psk, 0,
                sizeof(expected.payload_variant.network.wifi_psk));
    std::memcpy(expected.payload_variant.network.wifi_psk, ReservedSecret,
                sizeof(ReservedSecret));

    assert(redactRemoteWifiPsk(config));
    assert(std::memcmp(&config, &expected, sizeof(config)) == 0);
    assert(std::strcmp(config.payload_variant.network.wifi_psk, "sekrit") == 0);
}

void testMqttRedactionPreservesNonSecretFields() {
    meshtastic_ModuleConfig config{};
    config.which_payload_variant = meshtastic_ModuleConfig_mqtt_tag;
    auto &mqtt = config.payload_variant.mqtt;
    mqtt.enabled = true;
    std::strcpy(mqtt.address, "mqtt.synthetic.invalid");
    std::strcpy(mqtt.username, "synthetic-user");
    std::strcpy(mqtt.password, "synthetic-mqtt-secret");
    mqtt.encryption_enabled = true;
    mqtt.json_enabled = true;

    meshtastic_ModuleConfig expected = config;
    std::memset(expected.payload_variant.mqtt.password, 0,
                sizeof(expected.payload_variant.mqtt.password));
    std::memcpy(expected.payload_variant.mqtt.password, ReservedSecret,
                sizeof(ReservedSecret));

    assert(redactRemoteMqttPassword(config));
    assert(std::memcmp(&config, &expected, sizeof(config)) == 0);
    assert(std::strcmp(config.payload_variant.mqtt.password, "sekrit") == 0);
}

void testSecurityRedactionPreservesPublicIdentityAndAdminAllowlist() {
    meshtastic_Config config{};
    config.which_payload_variant = meshtastic_Config_security_tag;
    auto &security = config.payload_variant.security;
    security.public_key.size = sizeof(security.public_key.bytes);
    fillBytes(security.public_key.bytes, sizeof(security.public_key.bytes), 0x31);
    security.private_key.size = sizeof(security.private_key.bytes);
    fillBytes(security.private_key.bytes, sizeof(security.private_key.bytes), 0xA5);
    security.admin_key_count = 2;
    for (pb_size_t i = 0; i < security.admin_key_count; ++i) {
        security.admin_key[i].size = sizeof(security.admin_key[i].bytes);
        fillBytes(security.admin_key[i].bytes,
                  sizeof(security.admin_key[i].bytes),
                  static_cast<pb_byte_t>(0x41 + i));
    }
    security.is_managed = true;
    security.admin_channel_enabled = true;

    meshtastic_Config expected = config;
    std::memset(expected.payload_variant.security.private_key.bytes, 0,
                sizeof(expected.payload_variant.security.private_key.bytes));
    expected.payload_variant.security.private_key.size = 0;

    assert(redactRemoteSecurityPrivateKey(config));
    assert(std::memcmp(&config, &expected, sizeof(config)) == 0);
    assert(config.payload_variant.security.private_key.size == 0);
    for (pb_byte_t byte : config.payload_variant.security.private_key.bytes) {
        assert(byte == 0);
    }

    // Even malformed stale bytes behind a zero declared length are removed.
    // The helper handles the fixed nanopb capacity, not only the serialized size.
    auto &emptySizedSecurity = config.payload_variant.security;
    emptySizedSecurity.private_key.size = 0;
    fillBytes(emptySizedSecurity.private_key.bytes,
              sizeof(emptySizedSecurity.private_key.bytes), 0xD3);
    assert(redactRemoteSecurityPrivateKey(config));
    assert(emptySizedSecurity.private_key.size == 0);
    for (pb_byte_t byte : emptySizedSecurity.private_key.bytes) {
        assert(byte == 0);
    }
    assert(emptySizedSecurity.public_key.size == expected.payload_variant.security.public_key.size);
    assert(std::memcmp(&emptySizedSecurity.public_key,
                       &expected.payload_variant.security.public_key,
                       sizeof(emptySizedSecurity.public_key)) == 0);
    assert(emptySizedSecurity.admin_key_count == expected.payload_variant.security.admin_key_count);
    assert(std::memcmp(emptySizedSecurity.admin_key,
                       expected.payload_variant.security.admin_key,
                       sizeof(emptySizedSecurity.admin_key)) == 0);
}

void testWrongOneofIsRejectedWithoutMutation() {
    meshtastic_Config config{};
    config.which_payload_variant = meshtastic_Config_device_tag;
    config.payload_variant.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    meshtastic_Config before = config;
    assert(!redactRemoteWifiPsk(config));
    assert(!redactRemoteSecurityPrivateKey(config));
    assert(std::memcmp(&config, &before, sizeof(config)) == 0);

    meshtastic_ModuleConfig modules{};
    modules.which_payload_variant = meshtastic_ModuleConfig_serial_tag;
    modules.payload_variant.serial.enabled = true;
    meshtastic_ModuleConfig modulesBefore = modules;
    assert(!redactRemoteMqttPassword(modules));
    assert(std::memcmp(&modules, &modulesBefore, sizeof(modules)) == 0);

    config.which_payload_variant = static_cast<decltype(config.which_payload_variant)>(0x7fff);
    meshtastic_Config unknownConfigBefore = config;
    assert(!redactRemoteWifiPsk(config));
    assert(!redactRemoteSecurityPrivateKey(config));
    assert(std::memcmp(&config, &unknownConfigBefore, sizeof(config)) == 0);

    modules.which_payload_variant = static_cast<decltype(modules.which_payload_variant)>(0x7fff);
    meshtastic_ModuleConfig unknownModuleBefore = modules;
    assert(!redactRemoteMqttPassword(modules));
    assert(std::memcmp(&modules, &unknownModuleBefore, sizeof(modules)) == 0);
}

void testMalformedSecretFieldsFailClosedWithoutMutation() {
    meshtastic_Config unterminatedNetwork{};
    unterminatedNetwork.which_payload_variant = meshtastic_Config_network_tag;
    std::memset(unterminatedNetwork.payload_variant.network.wifi_psk, 'n',
                sizeof(unterminatedNetwork.payload_variant.network.wifi_psk));
    meshtastic_Config networkBefore = unterminatedNetwork;
    assert(!redactRemoteWifiPsk(unterminatedNetwork));
    assert(std::memcmp(&unterminatedNetwork, &networkBefore,
                       sizeof(unterminatedNetwork)) == 0);

    meshtastic_ModuleConfig unterminatedMqtt{};
    unterminatedMqtt.which_payload_variant = meshtastic_ModuleConfig_mqtt_tag;
    std::memset(unterminatedMqtt.payload_variant.mqtt.password, 'm',
                sizeof(unterminatedMqtt.payload_variant.mqtt.password));
    meshtastic_ModuleConfig mqttBefore = unterminatedMqtt;
    assert(!redactRemoteMqttPassword(unterminatedMqtt));
    assert(std::memcmp(&unterminatedMqtt, &mqttBefore,
                       sizeof(unterminatedMqtt)) == 0);

    meshtastic_Config malformedSecurity{};
    malformedSecurity.which_payload_variant = meshtastic_Config_security_tag;
    auto &security = malformedSecurity.payload_variant.security;
    security.private_key.size = sizeof(security.private_key.bytes) + 1;
    security.public_key.size = 0;
    security.admin_key_count = 0;
    fillBytes(security.private_key.bytes, sizeof(security.private_key.bytes), 0xC7);
    meshtastic_Config securityBefore = malformedSecurity;
    assert(!redactRemoteSecurityPrivateKey(malformedSecurity));
    assert(std::memcmp(&malformedSecurity, &securityBefore,
                       sizeof(malformedSecurity)) == 0);

    security.private_key.size = sizeof(security.private_key.bytes) - 1;
    security.public_key.size = 0;
    securityBefore = malformedSecurity;
    assert(!redactRemoteSecurityPrivateKey(malformedSecurity));
    assert(std::memcmp(&malformedSecurity, &securityBefore,
                       sizeof(malformedSecurity)) == 0);

    security.private_key.size = 0;
    security.public_key.size = sizeof(security.public_key.bytes) + 1;
    securityBefore = malformedSecurity;
    assert(!redactRemoteSecurityPrivateKey(malformedSecurity));
    assert(std::memcmp(&malformedSecurity, &securityBefore,
                       sizeof(malformedSecurity)) == 0);

    security.public_key.size = 0;
    security.admin_key_count =
        sizeof(security.admin_key) / sizeof(security.admin_key[0]) + 1;
    securityBefore = malformedSecurity;
    assert(!redactRemoteSecurityPrivateKey(malformedSecurity));
    assert(std::memcmp(&malformedSecurity, &securityBefore,
                       sizeof(malformedSecurity)) == 0);

    security.admin_key_count = 1;
    security.admin_key[0].size = sizeof(security.admin_key[0].bytes) - 1;
    securityBefore = malformedSecurity;
    assert(!redactRemoteSecurityPrivateKey(malformedSecurity));
    assert(std::memcmp(&malformedSecurity, &securityBefore,
                       sizeof(malformedSecurity)) == 0);
}

void testTooSmallReservedFieldRejected() {
    char field[sizeof(ReservedSecret) - 1] = {'x', 'x', 'x', 'x', 'x'};
    const char before[sizeof(field)] = {'x', 'x', 'x', 'x', 'x'};
    assert(!redactReservedString(field));
    assert(std::memcmp(field, before, sizeof(field)) == 0);
}

} // namespace

int main() {
    static_assert(std::is_trivially_copyable<meshtastic_Config>::value,
                  "generated config must remain POD-like");
    testNetworkRedactionPreservesNonSecretFields();
    testMqttRedactionPreservesNonSecretFields();
    testSecurityRedactionPreservesPublicIdentityAndAdminAllowlist();
    testWrongOneofIsRejectedWithoutMutation();
    testMalformedSecretFieldsFailClosedWithoutMutation();
    testTooSmallReservedFieldRejected();
}
