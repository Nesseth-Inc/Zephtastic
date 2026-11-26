// SPDX-License-Identifier: GPL-3.0-only
#include "network/MqttMapReporter.h"
#include "MqttServiceEnvelopeDecoder.h"
#include "meshtastic/mqtt.pb.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

namespace {
using namespace mt::mqtt;

struct Capture {
  bool called = false;
  PublishResult result = PublishResult::Accepted;
  char topic[160]{};
  meshtastic_MeshPacket packet{};
  char channel[129]{};
  char gateway[129]{};
};

PublishResult capture(void *context, const char *topic,
                      const meshtastic_MeshPacket *packet,
                      const char *channel, size_t channelLength,
                      const char *gateway, size_t gatewayLength) {
  auto &c = *static_cast<Capture *>(context);
  c.called = true;
  assert(strlen(topic) < sizeof(c.topic));
  strcpy(c.topic, topic);
  c.packet = *packet;
  assert(channelLength < sizeof(c.channel) && gatewayLength < sizeof(c.gateway));
  memcpy(c.channel, channel, channelLength); c.channel[channelLength] = 0;
  memcpy(c.gateway, gateway, gatewayLength); c.gateway[gatewayLength] = 0;
  return c.result;
}

mt::network::MqttGatewaySettings settings(bool reportEnabled = true,
                                         bool locationOptedIn = true) {
  meshtastic_ModuleConfig_MQTTConfig config = meshtastic_ModuleConfig_MQTTConfig_init_zero;
  strcpy(config.address, "mqtt.example.invalid");
  strcpy(config.root, "lab/map");
  config.enabled = true;
  config.map_reporting_enabled = reportEnabled;
  config.has_map_report_settings = true;
  config.map_report_settings.should_report_location = locationOptedIn;
  config.map_report_settings.publish_interval_secs = 3600;
  config.map_report_settings.position_precision = 14;
  mt::network::MqttGatewaySettings result;
  assert(result.assign(config) && result.valid());
  assert(result.admissionStatus(true, false) == 0);
  return result;
}

MqttMapReport::Source source() {
  MqttMapReport::Source value{};
  value.longName = "Synthetic Map Node With Long Name";
  value.shortName = "SYN1";
  value.firmwareVersion = "2.8.1-test-synthetic";
  value.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
  value.hwModel = meshtastic_HardwareModel_TBEAM;
  value.region = meshtastic_Config_LoRaConfig_RegionCode_US;
  value.modemPreset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
  value.hasDefaultChannel = true;
  value.latitudeI = 377749000;
  value.longitudeI = -1224194000;
  value.altitude = 18;
  value.numOnlineLocalNodes = 7;
  return value;
}

MqttMapReporter::Identity identity() {
  return {0x12345678u, 0, "LongFast", 8, "!12345678", 9};
}

void reporterTests() {
  auto cfg = settings();
  MqttMapReporter reporter;
  Capture out{};
  const auto src = source();
  const auto id = identity();

  assert(reporter.poll(cfg, src, id, "lab/map", 1000, false, mt::mqtt::State::WaitingForWifi, false,
                       capture, &out) == MqttMapReporter::Result::TransportUnavailable);
  const mt::mqtt::State disconnected[]={mt::mqtt::State::Connecting,mt::mqtt::State::Backoff,mt::mqtt::State::Refused};
  for(auto state:disconnected)
    assert(reporter.poll(cfg,src,id,"lab/map",1001,false,state,false,capture,&out)==MqttMapReporter::Result::TransportUnavailable);
  assert(!out.called && !reporter.lastReportMillis());
  MqttMapReporter proxy;
  Capture proxyOut{};
  assert(proxy.poll(cfg,src,id,"lab/map",1002,true,mt::mqtt::State::Stopped,false,capture,&proxyOut)==MqttMapReporter::Result::Published);
  assert(proxyOut.called);
  assert(reporter.poll(cfg, src, id, "lab/map", 1000, false, mt::mqtt::State::Online, false,
                       capture, &out) == MqttMapReporter::Result::Published);
  assert(out.called && !strcmp(out.topic, "lab/map/2/map/"));
  assert(out.packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag);
  assert(out.packet.from == id.fromNode && out.packet.to == UINT32_MAX &&
         out.packet.id == id.packetId);
  assert(out.packet.decoded.portnum == meshtastic_PortNum_MAP_REPORT_APP);
  assert(!strcmp(out.channel, "LongFast") && !strcmp(out.gateway, "!12345678"));
  assert(reporter.lastReportMillis() == 1000);
  static const uint8_t stockGolden[] = {
      0x0a,0x18,0x53,0x79,0x6e,0x74,0x68,0x65,0x74,0x69,0x63,0x20,0x4d,0x61,0x70,0x20,
      0x4e,0x6f,0x64,0x65,0x20,0x57,0x69,0x74,0x68,0x20,0x12,0x04,0x53,0x59,0x4e,0x31,
      0x20,0x04,0x2a,0x11,0x32,0x2e,0x38,0x2e,0x31,0x2d,0x74,0x65,0x73,0x74,0x2d,0x73,
      0x79,0x6e,0x74,0x68,0x65,0x30,0x01,0x40,0x01,0x4d,0x00,0x00,0x82,0x16,0x55,0x00,
      0x00,0x0a,0xb7,0x58,0x12,0x60,0x0e,0x68,0x07,0x70,0x01};
  assert(out.packet.decoded.payload.size == sizeof(stockGolden));
  assert(!memcmp(out.packet.decoded.payload.bytes, stockGolden, sizeof(stockGolden)));

  // The runtime sink uses this exact bounded encoder to create the stock
  // ServiceEnvelope. Decode it again and verify channel/gateway/packet identity.
  uint8_t envelopeBytes[ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes]{};
  size_t envelopeLength = 0;
  assert(ServiceEnvelopeCodec::encode(&out.packet, out.channel, strlen(out.channel),
                                     out.gateway, strlen(out.gateway),
                                     envelopeBytes, sizeof(envelopeBytes),
                                     &envelopeLength));
  MqttServiceEnvelopeDecoder::Decoded decoded{};
  assert(MqttServiceEnvelopeDecoder::decode(envelopeBytes, envelopeLength, &decoded));
  assert(decoded.kind == MqttServiceEnvelopeDecoder::PacketKind::kDecoded);
  assert(decoded.channelIdLength == strlen(out.channel) &&
         !strcmp(decoded.channelId, out.channel));
  assert(decoded.gatewayIdLength == strlen(out.gateway) &&
         !strcmp(decoded.gatewayId, out.gateway));
  assert(decoded.packet.from == id.fromNode && decoded.packet.to == UINT32_MAX &&
         decoded.packet.id == id.packetId &&
         decoded.packet.decoded.portnum == meshtastic_PortNum_MAP_REPORT_APP);
  meshtastic_MapReport report = meshtastic_MapReport_init_zero;
  auto body = pb_istream_from_buffer(decoded.packet.decoded.payload.bytes,
                                     decoded.packet.decoded.payload.size);
  assert(pb_decode(&body, meshtastic_MapReport_fields, &report) && body.bytes_left == 0);
  assert(report.has_opted_report_location && report.latitude_i == 377618432 &&
         report.longitude_i == -1224081408 && report.position_precision == 14);

  // Stock throttles from the publish attempt, even when the bounded queue
  // refuses it; this prevents a tight repeated-attempt loop.
  out.called = false;
  assert(reporter.poll(cfg, src, id, "lab/map", 3600999, false, mt::mqtt::State::Online, false,
                       capture, &out) == MqttMapReporter::Result::IntervalNotElapsed);
  out.result = PublishResult::QueueFull;
  assert(reporter.poll(cfg, src, id, "lab/map", 3601000, false, mt::mqtt::State::Online, false,
                       capture, &out) == MqttMapReporter::Result::QueueRejected);
  assert(reporter.lastReportMillis() == 3601000);
  assert(reporter.poll(cfg, src, id, "lab/map", 7200999, false, mt::mqtt::State::Online, false,
                       capture, &out) == MqttMapReporter::Result::IntervalNotElapsed);
  out.result = PublishResult::Accepted;
  assert(reporter.poll(cfg, src, id, "lab/map", 7201000, false, mt::mqtt::State::Online, false,
                       capture, &out) == MqttMapReporter::Result::Published);
  assert(reporter.lastReportMillis() == 7201000);
}

void setPrimaryKey(mt::Config &config, const uint8_t *key, size_t length) {
  auto node = config.node();
  mt::PrimaryChannel::Selection selection{};
  assert(mt::PrimaryChannel::normalize(key, length, selection,
                                       config.effectivePrimaryName()));
  node.setKey(selection.hash, selection.bytes, selection.length);
  mt::PrimaryChannel::clear(selection);
  config.node(node);
  assert(config.valid() && config.channelConfigured());
}

void stockChannelMetadataTests() {
  mt::Config publicPrimary;
  const auto unsetBefore = publicPrimary;
  assert(publicPrimary.regionCode() == 0 && !publicPrimary.provisioned() &&
         publicPrimary.channelConfigured());
  assert(mt::mqtt::MqttMapReporter::hasDefaultChannel(publicPrimary));
  assert(publicPrimary.same(unsetBefore) && publicPrimary.regionCode() == 0 &&
         !publicPrimary.provisioned());
  // Metadata recognition does not make UNSET a radio-admitted region.
  mt::LoRaPresets::Profile unprovisionedProfile{};
  mt::USChannelSelection unprovisionedSlot{};
  const auto unsetRequest = publicPrimary.radioRequest();
  assert(publicPrimary.resolveRadio(unprovisionedProfile, unprovisionedSlot) &&
         unprovisionedSlot.regionCode == 0);
  assert(!mt::RadioSettings::resolve(unsetRequest, unprovisionedProfile,
                                     unprovisionedSlot,
                                     publicPrimary.effectivePrimaryName(),
                                     mt::Config::radioPresetName(publicPrimary.modemPreset())));

  mt::Config usPublic;
  auto usRequest = usPublic.radioRequest();
  usRequest.regionCode = 1;
  usRequest.channel = 0;
  assert(usPublic.setRadio(usRequest));
  assert(usPublic.provisioned() && usPublic.regionCode() == 1 &&
         mt::mqtt::MqttMapReporter::hasDefaultChannel(usPublic));

  mt::Config eu433Public;
  auto regionalRequest = eu433Public.radioRequest();
  regionalRequest.regionCode = 2; // EU_433, standard profile supports LongFast
  regionalRequest.channel = 0;
  assert(eu433Public.setRadio(regionalRequest));
  assert(eu433Public.provisioned() && eu433Public.regionCode() == 2 &&
         mt::mqtt::MqttMapReporter::hasDefaultChannel(eu433Public));

  // Region-aware metadata resolution covers the stock ANZ 915 MHz region
  // (RegionCode 6); this is logical metadata only, not hardware or regulatory
  // suitability evidence.
  mt::Config au915Public;
  regionalRequest = au915Public.radioRequest();
  regionalRequest.regionCode = 6; // ANZ, 915–928 MHz
  regionalRequest.channel = 0;    // stock-selected default slot
  assert(au915Public.setRadio(regionalRequest));
  assert(au915Public.provisioned() && au915Public.regionCode() == 6 &&
         mt::mqtt::MqttMapReporter::hasDefaultChannel(au915Public));

  // An active inherited secondary with the preset-derived name and Public key
  // is a stock default channel.
  mt::ChannelRecord inherited;
  assert(inherited.set(2, "", nullptr, 0));
  assert(publicPrimary.setSecondaryChannel(1, inherited));
  assert(mt::mqtt::MqttMapReporter::hasDefaultChannel(publicPrimary));

  // No preset, explicit non-default frequency slot, or verbatim frequency
  // override each suppresses the stock predicate.
  mt::Config customModem;
  auto request = customModem.radioRequest();
  request.usePreset = false;
  request.channel = 0;
  assert(customModem.setRadio(request));
  assert(!mt::mqtt::MqttMapReporter::hasDefaultChannel(customModem));

  request = publicPrimary.radioRequest();
  mt::LoRaPresets::Profile profile{};
  mt::USChannelSelection defaultSlot{};
  request.channel = 0;
  assert(mt::RadioSettings::validateUnprovisioned(
      request, profile, defaultSlot, publicPrimary.effectivePrimaryName()));
  request.channel = defaultSlot.slot == 0 ? 2 : 1;
  assert(request.channel - 1 != defaultSlot.slot);
  mt::Config wrongSlot;
  assert(wrongSlot.setRadio(request));
  assert(!mt::mqtt::MqttMapReporter::hasDefaultChannel(wrongSlot));

  request = publicPrimary.radioRequest();
  request.overrideMHz = 915.0f;
  assert(wrongSlot.setRadio(request));
  assert(!mt::mqtt::MqttMapReporter::hasDefaultChannel(wrongSlot));

  // The exact preset display name is required even with the equivalent
  // expanded Public key.
  mt::Config customName;
  assert(customName.setPrimaryName("Other"));
  mt::PrimaryChannel::Selection customNameKey{};
  assert(customName.channelSelection(0, customNameKey) && customNameKey.publicDefault);
  mt::PrimaryChannel::clear(customNameKey);
  assert(!mt::mqtt::MqttMapReporter::hasDefaultChannel(customName));

  // A configured custom-key primary is not the default Public channel.
  mt::Config privatePrimary;
  uint8_t privateKey[16]{};
  for (size_t i = 0; i < sizeof(privateKey); ++i) privateKey[i] = uint8_t(i + 1);
  setPrimaryKey(privatePrimary, privateKey, sizeof(privateKey));
  assert(!mt::mqtt::MqttMapReporter::hasDefaultChannel(privatePrimary));

  // A disabled slot cannot make a private configuration look like Public.
  mt::ChannelRecord disabled;
  assert(disabled.set(0, "", nullptr, 0));
  assert(privatePrimary.setSecondaryChannel(1, disabled));
  assert(!mt::mqtt::MqttMapReporter::hasDefaultChannel(privatePrimary));
}

void eventChannelTests() {
  mt::Config namedEvent;
  assert(namedEvent.setPrimaryName("Event"));
#if defined(USERPREFS_BLOCK_POSITION_ON_EVENT_CHANNEL) && \
    USERPREFS_BLOCK_POSITION_ON_EVENT_CHANNEL && defined(USERPREFS_CHANNEL_0_PSK)
  static const uint8_t eventKey[] = USERPREFS_CHANNEL_0_PSK;
  mt::Config keyedEvent;
  assert(keyedEvent.setPrimaryName("NotEvent"));
  setPrimaryKey(keyedEvent, eventKey, sizeof(eventKey));
  assert(mt::mqtt::MqttMapReporter::isEventChannel(keyedEvent, 0));
  uint8_t otherKey[16]{};
  for (size_t i = 0; i < sizeof(otherKey); ++i) otherKey[i] = uint8_t(0xa0 + i);
  mt::Config nonEvent;
  assert(nonEvent.setPrimaryName("Event"));
  setPrimaryKey(nonEvent, otherKey, sizeof(otherKey));
  assert(!mt::mqtt::MqttMapReporter::isEventChannel(nonEvent, 0));
#else
  // The name is irrelevant; without the compiled policy/key, stock returns false.
  assert(!mt::mqtt::MqttMapReporter::isEventChannel(namedEvent, 0));
#endif
}

void gateAndFailureTests() {
  auto cfg = settings();
  MqttMapReporter reporter;
  auto src = source(); auto id = identity(); Capture out{};
  auto noOptIn = settings(true, false);
  assert(reporter.poll(noOptIn, src, id, "lab/map", 1000, false, mt::mqtt::State::Online, false,
                       capture, &out) == MqttMapReporter::Result::LocationNotOptedIn);
  auto disabled = settings(false, true);
  assert(reporter.poll(disabled, src, id, "lab/map", 1000, false, mt::mqtt::State::Online, false,
                       capture, &out) == MqttMapReporter::Result::Disabled);
  assert(!out.called);
  cfg.bytes[mt::network::MqttGatewaySettings::ReportLocation] = 0;
  // Mutating canonical storage invalidates the CRC; malformed snapshots fail
  // closed before any call to the publication sink.
  assert(reporter.poll(cfg, src, id, "lab/map", 1000, false, mt::mqtt::State::Online, false,
                       capture, &out) == MqttMapReporter::Result::InvalidInput);
  cfg = settings();
  assert(reporter.poll(cfg, src, id, "lab/map", 1000, false, mt::mqtt::State::Online, true,
                       capture, &out) == MqttMapReporter::Result::EventChannelSuppressed);
  src.latitudeI = 0; src.longitudeI = 0;
  assert(reporter.poll(cfg, src, id, "lab/map", 1000, false, mt::mqtt::State::Online, false,
                       capture, &out) == MqttMapReporter::Result::PositionUnavailable);
  src = source();
  // Stock map MeshPacket has no assigned mesh packet ID (zero).
  assert(id.packetId == 0);
  assert(reporter.poll(cfg, src, id, "bad/#", 1000, false, mt::mqtt::State::Online, false,
                       capture, &out) == MqttMapReporter::Result::InvalidInput);
  assert(!out.called);
}
}  // namespace

int main() {
  reporterTests();
  stockChannelMetadataTests();
  eventChannelTests();
  gateAndFailureTests();
  puts("PASS MqttMapReporter stock packet/envelope, identity, privacy and interval tests");
}
