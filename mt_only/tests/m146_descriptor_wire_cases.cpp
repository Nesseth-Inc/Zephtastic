// SPDX-License-Identifier: GPL-3.0-only
// Host-only synthetic wire vectors for M146 bounded descriptor-width candidates.
// No radio, live packet, identity, key, or device input is used.
#include <pb_decode.h>
#include <pb_encode.h>
#include <meshtastic/mesh.pb.h>
#include <meshtastic/config.pb.h>
#include <meshtastic/admin.pb.h>
#include <meshtastic/channel.pb.h>
#include <meshtastic/module_config.pb.h>
#include <meshtastic/mqtt.pb.h>
#include <meshtastic/telemetry.pb.h>
#include <meshtastic/connection_status.pb.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
template <typename Message>
bool emit(const char *caseId, const pb_msgdesc_t *fields, const Message &value) {
  uint8_t first[512]{};
  pb_ostream_t out = pb_ostream_from_buffer(first, sizeof(first));
  if (!pb_encode(&out, fields, &value)) return false;

  Message decoded{};
  pb_istream_t in = pb_istream_from_buffer(first, out.bytes_written);
  if (!pb_decode(&in, fields, &decoded) || in.bytes_left != 0) return false;

  uint8_t second[512]{};
  pb_ostream_t again = pb_ostream_from_buffer(second, sizeof(second));
  if (!pb_encode(&again, fields, &decoded) || again.bytes_written != out.bytes_written ||
      std::memcmp(first, second, out.bytes_written) != 0) return false;

  std::printf("%s ", caseId);
  for (size_t i = 0; i < out.bytes_written; ++i) std::printf("%02x", first[i]);
  std::putchar('\n');
  pb_release(fields, &decoded);
  return true;
}

template <size_t N>
void fullString(char (&dst)[N], char seed) {
  static_assert(N > 1, "nanopb string needs room for terminator");
  for (size_t i = 0; i + 1 < N; ++i) dst[i] = char(seed + (i % 20));
  dst[N - 1] = '\0';
}

bool allCases() {
  meshtastic_ModuleSettings moduleSettings{};
  if (!emit("module-settings.default", meshtastic_ModuleSettings_fields, moduleSettings)) return false;
  moduleSettings.position_precision = UINT32_MAX; moduleSettings.is_muted = true;
  if (!emit("module-settings.boundary", meshtastic_ModuleSettings_fields, moduleSettings)) return false;

  meshtastic_NetworkConnectionStatus connection{};
  connection.ip_address = UINT32_MAX;
  connection.is_connected = connection.is_mqtt_connected = connection.is_syslog_connected = true;
  meshtastic_WifiConnectionStatus wifi{};
  if (!emit("wifi-status.default", meshtastic_WifiConnectionStatus_fields, wifi)) return false;
  wifi.has_status = true; wifi.status = connection; fullString(wifi.ssid, 'W'); wifi.rssi = INT32_MIN;
  if (!emit("wifi-status.boundary", meshtastic_WifiConnectionStatus_fields, wifi)) return false;

  meshtastic_EthernetConnectionStatus ethernet{};
  if (!emit("ethernet-status.default", meshtastic_EthernetConnectionStatus_fields, ethernet)) return false;
  ethernet.has_status = true; ethernet.status = connection;
  if (!emit("ethernet-status.boundary", meshtastic_EthernetConnectionStatus_fields, ethernet)) return false;

  meshtastic_DeviceConnectionStatus deviceConnection{};
  if (!emit("device-connection.default", meshtastic_DeviceConnectionStatus_fields, deviceConnection)) return false;
  deviceConnection.has_wifi = deviceConnection.has_ethernet = true;
  deviceConnection.wifi = wifi; deviceConnection.ethernet = ethernet;
  deviceConnection.has_bluetooth = true; deviceConnection.bluetooth.pin = UINT32_MAX;
  deviceConnection.bluetooth.rssi = INT32_MIN; deviceConnection.bluetooth.is_connected = true;
  deviceConnection.has_serial = true; deviceConnection.serial.baud = UINT32_MAX;
  deviceConnection.serial.is_connected = true;
  if (!emit("device-connection.boundary", meshtastic_DeviceConnectionStatus_fields, deviceConnection)) return false;

  meshtastic_Config_PowerConfig powerConfig{};
  if (!emit("config-power.default", meshtastic_Config_PowerConfig_fields, powerConfig)) return false;
  powerConfig.is_power_saving = true; powerConfig.on_battery_shutdown_after_secs = UINT32_MAX;
  powerConfig.adc_multiplier_override = 3.25f; powerConfig.powermon_enables = UINT64_MAX;
  if (!emit("config-power.boundary", meshtastic_Config_PowerConfig_fields, powerConfig)) return false;

  meshtastic_Config_DisplayConfig display{};
  if (!emit("config-display.default", meshtastic_Config_DisplayConfig_fields, display)) return false;
  display.screen_on_secs = UINT32_MAX; display.gps_format = meshtastic_Config_DisplayConfig_DeprecatedGpsCoordinateFormat_UNUSED;
  display.auto_screen_carousel_secs = UINT32_MAX; display.compass_north_top = display.flip_screen = true;
  display.units = meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL;
  display.oled = meshtastic_Config_DisplayConfig_OledType_OLED_AUTO;
  display.displaymode = meshtastic_Config_DisplayConfig_DisplayMode_COLOR;
  display.heading_bold = display.wake_on_tap_or_motion = true;
  display.compass_orientation = meshtastic_Config_DisplayConfig_CompassOrientation_DEGREES_270_INVERTED;
  display.use_12h_clock = display.use_long_node_name = display.enable_message_bubbles = true;
  if (!emit("config-display.boundary", meshtastic_Config_DisplayConfig_fields, display)) return false;

  meshtastic_Config_LoRaConfig lora{};
  if (!emit("config-lora.default", meshtastic_Config_LoRaConfig_fields, lora)) return false;
  lora.use_preset = true; lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
  lora.bandwidth = UINT16_MAX; lora.spread_factor = UINT32_MAX; lora.coding_rate = UINT8_MAX;
  lora.frequency_offset = 125.5f; lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
  lora.hop_limit = UINT32_MAX; lora.tx_enabled = true; lora.tx_power = INT8_MIN;
  lora.channel_num = UINT16_MAX; lora.override_duty_cycle = lora.sx126x_rx_boosted_gain = true;
  lora.override_frequency = 915.0f; lora.pa_fan_disabled = true;
  lora.ignore_incoming_count = 3; lora.ignore_incoming[0] = UINT32_MAX;
  lora.ignore_incoming[1] = UINT32_MAX - 1; lora.ignore_incoming[2] = UINT32_MAX - 2;
  lora.ignore_mqtt = lora.config_ok_to_mqtt = true;
  lora.fem_lna_mode = meshtastic_Config_LoRaConfig_FEM_LNA_Mode_NOT_PRESENT;
  lora.serial_hal_only = true;
  if (!emit("config-lora.boundary", meshtastic_Config_LoRaConfig_fields, lora)) return false;

  meshtastic_Config_SecurityConfig security{};
  if (!emit("config-security.default", meshtastic_Config_SecurityConfig_fields, security)) return false;
  security.public_key.size = sizeof(security.public_key.bytes);
  security.private_key.size = sizeof(security.private_key.bytes);
  for (size_t i = 0; i < security.public_key.size; ++i) {
    security.public_key.bytes[i] = uint8_t(0x10 + i);
    security.private_key.bytes[i] = uint8_t(0x80 + i);
  }
  security.admin_key_count = 3;
  for (size_t k = 0; k < security.admin_key_count; ++k) {
    security.admin_key[k].size = sizeof(security.admin_key[k].bytes);
    for (size_t i = 0; i < security.admin_key[k].size; ++i)
      security.admin_key[k].bytes[i] = uint8_t(k * 37 + i);
  }
  security.is_managed = security.serial_enabled = security.debug_log_api_enabled = true;
  security.admin_channel_enabled = true;
  security.packet_signature_policy = meshtastic_Config_SecurityConfig_PacketSignaturePolicy_PACKET_SIGNATURE_POLICY_STRICT;
  if (!emit("config-security.boundary", meshtastic_Config_SecurityConfig_fields, security)) return false;

  meshtastic_Config_SessionkeyConfig sessionkey{};
  if (!emit("config-sessionkey.default", meshtastic_Config_SessionkeyConfig_fields, sessionkey)) return false;
  sessionkey.dummy_field = 'x';
  if (!emit("config-sessionkey.boundary", meshtastic_Config_SessionkeyConfig_fields, sessionkey)) return false;

  meshtastic_AdminMessage admin{};
  if (!emit("admin.default", meshtastic_AdminMessage_fields, admin)) return false;
  admin.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
  admin.set_config.which_payload_variant = meshtastic_Config_security_tag;
  admin.set_config.payload_variant.security = security;
  admin.session_passkey.size = 8;
  for (size_t i = 0; i < admin.session_passkey.size; ++i)
    admin.session_passkey.bytes[i] = uint8_t(0xA0 + i);
  if (!emit("admin.synthetic-passkey-config-security", meshtastic_AdminMessage_fields, admin)) return false;

  meshtastic_ChannelSettings channelSettings{};
  if (!emit("channel-settings.default", meshtastic_ChannelSettings_fields, channelSettings)) return false;
  channelSettings.channel_num = UINT32_MAX; channelSettings.psk.size = 32;
  for (size_t i = 0; i < channelSettings.psk.size; ++i) channelSettings.psk.bytes[i] = uint8_t(i * 7);
  fullString(channelSettings.name, 'C'); channelSettings.id = UINT32_MAX;
  channelSettings.uplink_enabled = channelSettings.downlink_enabled = true;
  channelSettings.has_module_settings = true; channelSettings.module_settings.position_precision = UINT32_MAX;
  channelSettings.module_settings.is_muted = true; channelSettings.use_aead = true;
  if (!emit("channel-settings.boundary", meshtastic_ChannelSettings_fields, channelSettings)) return false;

  meshtastic_Channel channel{};
  if (!emit("channel.default", meshtastic_Channel_fields, channel)) return false;
  channel.index = INT8_MAX; channel.has_settings = true; channel.settings = channelSettings;
  channel.role = meshtastic_Channel_Role_PRIMARY;
  if (!emit("channel.boundary", meshtastic_Channel_fields, channel)) return false;

  meshtastic_Config_NetworkConfig networkConfig{};
  if (!emit("config-network.default", meshtastic_Config_NetworkConfig_fields, networkConfig)) return false;
  networkConfig.wifi_enabled = networkConfig.eth_enabled = true;
  fullString(networkConfig.wifi_ssid, 's'); fullString(networkConfig.wifi_psk, 'p');
  fullString(networkConfig.ntp_server, 'n');
  networkConfig.address_mode = meshtastic_Config_NetworkConfig_AddressMode_STATIC;
  networkConfig.has_ipv4_config = true;
  networkConfig.ipv4_config.ip = networkConfig.ipv4_config.gateway = UINT32_MAX;
  networkConfig.ipv4_config.subnet = 0xFFFFFF00u; networkConfig.ipv4_config.dns = 0x08080808u;
  fullString(networkConfig.rsyslog_server, 'r'); networkConfig.enabled_protocols = UINT32_MAX;
  networkConfig.ipv6_enabled = true;
  if (!emit("config-network.boundary", meshtastic_Config_NetworkConfig_fields, networkConfig)) return false;

  meshtastic_Config config{};
  if (!emit("config.default", meshtastic_Config_fields, config)) return false;
  config.which_payload_variant = meshtastic_Config_network_tag;
  config.payload_variant.network = networkConfig;
  if (!emit("config.network-nested", meshtastic_Config_fields, config)) return false;

  meshtastic_ModuleConfig_MQTTConfig mqtt{};
  if (!emit("mqtt-config.default", meshtastic_ModuleConfig_MQTTConfig_fields, mqtt)) return false;
  mqtt.enabled = mqtt.encryption_enabled = mqtt.json_enabled = mqtt.tls_enabled = true;
  fullString(mqtt.address, 'a'); fullString(mqtt.username, 'u'); fullString(mqtt.password, 'x');
  fullString(mqtt.root, 't'); mqtt.proxy_to_client_enabled = mqtt.map_reporting_enabled = true;
  mqtt.has_map_report_settings = true; mqtt.map_report_settings.publish_interval_secs = UINT32_MAX;
  mqtt.map_report_settings.position_precision = UINT32_MAX;
  mqtt.map_report_settings.should_report_location = true;
  if (!emit("mqtt-config.boundary", meshtastic_ModuleConfig_MQTTConfig_fields, mqtt)) return false;

  meshtastic_ModuleConfig moduleConfig{};
  if (!emit("module-config.default", meshtastic_ModuleConfig_fields, moduleConfig)) return false;
  moduleConfig.which_payload_variant = meshtastic_ModuleConfig_mqtt_tag;
  moduleConfig.payload_variant.mqtt = mqtt;
  if (!emit("module-config.mqtt-nested", meshtastic_ModuleConfig_fields, moduleConfig)) return false;

  meshtastic_Data data{};
  if (!emit("data.default", meshtastic_Data_fields, data)) return false;
  data.portnum = meshtastic_PortNum_ADMIN_APP;
  data.payload.size = sizeof(data.payload.bytes);
  for (size_t i = 0; i < data.payload.size; ++i) data.payload.bytes[i] = uint8_t(0xE7 - i);
  data.want_response = true; data.dest = UINT32_MAX; data.source = UINT32_MAX - 1;
  data.request_id = UINT32_MAX - 2; data.reply_id = UINT32_MAX - 3; data.emoji = UINT32_MAX;
  data.has_bitfield = true; data.bitfield = UINT8_MAX;
  data.xeddsa_signature.size = sizeof(data.xeddsa_signature.bytes);
  for (size_t i = 0; i < data.xeddsa_signature.size; ++i) data.xeddsa_signature.bytes[i] = uint8_t(i ^ 0xA5);
  if (!emit("data.boundary", meshtastic_Data_fields, data)) return false;

  meshtastic_MeshPacket packet{};
  if (!emit("mesh-packet.default", meshtastic_MeshPacket_fields, packet)) return false;
  packet.from = UINT32_MAX; packet.to = UINT32_MAX - 1; packet.channel = UINT8_MAX;
  packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
  packet.decoded = data; packet.id = UINT32_MAX; packet.has_rx_time = true;
  packet.rx_time = UINT32_MAX; packet.rx_snr = -127.5f; packet.hop_limit = UINT8_MAX;
  packet.want_ack = true; packet.priority = meshtastic_MeshPacket_Priority_ACK;
  if (!emit("mesh-packet.decoded-admin", meshtastic_MeshPacket_fields, packet)) return false;

  meshtastic_FromRadio fromRadio{};
  if (!emit("from-radio.default", meshtastic_FromRadio_fields, fromRadio)) return false;
  fromRadio.id = UINT32_MAX; fromRadio.which_payload_variant = meshtastic_FromRadio_packet_tag;
  fromRadio.packet = packet;
  if (!emit("from-radio.packet-nested", meshtastic_FromRadio_fields, fromRadio)) return false;

  meshtastic_ToRadio toRadio{};
  if (!emit("to-radio.default", meshtastic_ToRadio_fields, toRadio)) return false;
  toRadio.which_payload_variant = meshtastic_ToRadio_packet_tag; toRadio.packet = packet;
  if (!emit("to-radio.packet-nested", meshtastic_ToRadio_fields, toRadio)) return false;

  meshtastic_ServiceEnvelope envelope{};
  if (!emit("service-envelope.default", meshtastic_ServiceEnvelope_fields, envelope)) return false;
  meshtastic_MeshPacket envelopePacket = packet;
  char channelId[] = "synth-channel-4294967295";
  char gatewayId[] = "!4294967295";
  envelope.packet = &envelopePacket; envelope.channel_id = channelId; envelope.gateway_id = gatewayId;
  if (!emit("service-envelope.nested-admin", meshtastic_ServiceEnvelope_fields, envelope)) return false;

  meshtastic_Config_DeviceConfig device{};
  if (!emit("config-device.default", meshtastic_Config_DeviceConfig_fields, device)) return false;
  device.role = meshtastic_Config_DeviceConfig_Role_CLIENT_BASE;
  device.serial_enabled = true; device.button_gpio = UINT32_MAX; device.buzzer_gpio = UINT32_MAX;
  device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY;
  device.node_info_broadcast_secs = UINT32_MAX; device.double_tap_as_button_press = true;
  device.is_managed = true; device.disable_triple_click = true;
  fullString(device.tzdef, 'T'); device.led_heartbeat_disabled = true;
  device.buzzer_mode = meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED;
  if (!emit("config-device.boundary", meshtastic_Config_DeviceConfig_fields, device)) return false;

  meshtastic_Config_PositionConfig positionConfig{};
  if (!emit("config-position.default", meshtastic_Config_PositionConfig_fields, positionConfig)) return false;
  positionConfig.position_broadcast_secs = UINT32_MAX; positionConfig.position_broadcast_smart_enabled = true;
  positionConfig.fixed_position = positionConfig.gps_enabled = true;
  positionConfig.gps_update_interval = positionConfig.gps_attempt_time = UINT32_MAX;
  positionConfig.position_flags = positionConfig.rx_gpio = positionConfig.tx_gpio = UINT32_MAX;
  positionConfig.broadcast_smart_minimum_distance = UINT32_MAX;
  positionConfig.broadcast_smart_minimum_interval_secs = UINT32_MAX;
  positionConfig.gps_en_gpio = UINT32_MAX;
  positionConfig.gps_mode = meshtastic_Config_PositionConfig_GpsMode_ENABLED;
  if (!emit("config-position.boundary", meshtastic_Config_PositionConfig_fields, positionConfig)) return false;

  meshtastic_Config_NetworkConfig_IpV4Config ipv4{};
  if (!emit("config-ipv4.default", meshtastic_Config_NetworkConfig_IpV4Config_fields, ipv4)) return false;
  ipv4.ip = ipv4.gateway = ipv4.subnet = ipv4.dns = UINT32_MAX;
  if (!emit("config-ipv4.boundary", meshtastic_Config_NetworkConfig_IpV4Config_fields, ipv4)) return false;

  meshtastic_Config_BluetoothConfig bluetooth{};
  if (!emit("config-bluetooth.default", meshtastic_Config_BluetoothConfig_fields, bluetooth)) return false;
  bluetooth.enabled = true; bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN;
  bluetooth.fixed_pin = UINT32_MAX;
  if (!emit("config-bluetooth.boundary", meshtastic_Config_BluetoothConfig_fields, bluetooth)) return false;

  meshtastic_DeviceMetrics deviceMetrics{};
  if (!emit("device-metrics.default", meshtastic_DeviceMetrics_fields, deviceMetrics)) return false;
  deviceMetrics.has_battery_level = deviceMetrics.has_voltage = deviceMetrics.has_channel_utilization = true;
  deviceMetrics.battery_level = UINT32_MAX; deviceMetrics.voltage = 12.5f;
  deviceMetrics.channel_utilization = 0.75f;
  deviceMetrics.has_air_util_tx = deviceMetrics.has_uptime_seconds = true;
  deviceMetrics.air_util_tx = 0.5f; deviceMetrics.uptime_seconds = UINT32_MAX;
  if (!emit("device-metrics.boundary", meshtastic_DeviceMetrics_fields, deviceMetrics)) return false;

  meshtastic_SoilWaterMetrics soil{};
  if (!emit("soil-water.default", meshtastic_SoilWaterMetrics_fields, soil)) return false;
  soil.has_soil_ph = soil.has_solar_irradiance = true;
  soil.soil_ph = 6.25f; soil.solar_irradiance = 999.0f;
  if (!emit("soil-water.boundary", meshtastic_SoilWaterMetrics_fields, soil)) return false;

  meshtastic_PowerMetrics power{};
  if (!emit("power-metrics.default", meshtastic_PowerMetrics_fields, power)) return false;
  power.has_ch1_voltage = power.has_ch8_current = true;
  power.ch1_voltage = 24.0f; power.ch8_current = -12.5f;
  if (!emit("power-metrics.boundary", meshtastic_PowerMetrics_fields, power)) return false;

  meshtastic_AirQualityMetrics air{};
  if (!emit("air-quality.default", meshtastic_AirQualityMetrics_fields, air)) return false;
  air.has_pm10_standard = air.has_pm_status_flags = true;
  air.pm10_standard = UINT32_MAX; air.pm_status_flags = UINT32_MAX;
  if (!emit("air-quality.boundary", meshtastic_AirQualityMetrics_fields, air)) return false;

  meshtastic_LocalStats localStats{};
  if (!emit("local-stats.default", meshtastic_LocalStats_fields, localStats)) return false;
  localStats.uptime_seconds = localStats.num_packets_tx = UINT32_MAX;
  localStats.noise_floor = INT32_MIN; localStats.channel_utilization = 0.875f;
  if (!emit("local-stats.boundary", meshtastic_LocalStats_fields, localStats)) return false;

  meshtastic_TrafficManagementStats traffic{};
  if (!emit("traffic-stats.default", meshtastic_TrafficManagementStats_fields, traffic)) return false;
  traffic.packets_inspected = traffic.router_hops_preserved = UINT32_MAX;
  if (!emit("traffic-stats.boundary", meshtastic_TrafficManagementStats_fields, traffic)) return false;

  meshtastic_HealthMetrics health{};
  if (!emit("health-metrics.default", meshtastic_HealthMetrics_fields, health)) return false;
  health.has_heart_bpm = health.has_spO2 = health.has_temperature = true;
  health.heart_bpm = UINT8_MAX; health.spO2 = UINT8_MAX; health.temperature = 37.25f;
  if (!emit("health-metrics.boundary", meshtastic_HealthMetrics_fields, health)) return false;

  meshtastic_HostMetrics host{};
  if (!emit("host-metrics.default", meshtastic_HostMetrics_fields, host)) return false;
  host.uptime_seconds = UINT32_MAX; host.freemem_bytes = UINT64_MAX;
  host.has_diskfree3_bytes = true; host.diskfree3_bytes = UINT64_MAX;
  fullString(host.user_string, 'h');
  if (!emit("host-metrics.boundary", meshtastic_HostMetrics_fields, host)) return false;

  meshtastic_NetworkConnectionStatus network{};
  if (!emit("network-status.default", meshtastic_NetworkConnectionStatus_fields, network)) return false;
  network.ip_address = UINT32_MAX;
  network.is_connected = network.is_mqtt_connected = network.is_syslog_connected = true;
  if (!emit("network-status.boundary", meshtastic_NetworkConnectionStatus_fields, network)) return false;

  meshtastic_BluetoothConnectionStatus btStatus{};
  if (!emit("bluetooth-status.default", meshtastic_BluetoothConnectionStatus_fields, btStatus)) return false;
  btStatus.pin = UINT32_MAX; btStatus.rssi = INT32_MIN; btStatus.is_connected = true;
  if (!emit("bluetooth-status.boundary", meshtastic_BluetoothConnectionStatus_fields, btStatus)) return false;

  meshtastic_SerialConnectionStatus serial{};
  if (!emit("serial-status.default", meshtastic_SerialConnectionStatus_fields, serial)) return false;
  serial.baud = UINT32_MAX; serial.is_connected = true;
  if (!emit("serial-status.boundary", meshtastic_SerialConnectionStatus_fields, serial)) return false;

  meshtastic_Position position{};
  if (!emit("position.default", meshtastic_Position_fields, position)) return false;
  position.has_latitude_i = true;
  position.latitude_i = INT32_MIN;
  position.has_longitude_i = true;
  position.longitude_i = INT32_MAX;
  position.has_altitude = true;
  position.altitude = INT32_MIN;
  position.time = UINT32_MAX;
  position.location_source = meshtastic_Position_LocSource_LOC_EXTERNAL;
  position.altitude_source = meshtastic_Position_AltSource_ALT_BAROMETRIC;
  position.timestamp = UINT32_MAX;
  position.timestamp_millis_adjust = INT32_MIN;
  position.has_altitude_hae = true;
  position.altitude_hae = INT32_MAX;
  position.has_altitude_geoidal_separation = true;
  position.altitude_geoidal_separation = INT32_MIN;
  position.PDOP = UINT32_MAX; position.HDOP = UINT32_MAX; position.VDOP = UINT32_MAX;
  position.gps_accuracy = UINT32_MAX;
  position.has_ground_speed = true; position.ground_speed = UINT32_MAX;
  position.has_ground_track = true; position.ground_track = UINT32_MAX;
  position.fix_quality = UINT32_MAX; position.fix_type = UINT32_MAX;
  position.sats_in_view = UINT32_MAX; position.sensor_id = UINT32_MAX;
  position.next_update = UINT32_MAX; position.seq_number = UINT32_MAX;
  position.precision_bits = UINT32_MAX;
  if (!emit("position.boundary", meshtastic_Position_fields, position)) return false;

  meshtastic_User user{};
  if (!emit("user.default", meshtastic_User_fields, user)) return false;
  fullString(user.id, 'i'); fullString(user.long_name, 'L'); fullString(user.short_name, 's');
  for (size_t i = 0; i < sizeof(user.macaddr); ++i) user.macaddr[i] = uint8_t(0xA0 + i);
  user.hw_model = meshtastic_HardwareModel_PRIVATE_HW;
  user.is_licensed = true;
  user.role = meshtastic_Config_DeviceConfig_Role_CLIENT_BASE;
  user.public_key.size = sizeof(user.public_key.bytes);
  for (size_t i = 0; i < user.public_key.size; ++i) user.public_key.bytes[i] = uint8_t(i);
  user.has_is_unmessagable = true; user.is_unmessagable = true;
  if (!emit("user.boundary", meshtastic_User_fields, user)) return false;

  meshtastic_KeyVerification keyVerification{};
  if (!emit("key-verification.default", meshtastic_KeyVerification_fields, keyVerification)) return false;
  keyVerification.nonce = UINT64_MAX;
  keyVerification.hash1.size = sizeof(keyVerification.hash1.bytes);
  keyVerification.hash2.size = sizeof(keyVerification.hash2.bytes);
  for (size_t i = 0; i < keyVerification.hash1.size; ++i) {
    keyVerification.hash1.bytes[i] = uint8_t(0x20 + i);
    keyVerification.hash2.bytes[i] = uint8_t(0x80 + i);
  }
  if (!emit("key-verification.boundary", meshtastic_KeyVerification_fields, keyVerification)) return false;

  meshtastic_BoundingBox box{};
  if (!emit("bounding-box.default", meshtastic_BoundingBox_fields, box)) return false;
  box.longitude_west_i = INT32_MIN; box.latitude_south_i = -1;
  box.longitude_east_i = 1; box.latitude_north_i = INT32_MAX;
  if (!emit("bounding-box.boundary", meshtastic_BoundingBox_fields, box)) return false;

  meshtastic_StatusMessage status{};
  if (!emit("status.default", meshtastic_StatusMessage_fields, status)) return false;
  fullString(status.status, 'S');
  if (!emit("status.boundary", meshtastic_StatusMessage_fields, status)) return false;

  meshtastic_DeviceMetadata metadata{};
  if (!emit("device-metadata.default", meshtastic_DeviceMetadata_fields, metadata)) return false;
  fullString(metadata.firmware_version, 'v');
  metadata.device_state_version = UINT32_MAX;
  metadata.canShutdown = metadata.hasWifi = metadata.hasBluetooth = metadata.hasEthernet = true;
  metadata.role = meshtastic_Config_DeviceConfig_Role_CLIENT_BASE;
  metadata.position_flags = UINT32_MAX;
  metadata.hw_model = meshtastic_HardwareModel_PRIVATE_HW;
  metadata.hasRemoteHardware = metadata.hasPKC = metadata.has_xeddsa = true;
  metadata.excluded_modules = UINT32_MAX;
  if (!emit("device-metadata.boundary", meshtastic_DeviceMetadata_fields, metadata)) return false;

  meshtastic_FileInfo file{};
  if (!emit("file-info.default", meshtastic_FileInfo_fields, file)) return false;
  fullString(file.file_name, 'f'); file.size_bytes = UINT32_MAX;
  if (!emit("file-info.boundary", meshtastic_FileInfo_fields, file)) return false;

  meshtastic_Compressed compressed{};
  if (!emit("compressed.default", meshtastic_Compressed_fields, compressed)) return false;
  compressed.portnum = meshtastic_PortNum_ADMIN_APP;
  compressed.data.size = sizeof(compressed.data.bytes);
  for (size_t i = 0; i < compressed.data.size; ++i) compressed.data.bytes[i] = uint8_t(i * 17u);
  if (!emit("compressed.boundary", meshtastic_Compressed_fields, compressed)) return false;

  meshtastic_RemoteShell shell{};
  if (!emit("remote-shell.default", meshtastic_RemoteShell_fields, shell)) return false;
  shell.op = meshtastic_RemoteShell_OpCode_PONG;
  shell.session_id = shell.seq = shell.ack_seq = UINT32_MAX;
  shell.payload.size = sizeof(shell.payload.bytes);
  for (size_t i = 0; i < shell.payload.size; ++i) shell.payload.bytes[i] = uint8_t(255u - i);
  shell.cols = shell.rows = shell.flags = shell.last_tx_seq = shell.last_rx_seq = UINT32_MAX;
  if (!emit("remote-shell.boundary", meshtastic_RemoteShell_fields, shell)) return false;

  meshtastic_ChunkedPayload chunked{};
  if (!emit("chunked-payload.default", meshtastic_ChunkedPayload_fields, chunked)) return false;
  chunked.payload_id = UINT32_MAX; chunked.chunk_count = UINT16_MAX; chunked.chunk_index = UINT16_MAX;
  chunked.payload_chunk.size = sizeof(chunked.payload_chunk.bytes);
  for (size_t i = 0; i < chunked.payload_chunk.size; ++i) chunked.payload_chunk.bytes[i] = uint8_t(i ^ 0x5A);
  if (!emit("chunked-payload.boundary", meshtastic_ChunkedPayload_fields, chunked)) return false;

  meshtastic_LoRaRegionPresets region{};
  if (!emit("lora-region-presets.default", meshtastic_LoRaRegionPresets_fields, region)) return false;
  region.region = meshtastic_Config_LoRaConfig_RegionCode_LORA_24;
  region.group_index = UINT8_MAX;
  if (!emit("lora-region-presets.boundary", meshtastic_LoRaRegionPresets_fields, region)) return false;
  meshtastic_QueueStatus q{};
  if (!emit("queue.default", meshtastic_QueueStatus_fields, q)) return false;
  q.res = INT8_MIN;
  q.free = UINT8_MAX;
  q.maxlen = UINT8_MAX;
  q.mesh_packet_id = UINT32_MAX;
  if (!emit("queue.boundary", meshtastic_QueueStatus_fields, q)) return false;

  meshtastic_LockdownStatus lock{};
  if (!emit("lockdown.default", meshtastic_LockdownStatus_fields, lock)) return false;
  lock.state = meshtastic_LockdownStatus_State_NEEDS_PROVISION;
  fullString(lock.lock_reason, 'a');
  lock.boots_remaining = UINT32_MAX;
  lock.valid_until_epoch = UINT32_MAX;
  lock.backoff_seconds = UINT32_MAX;
  if (!emit("lockdown.enum-low", meshtastic_LockdownStatus_fields, lock)) return false;
  lock.state = meshtastic_LockdownStatus_State_DISABLED;
  if (!emit("lockdown.enum-high", meshtastic_LockdownStatus_fields, lock)) return false;

  meshtastic_KeyVerificationNumberInform inform{};
  if (!emit("key-inform.default", meshtastic_KeyVerificationNumberInform_fields, inform)) return false;
  inform.nonce = UINT64_MAX;
  fullString(inform.remote_longname, 'A');
  inform.security_number = UINT32_MAX;
  if (!emit("key-inform.boundary", meshtastic_KeyVerificationNumberInform_fields, inform)) return false;

  meshtastic_KeyVerificationNumberRequest request{};
  if (!emit("key-request.default", meshtastic_KeyVerificationNumberRequest_fields, request)) return false;
  request.nonce = UINT64_MAX;
  fullString(request.remote_longname, 'k');
  if (!emit("key-request.boundary", meshtastic_KeyVerificationNumberRequest_fields, request)) return false;

  meshtastic_KeyVerificationFinal finalMessage{};
  if (!emit("key-final.default", meshtastic_KeyVerificationFinal_fields, finalMessage)) return false;
  finalMessage.nonce = UINT64_MAX;
  fullString(finalMessage.remote_longname, 'R');
  finalMessage.isSender = true;
  fullString(finalMessage.verification_characters, '0');
  if (!emit("key-final.boundary", meshtastic_KeyVerificationFinal_fields, finalMessage)) return false;

  meshtastic_Neighbor neighbor{};
  if (!emit("neighbor.default", meshtastic_Neighbor_fields, neighbor)) return false;
  neighbor.node_id = UINT32_MAX;
  neighbor.snr = 127.5f;
  if (!std::isfinite(neighbor.snr)) return false;
  neighbor.last_rx_time = UINT32_MAX;
  neighbor.node_broadcast_interval_secs = UINT32_MAX;
  if (!emit("neighbor.boundary", meshtastic_Neighbor_fields, neighbor)) return false;

  meshtastic_Heartbeat heartbeat{};
  if (!emit("heartbeat.default", meshtastic_Heartbeat_fields, heartbeat)) return false;
  heartbeat.nonce = UINT32_MAX;
  return emit("heartbeat.boundary", meshtastic_Heartbeat_fields, heartbeat);
}
}  // namespace

int main() {
  return allCases() ? 0 : 1;
}
