// SPDX-License-Identifier: GPL-3.0-only
#include "modules/StoreForwardLocalReplay.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {
using Archive = mt::modules::StoreForwardArchive;
using Replay = mt::modules::StoreForwardLocalReplay;
constexpr uint32_t Local = 0x10203040u;
constexpr uint32_t Peer = 0x50607080u;
constexpr uint32_t Other = 0x61728394u;

struct Store {
  Archive::Record records[16]{};
  Archive::Cursor cursors[4]{};
  Archive archive{records, 16, cursors, 4};
  unsigned snapshots = 0;
  unsigned reads = 0;
  static bool snapshot(void *context, Replay::ArchiveView &view) {
    auto &self = *static_cast<Store *>(context);
    ++self.snapshots;
    view.epoch = self.archive.epoch();
    view.count = self.archive.size();
    return self.archive.ready();
  }
  static const Archive::Record *recordAt(void *context, size_t index) {
    auto &self = *static_cast<Store *>(context);
    ++self.reads;
    return self.archive.recordAt(index);
  }
  Replay::Reader reader() { return {this, snapshot, recordAt}; }
  void add(uint32_t from, uint32_t to, uint32_t id, uint32_t time,
           const char *body, uint8_t channel = 1) {
    Archive::MessageView v{};
    v.from = from; v.to = to; v.id = id; v.time = time;
    v.hasRxTime = false; v.channel = channel; v.hopStart = 2; v.hopLimit = 5;
    v.transportMechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_MQTT;
    v.hasRxRssi = true; v.rxRssi = -99; v.rxSnr = 2.25f; v.viaMqtt = true;
    v.replyId = 0xabcdef01; v.emoji = true;
    v.payload = reinterpret_cast<const uint8_t *>(body); v.payloadSize = strlen(body);
    assert(archive.add(v) == Archive::AddResult::Added);
  }
};

struct Sink {
  bool accept = true;
  unsigned calls = 0;
  meshtastic_MeshPacket last = meshtastic_MeshPacket_init_zero;
  static bool emit(void *context, const meshtastic_MeshPacket &packet) {
    auto &self = *static_cast<Sink *>(context);
    ++self.calls;
    self.last = packet;
    return self.accept;
  }
};

void testFilterMetadataAndBackpressure() {
  Store store;
  store.add(Peer, UINT32_MAX, 1, 0, "unknown time");
  store.add(Local, UINT32_MAX, 2, 500, "self-authored");
  store.add(Peer, Other, 3, 600, "not addressed here");
  store.add(Peer, UINT32_MAX, 4, 700, "eligible broadcast");
  store.add(Peer, Local, 5, 800, "eligible direct");
  store.add(Peer, Local, 0, 900, "zero id unicast");

  Replay replay;
  Sink sink; sink.accept = false;
  meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
  auto reader = store.reader();
  assert(replay.poll(true, true, true, false, 9, Local, reader,
                     Sink::emit, &sink, packet) == Replay::Result::Backpressured);
  assert(sink.calls == 1 && replay.progress().nextIndex == 3);
  assert(packet.from == Peer && packet.to == UINT32_MAX && packet.id == 4);
  assert(packet.channel == 1 && packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag);
  assert(packet.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP);
  assert(packet.decoded.payload.size == strlen("eligible broadcast"));
  assert(memcmp(packet.decoded.payload.bytes, "eligible broadcast", packet.decoded.payload.size) == 0);
  assert(packet.decoded.reply_id == 0xabcdef01 && packet.decoded.emoji == 1);
  assert(!packet.decoded.want_response && !packet.want_ack);
  assert(!packet.has_rx_time && packet.rx_time == 700);
  assert(packet.has_rx_rssi && packet.rx_rssi == -99 && packet.rx_snr == 2.25f);
  assert(packet.hop_start == 2 && packet.hop_limit == 5 && packet.via_mqtt);
  assert(packet.transport_mechanism == meshtastic_MeshPacket_TransportMechanism_TRANSPORT_MQTT);
  assert(!packet.pki_encrypted && !packet.xeddsa_signed && packet.public_key.size == 0);

  const auto firstAttempt = packet;
  assert(replay.poll(true, true, true, true, 9, Local, reader,
                     Sink::emit, &sink, packet) == Replay::Result::PriorityPending);
  assert(sink.calls == 1 && replay.progress().nextIndex == 3);
  sink.accept = true;
  assert(replay.poll(true, true, true, false, 9, Local, reader,
                     Sink::emit, &sink, packet) == Replay::Result::Emitted);
  assert(sink.calls == 2 && replay.progress().nextIndex == 4);
  assert(memcmp(&packet, &firstAttempt, sizeof(packet)) == 0);

  assert(replay.poll(true, true, true, false, 9, Local, reader,
                     Sink::emit, &sink, packet) == Replay::Result::Emitted);
  assert(sink.calls == 3 && replay.progress().nextIndex == 5);
  assert(packet.from == Peer && packet.to == Local && packet.id == 5);
  assert(replay.poll(true, true, true, false, 9, Local, reader,
                     Sink::emit, &sink, packet) == Replay::Result::Emitted);
  assert(packet.to == Local && packet.id == 0); // stock IDs may be zero on unicast text
  assert(replay.poll(true, true, true, false, 9, Local, reader,
                     Sink::emit, &sink, packet) == Replay::Result::Drained);
  assert(sink.calls == 4);
  puts("PASS stock privacy filter, exact metadata, one-per-poll, identical retry on backpressure");
}

void testScanBoundAndMalformedRecords() {
  Store store;
  for (uint32_t i = 0; i < 12; ++i)
    store.add(Peer, Other, 100 + i, 1000 + i, "not for local");
  Replay replay;
  Sink sink;
  meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
  auto reader = store.reader();
  assert(replay.poll(true, true, true, false, 1, Local, reader,
                     Sink::emit, &sink, packet) == Replay::Result::Scanned);
  assert(replay.progress().nextIndex == Replay::MaxScanPerPoll);
  assert(store.reads == Replay::MaxScanPerPoll && sink.calls == 0);
  assert(replay.poll(true, true, true, false, 1, Local, reader,
                     Sink::emit, &sink, packet) == Replay::Result::Drained);
  assert(replay.progress().nextIndex == 12 && store.reads == 12);

  Store malformed;
  malformed.add(Peer, UINT32_MAX, 20, 1000, "bad channel");
  malformed.records[0].channel = 8;
  malformed.add(Peer, UINT32_MAX, 21, 1001, "bad transport");
  malformed.records[1].transportMechanism = 0xff;
  malformed.add(Peer, UINT32_MAX, 22, 1002, "bad size");
  malformed.records[2].payloadSize = Archive::MaxPayload + 1;
  malformed.add(UINT32_MAX, UINT32_MAX, 23, 1003, "bad source");
  Replay filtered;
  Sink noEmit;
  auto malformedReader = malformed.reader();
  packet = meshtastic_MeshPacket_init_zero; packet.id = 0x55aa;
  const auto untouched = packet;
  assert(filtered.poll(true, true, true, false, 1, Local, malformedReader,
                       Sink::emit, &noEmit, packet) == Replay::Result::Drained);
  assert(filtered.progress().nextIndex == 4 && noEmit.calls == 0);
  assert(memcmp(&packet, &untouched, sizeof(packet)) == 0);
  puts("PASS scan budget is eight and invalid channel/enum/size/source never emits");
}

void testLifecycleAndEpoch() {
  Store store;
  store.add(Peer, UINT32_MAX, 31, 1200, "first");
  Replay replay;
  Sink sink;
  meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
  auto reader = store.reader();
  assert(replay.poll(true, true, true, false, 10, Local, reader,
                     Sink::emit, &sink, packet) == Replay::Result::Emitted);
  assert(replay.progress().nextIndex == 1);

  const unsigned snapshots = store.snapshots;
  assert(replay.poll(false, true, true, false, 10, Local, reader,
                     Sink::emit, &sink, packet) == Replay::Result::Dormant);
  assert(!replay.progress().active && store.snapshots == snapshots);
  assert(replay.poll(true, true, false, false, 10, Local, reader,
                     Sink::emit, &sink, packet) == Replay::Result::Dormant);
  assert(replay.poll(true, false, true, false, 10, Local, reader,
                     Sink::emit, &sink, packet) == Replay::Result::Dormant);

  // A new session starts from the beginning after disconnect/config gating.
  assert(replay.poll(true, true, true, false, 10, Local, reader,
                     Sink::emit, &sink, packet) == Replay::Result::Emitted);
  assert(packet.id == 31);
  assert(replay.poll(true, true, true, false, 11, Local, reader,
                     Sink::emit, &sink, packet) == Replay::Result::Emitted);
  assert(packet.id == 31 && replay.progress().sessionEpoch == 11);

  // Archive overwrite advances its epoch; the old cursor must not hide the
  // replacement record at index zero.
  Archive::Record wrapRecords[2]{};
  Archive::Cursor wrapCursors[2]{};
  Archive archive(wrapRecords, 2, wrapCursors, 2);
  Archive::MessageView v{};
  v.from = Peer; v.to = UINT32_MAX; v.id = 40; v.time = 2000;
  v.payload = reinterpret_cast<const uint8_t *>("old0"); v.payloadSize = 4;
  assert(archive.add(v) == Archive::AddResult::Added);
  v.id = 41; v.time = 2001;
  assert(archive.add(v) == Archive::AddResult::Added);
  Replay epochReplay;
  // Use direct archive callbacks for a two-record wrapping archive.
  struct WrapReader {
    Archive *archive;
    Archive::Record *records;
    static bool snapshot(void *ctx, Replay::ArchiveView &out) {
      auto &self = *static_cast<WrapReader *>(ctx);
      out = {self.archive->epoch(), self.archive->size()}; return true;
    }
    static const Archive::Record *at(void *ctx, size_t i) {
      auto &self = *static_cast<WrapReader *>(ctx);
      return i < self.archive->size() ? &self.records[i] : nullptr;
    }
  } wr{&archive, wrapRecords};
  Replay::Reader wrapReader{&wr, WrapReader::snapshot, WrapReader::at};
  assert(epochReplay.poll(true, true, true, false, 1, Local, wrapReader,
                          Sink::emit, &sink, packet) == Replay::Result::Emitted);
  assert(packet.id == 40 && epochReplay.progress().nextIndex == 1);
  Archive::MessageView replacement{};
  replacement.from = Peer; replacement.to = UINT32_MAX; replacement.id = 42;
  replacement.time = 2002; replacement.payload = reinterpret_cast<const uint8_t *>("new");
  replacement.payloadSize = 3;
  assert(archive.add(replacement) == Archive::AddResult::OverwroteOldest);
  assert(epochReplay.poll(true, true, true, false, 1, Local, wrapReader,
                          Sink::emit, &sink, packet) == Replay::Result::Emitted);
  assert(packet.id == 42 && epochReplay.progress().nextIndex == 1);

  // A missing reader/emit callback fails closed without invoking any radio
  // pathway or materializing a packet.
  Replay invalid;
  auto noReader = Replay::Reader{};
  assert(invalid.poll(true, true, true, false, 1, Local, noReader,
                      Sink::emit, &sink, packet) == Replay::Result::ReaderUnavailable);
  assert(invalid.poll(true, true, true, false, 1, Local, wrapReader,
                      nullptr, &sink, packet) == Replay::Result::ReaderUnavailable);
  puts("PASS disconnect/config gating and archive epoch reset; no radio/API fallback on missing callbacks");
}
}  // namespace

int main() {
  testFilterMetadataAndBackpressure();
  testScanBoundAndMalformedRecords();
  testLifecycleAndEpoch();
  puts("RESULT PASS StoreForwardLocalReplay proposal tests");
}
