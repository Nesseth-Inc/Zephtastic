// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "MqttGatewaySettings.h"
#include "MqttMapReport.h"
#include "MqttPublisherTransport.h"
#include "../MqttServiceEnvelopeCodec.h"
#include "meshtastic/mesh.pb.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::mqtt {

// Main-loop adapter for stock MapReport. Caller snapshots all identity and
// radio/config metadata; this object retains only the stock report timestamp.
// The sink is expected to enqueue synchronously (e.g. network::enqueuePacket),
// which copies its bounded inputs before returning. No socket or RF work occurs.
class MqttMapReporter {
 public:
  using Sink = PublishResult (*)(void *context, const char *topic,
                                 const meshtastic_MeshPacket *packet,
                                 const char *channelId, size_t channelIdLength,
                                 const char *gatewayId, size_t gatewayIdLength);

  struct Identity {
    uint32_t fromNode = 0;
    uint32_t packetId = 0;
    const char *channelId = nullptr;
    size_t channelIdLength = 0;
    const char *gatewayId = nullptr;
    size_t gatewayIdLength = 0;
  };

  enum class Result : uint8_t {
    Published,
    Disabled,
    LocationNotOptedIn,
    TransportUnavailable,
    EventChannelSuppressed,
    PositionUnavailable,
    IntervalNotElapsed,
    InvalidInput,
    EncodeFailure,
    QueueRejected
  };

  // Pin stock's Channels::hasDefaultChannel/isDefaultChannel semantics for the
  // supported regional/UNSET model: preset enabled, no frequency override,
  // active channel named for the selected modem preset, equivalent Public
  // key, and the actual slot matching the preset's default channel-name hash.
  // Resolve both slots through ZephCore's radio resolver; never bake in a slot
  // number. Config::channelSelection rejects disabled slots; always wipe keys.
  static bool hasDefaultChannel(const Config &config) {
    const auto request = config.radioRequest();
    if (!request.usePreset || request.overrideMHz != 0.0f) return false;
    const char *presetName = Config::radioPresetName(request.preset);
    if (!presetName) return false;
    LoRaPresets::Profile actualProfile{}, defaultProfile{};
    USChannelSelection actualSlot{}, defaultSlot{};
    auto defaultRequest = request;
    defaultRequest.channel = 0; // stock auto/hash slot for effective primary name
    // This is metadata, not radio admission. An UNSET record may describe the
    // stock default channel while remaining explicitly unprovisioned. Keep
    // that structural result local; it never configures a radio or permits TX.
    const auto metadataSlot = [&](const RadioSettings::Request &value,
                                  LoRaPresets::Profile &profile,
                                  USChannelSelection &slot) {
      return value.regionCode == 0
          ? RadioSettings::validateUnprovisioned(value, profile, slot,
                                                config.effectivePrimaryName())
          : RadioSettings::resolve(value, profile, slot,
                                   config.effectivePrimaryName(), presetName);
    };
    if (!metadataSlot(request, actualProfile, actualSlot) ||
        !metadataSlot(defaultRequest, defaultProfile, defaultSlot) ||
        actualSlot.slot != defaultSlot.slot)
      return false;

    bool found = false;
    for (unsigned index = 0; index < 8; ++index) {
      ChannelRecord channel;
      const bool channelExists = config.channelRecord(index, channel);
      PrimaryChannel::Selection selection{};
      const bool selected = channelExists && config.channelSelection(index, selection);
      const char *channelName = channelExists && channel.name()[0]
          ? channel.name() : config.modemName();
      if (selected && selection.publicDefault &&
          !strcmp(channelName, presetName))
        found = true;
      PrimaryChannel::clear(selection);
    }
    return found;
  }

  // Match stock Channels::isEventChannel: only builds with the explicit
  // event-position policy and configured channel-0 key have an event key.
  // Channel names alone never identify event channels.
  static bool isEventChannel(const Config &config, unsigned index) {
    return config.isPositionEventChannel(index);
  }

  // One cooperative scheduling step. The settings object, source strings,
  // identity strings and sink context need only remain valid for this call.
  // The running worker can be offline. Only the connected broker or an
  // authorized phone proxy can start the stock reporting interval.
  Result poll(const network::MqttGatewaySettings &settings,
              const MqttMapReport::Source &source, const Identity &identity,
              const char *configuredRoot, uint32_t nowMillis,
              bool proxyReady, State directState, bool suppressOnEventChannel,
              Sink sink, void *context) {
    if (!settings.valid()) return Result::InvalidInput;
    MqttMapReport::Gate gate{};
    gate.reportingEnabled = settings.mapReportingEnabled();
    gate.locationOptedIn = settings.mapSettingsConfigured() &&
                           settings.mapLocationOptedIn();
    gate.transportReady = proxyReady || directState == State::Online;
    gate.suppressOnEventChannel = suppressOnEventChannel;
    gate.configuredIntervalSeconds = settings.mapPublishIntervalSeconds();
    gate.lastReportMillis = lastReportMillis_;
    gate.nowMillis = nowMillis;

    const auto allowed = MqttMapReport::admission(
        gate, source.latitudeI, source.longitudeI);
    if (allowed != MqttMapReport::Decision::Ready) return mapResult(allowed);
    if (!sink || !validIdentity(identity)) return Result::InvalidInput;

    char topic[MqttMapReport::kTopicCapacity]{};
    size_t topicLength = 0;
    if (!MqttMapReport::topic(configuredRoot, topic, sizeof(topic),
                              &topicLength))
      return Result::InvalidInput;

    // This is the only map payload scratch; the packet owns the encoded bytes
    // passed to the enqueue sink, which performs bounded ServiceEnvelope encode.
    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.from = identity.fromNode;
    packet.to = UINT32_MAX;
    packet.id = identity.packetId;
    packet.decoded.portnum = meshtastic_PortNum_MAP_REPORT_APP;
    size_t payloadLength = 0;
    const auto encoded = MqttMapReport::encode(
        gate, settings.mapPositionPrecision(), source,
        packet.decoded.payload.bytes, sizeof(packet.decoded.payload.bytes),
        &payloadLength);
    if (encoded != MqttMapReport::Decision::Ready || payloadLength == 0 ||
        payloadLength > sizeof(packet.decoded.payload.bytes))
      return encoded == MqttMapReport::Decision::EncodeFailure ||
                     encoded == MqttMapReport::Decision::OutputTooSmall
                 ? Result::EncodeFailure
                 : mapResult(encoded);
    packet.decoded.payload.size = static_cast<pb_size_t>(payloadLength);

    // Stock advances last_report_to_map after calling publish(), regardless of
    // its result. Match that attempt interval so a full bounded queue cannot
    // cause a tight retry loop on each Service iteration.
    const PublishResult queued = sink(context, topic, &packet,
                                      identity.channelId,
                                      identity.channelIdLength,
                                      identity.gatewayId,
                                      identity.gatewayIdLength);
    lastReportMillis_ = nowMillis == 0 ? 1 : nowMillis; // stock skipZero()
    if (queued != PublishResult::Accepted) return Result::QueueRejected;
    return Result::Published;
  }

  uint32_t lastReportMillis() const { return lastReportMillis_; }
  void reset() { lastReportMillis_ = 0; }

 private:
  uint32_t lastReportMillis_ = 0;

  static bool validIdentity(const Identity &identity) {
    return identity.fromNode > 1 && identity.fromNode < 0xfffffffeu &&
           validString(identity.channelId, identity.channelIdLength) &&
           validString(identity.gatewayId, identity.gatewayIdLength);
  }
  static bool validString(const char *value, size_t length) {
    if (!value || length > ServiceEnvelopeCodec::kMaxStringBytes) return false;
    for (size_t i = 0; i < length; ++i) if (!value[i]) return false;
    return value[length] == '\0';
  }
  static Result mapResult(MqttMapReport::Decision decision) {
    switch (decision) {
      case MqttMapReport::Decision::Disabled: return Result::Disabled;
      case MqttMapReport::Decision::LocationNotOptedIn:
        return Result::LocationNotOptedIn;
      case MqttMapReport::Decision::TransportUnavailable:
        return Result::TransportUnavailable;
      case MqttMapReport::Decision::EventChannelSuppressed:
        return Result::EventChannelSuppressed;
      case MqttMapReport::Decision::PositionUnavailable:
        return Result::PositionUnavailable;
      case MqttMapReport::Decision::IntervalNotElapsed:
        return Result::IntervalNotElapsed;
      case MqttMapReport::Decision::OutputTooSmall:
      case MqttMapReport::Decision::EncodeFailure: return Result::EncodeFailure;
      case MqttMapReport::Decision::InvalidInput: return Result::InvalidInput;
      case MqttMapReport::Decision::Ready: return Result::Published;
    }
    return Result::InvalidInput;
  }
};

}  // namespace mt::mqtt
