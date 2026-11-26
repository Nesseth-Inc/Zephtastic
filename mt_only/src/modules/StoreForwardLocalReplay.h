// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "modules/StoreForwardArchive.h"
#include "meshtastic/mesh.pb.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::modules {
// Portable proposal for the stock local PhoneAPI history path. The owner must
// hold its archive/session lock for the entire poll, including emit. This type
// has no Service, radio, settings, allocation, persistence, or Zephyr access.
class StoreForwardLocalReplay {
public:
  static constexpr size_t MaxScanPerPoll = 8;
  static constexpr size_t ChannelCount = 8;
  static constexpr uint32_t Broadcast = UINT32_MAX;

  struct ArchiveView {
    uint32_t epoch = 0;
    size_t count = 0;
  };
  // Within a caller-owned lock, snapshot count/epoch and recordAt pointers
  // are stable for the duration of poll. Records are immutable and append-only
  // within an epoch; overwrite/reset must increment the archive epoch.
  struct Reader {
    void *context = nullptr;
    bool (*snapshot)(void *, ArchiveView &) = nullptr;
    const StoreForwardArchive::Record *(*recordAt)(void *, size_t) = nullptr;
  };
  using EmitFn = bool (*)(void *, const meshtastic_MeshPacket &);

  enum class Result : uint8_t {
    Dormant,
    PriorityPending,
    ReaderUnavailable,
    Drained,
    Scanned,
    Emitted,
    Backpressured
  };

  struct Progress {
    bool active = false;
    uint32_t sessionEpoch = 0;
    uint32_t archiveEpoch = 0;
    size_t nextIndex = 0;
  };

private:
  Progress progress_{};

  static bool validNode(uint32_t node) {
    return node > 1 && node < 0xfffffffeu;
  }

  static bool eligible(const StoreForwardArchive::Record &record,
                       uint32_t localNode) {
    return record.time != 0 && record.from != localNode &&
           (record.to == Broadcast || record.to == localNode);
  }

  static bool validRecord(const StoreForwardArchive::Record &record) {
    return validNode(record.from) &&
           (record.to == Broadcast || validNode(record.to)) &&
           record.channel < ChannelCount && record.hopStart <= 7 &&
           record.hopLimit <= 7 &&
           record.payloadSize <= StoreForwardArchive::MaxPayload &&
           record.payloadSize <= meshtastic_Constants_DATA_PAYLOAD_LEN &&
           record.transportMechanism >=
               _meshtastic_MeshPacket_TransportMechanism_MIN &&
           record.transportMechanism <=
               _meshtastic_MeshPacket_TransportMechanism_MAX;
  }

  static void materialize(const StoreForwardArchive::Record &record,
                          meshtastic_MeshPacket &packet) {
    packet = meshtastic_MeshPacket_init_zero;
    packet.from = record.from;
    packet.to = record.to;
    packet.id = record.id;
    packet.channel = record.channel;
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.has_rx_time = record.hasRxTime;
    packet.rx_time = record.time;
    packet.rx_snr = record.rxSnr;
    packet.hop_limit = record.hopLimit;
    packet.hop_start = record.hopStart;
    packet.want_ack = false;
    packet.has_rx_rssi = record.hasRxRssi;
    packet.rx_rssi = record.rxRssi;
    packet.via_mqtt = record.viaMqtt;
    packet.transport_mechanism =
        static_cast<meshtastic_MeshPacket_TransportMechanism>(
            record.transportMechanism);
    // This is unsigned shared-channel history metadata, never an identity or
    // signature proof. The replay must not claim PKI or XEdDSA authentication.
    packet.pki_encrypted = false;
    packet.xeddsa_signed = false;

    auto &data = packet.decoded;
    data.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    data.payload.size = static_cast<pb_size_t>(record.payloadSize);
    if (record.payloadSize)
      memcpy(data.payload.bytes, record.payload, record.payloadSize);
    data.want_response = false;
    data.reply_id = record.replyId;
    data.emoji = record.emoji ? 1u : 0u;
  }

  void reset() { progress_ = Progress{}; }

public:
  StoreForwardLocalReplay() = default;
  StoreForwardLocalReplay(const StoreForwardLocalReplay &) = delete;
  StoreForwardLocalReplay &operator=(const StoreForwardLocalReplay &) = delete;

  const Progress &progress() const { return progress_; }
  void clear() { reset(); }

  // `higherPriorityPending` mirrors PhoneAPI's queue order. Its caller invokes
  // this only in the packet-delivery slot; a true value leaves progress intact.
  // `enabled`, `connected` and `configComplete` jointly gate all archive reads.
  // `sessionEpoch` must change for a new Companion connection/config session.
  // EmitFn true means the caller accepted the packet into its local API queue;
  // false means it did not consume it, so the same record is retried verbatim.
  Result poll(bool enabled, bool connected, bool configComplete,
              bool higherPriorityPending, uint32_t sessionEpoch,
              uint32_t localNode, const Reader &reader, EmitFn emit,
              void *emitContext, meshtastic_MeshPacket &packetBuffer) {
    if (!enabled || !connected || !configComplete) {
      reset();
      return Result::Dormant;
    }
    if (higherPriorityPending) return Result::PriorityPending;
    if (!validNode(localNode) || !reader.snapshot || !reader.recordAt || !emit)
      return Result::ReaderUnavailable;

    ArchiveView view{};
    if (!reader.snapshot(reader.context, view)) return Result::ReaderUnavailable;

    if (!progress_.active || progress_.sessionEpoch != sessionEpoch ||
        progress_.archiveEpoch != view.epoch) {
      progress_.active = true;
      progress_.sessionEpoch = sessionEpoch;
      progress_.archiveEpoch = view.epoch;
      progress_.nextIndex = 0;
    }
    // A count regression without an epoch change violates the reader contract.
    // Fail closed; don't index stale/wrapped storage or expose a partial packet.
    if (progress_.nextIndex > view.count) {
      reset();
      return Result::ReaderUnavailable;
    }

    size_t scanned = 0;
    while (progress_.nextIndex < view.count && scanned < MaxScanPerPoll) {
      const size_t index = progress_.nextIndex;
      const auto *record = reader.recordAt(reader.context, index);
      if (!record) return Result::ReaderUnavailable;
      ++scanned;

      // Malformed stored data is skipped without constructing or emitting a
      // partial packet. Valid but ineligible records are skipped as stock does.
      if (!validRecord(*record)) {
        ++progress_.nextIndex;
        continue;
      }
      if (!eligible(*record, localNode)) {
        ++progress_.nextIndex;
        continue;
      }

      // Build the complete phone packet only after validating the entire source
      // record. A failed EmitFn leaves nextIndex unchanged for identical retry.
      materialize(*record, packetBuffer);
      if (!emit(emitContext, packetBuffer)) return Result::Backpressured;
      ++progress_.nextIndex;
      return Result::Emitted;
    }
    return progress_.nextIndex == view.count ? Result::Drained : Result::Scanned;
  }
};

static_assert(StoreForwardLocalReplay::MaxScanPerPoll == 8,
              "local archive scan must be bounded");
}
