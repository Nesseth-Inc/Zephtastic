// SPDX-License-Identifier: GPL-3.0-only
#define CONFIG_MT_STORE_FORWARD_PSRAM 1
#include <errno.h>
// The host fixture also builds against Darwin libc, which lacks this Zephyr errno.
#ifndef EKEYREJECTED
#define EKEYREJECTED 129
#endif
#include "modules/StoreForwardRuntime.h"

#include "meshtastic/mesh.pb.h"
#include "meshtastic/storeforward.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace {
constexpr uint32_t Self = 0x12345678u;
constexpr uint32_t Sender = 0x24680135u;
constexpr uint32_t Requester = 0x13572468u;

struct Radio final : mt::Radio {
  int state = 0;
  int sendStatus = 0;
  int completionStatus = 0;
  bool completionReady = false;
  unsigned sends = 0, starts = 0, stops = 0;
  uint8_t lastFrame[255]{};
  size_t lastSize = 0;
  bool ready() override { return true; }
  int configure(const mt::Config &) override { return 0; }
  int start() override { ++starts; state = 1; return 0; }
  int restore() override { state = 1; return 0; }
  int stop() override { ++stops; state = 0; return 0; }
  int health() override { return state; }
  bool busy() override { return false; }
  uint32_t airtime(size_t) override { return 100; }
  int send(uint8_t *frame, size_t size) override {
    ++sends;
    assert(size <= sizeof(lastFrame));
    lastSize = size;
    memcpy(lastFrame, frame, size);
    return sendStatus;
  }
  bool completion(int &result) override {
    if (!completionReady) return false;
    completionReady = false;
    result = completionStatus;
    return true;
  }
};

struct Private final : mt::pki::PrivateRuntime {
  uint32_t local = Self;
  unsigned storeForwardEncodes = 0;
  explicit Private(uint32_t node = Self) : local(node) {}
  uint32_t peer() const override { return Sender; }
  bool ready(uint32_t node) const override { return node == local; }
  bool publicIdentity(uint8_t out[32]) const override { memset(out, 0x42, 32); return true; }
  bool bootstrap(bench::MtCodec &, const bench::MtNodeRecord &, uint8_t, uint32_t,
                 uint32_t, bool, uint32_t, bench::Frame &) override { return false; }
  bool encodeStoreForward(bench::MtCodec &codec,
                          const mt::modules::StoreForwardPacket &packet,
                          uint32_t id, bench::Frame &out) override {
    ++storeForwardEncodes;
    size_t size = 0;
    if (!codec.encodeZeroHopData(packet.from(), packet.to(), id, packet.data(),
                                 packet.size(), out.bytes, sizeof(out.bytes), size))
      return false;
    out.len = size;
    out.bytes[12] = packet.flags();
    return true;
  }
  void freeze() override {}
  bool matchesPeer(uint32_t node) const override { return node == Sender; }
  bool revoke(uint32_t node) override { return node == Sender; }
  void clearEvent() override {}
  void sweep(mt::Endpoint &) override {}
  int submit(mt::Endpoint &, uint8_t, uint32_t, uint32_t, const uint8_t *,
             size_t, unsigned, bool, const uint8_t *, size_t, bool,
             uint32_t) override { return -ENOTSUP; }
  void receive(mt::Endpoint &, bench::MtCodec &, const uint8_t *, size_t,
               uint32_t, uint32_t, bool) override {}
  void proof(mt::Endpoint &, const uint8_t *, size_t,
             const bench::RouteMetadata &, uint32_t) override {}
  bool event(uint8_t, const uint8_t *, size_t,
             mt::pki::PrivateEvent &) override { return false; }
};

struct Clock {
  uint64_t now = 0;
  static uint64_t read(void *context) {
    return static_cast<Clock *>(context)->now;
  }
};

struct Fixture {
  Clock clock;
  Radio radio;
  Private privateRuntime;
  mt::Service service;
  mt::modules::StoreForwardRuntime runtime;
  mt::modules::StoreForwardSettings settings;
  bool psramReady = true;
  uint32_t now = 0;
  uint32_t utc = 200000;
  uint8_t channelHash = 0;

  Fixture(bool server, bool publicPrimary = false, uint32_t records = 20, bool secondary = false)
      : service(radio, true, Self, 81, true, false, &privateRuntime) {
    assert(service.channelMeter().bind({Clock::read, &clock}));
    service.channelMeter().supported(true);

    mt::Config config;
    config.bytes[4] = meshtastic_Config_LoRaConfig_RegionCode_US;
    auto node = config.node();
    if (!publicPrimary) {
      uint8_t rawKey[32]{};
      for (size_t i = 0; i < sizeof(rawKey); ++i) rawKey[i] = uint8_t(0x51u + i);
      mt::PrimaryChannel::Selection key{};
      assert(mt::PrimaryChannel::normalize(rawKey, sizeof(rawKey), key, "LongFast"));
      channelHash = key.hash;
      node.setKey(key.hash, key.bytes, key.length);
      mt::PrimaryChannel::clear(key);
    } else {
      node.setPublic();
      mt::PrimaryChannel::Selection key{};
      assert(mt::PrimaryChannel::normalize(node.bytes + bench::MtNodeRecord::Key,
                                            node.bytes[3], key, "LongFast"));
      channelHash = key.hash;
      mt::PrimaryChannel::clear(key);
    }
    config.node(node);
    if (secondary) {
      mt::ChannelRecord channel;
      assert(channel.set(2, "Archive", nullptr, 0, 0, false));
      assert(config.setSecondaryChannel(1, channel));
    }
    assert(config.setDeviceRole(meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE,
                                meshtastic_Config_DeviceConfig_RebroadcastMode_ALL));
    config.setTxEnabled(true);
    assert(config.valid() && config.provisioned());
    assert(service.apply(config));
    assert(service.bindStoreForwardSink(&Fixture::storeForwardIngress, this));
    assert(service.start(true, 0));
    assert(service.armEndpoint(true));

    meshtastic_ModuleConfig_StoreForwardConfig values =
        meshtastic_ModuleConfig_StoreForwardConfig_init_zero;
    values.enabled = true;
    values.is_server = server;
    values.heartbeat = true;
    values.records = records;
    values.history_return_max = 20;
    values.history_return_window = 240;
    assert(settings.set(values));
    runtime.poll(service, settings, true, false, psramReady, now, 7);
    assert(runtime.snapshot().enabled);
  }

  static void storeForwardIngress(void *context, const uint8_t *frame,
                                  size_t size, const bench::RouteMetadata &m,
                                  unsigned channel) {
    auto &self = *static_cast<Fixture *>(context);
    self.runtime.receive(self.service, frame, size, m, uint8_t(channel),
                         self.now, self.utc, self.now / 1000u);
  }

  void setNow(uint32_t milliseconds, uint32_t epoch = 200000) {
    now = milliseconds;
    utc = epoch;
    clock.now = milliseconds;
  }

  std::vector<uint8_t> frame(uint32_t from, uint32_t to, uint32_t id,
                             meshtastic_PortNum port, const uint8_t *payload,
                             size_t payloadSize, uint8_t flags = 0x91,
                             uint32_t replyId = 0, bool emoji = false, unsigned channel = 0) {
    meshtastic_Data data = meshtastic_Data_init_zero;
    data.portnum = port;
    data.payload.size = pb_size_t(payloadSize);
    if (payloadSize) memcpy(data.payload.bytes, payload, payloadSize);
    data.has_bitfield = true;
    data.bitfield = 1;
    data.reply_id = replyId;
    data.emoji = emoji;
    uint8_t encoded[239]{};
    auto output = pb_ostream_from_buffer(encoded, sizeof(encoded));
    assert(pb_encode(&output, meshtastic_Data_fields, &data));

    mt::PrimaryChannel::Selection key{};
    assert(service.config.channelSelection(channel, key));
    bench::MtCodec sender;
    assert(sender.setKey(key.hash, key.bytes, key.length));
    mt::PrimaryChannel::clear(key);
    uint8_t bytes[255]{};
    size_t size = 0;
    assert(sender.encodeZeroHopData(from, to, id, encoded, output.bytes_written,
                                    bytes, sizeof(bytes), size));
    bytes[12] = flags;
    return std::vector<uint8_t>(bytes, bytes + size);
  }

  std::vector<uint8_t> textFrame(uint32_t from, uint32_t id,
                                 const char *text = "archived text",
                                 uint32_t replyId = 0, bool emoji = false) {
    return frame(from, UINT32_MAX, id, meshtastic_PortNum_TEXT_MESSAGE_APP,
                 reinterpret_cast<const uint8_t *>(text), strlen(text), 0x91,
                 replyId, emoji);
  }

  std::vector<uint8_t> storeForwardFrame(
      uint32_t from, uint32_t to, uint32_t id,
      meshtastic_StoreAndForward_RequestResponse rr,
      bool includeHistory = false, uint32_t window = 1,
      bool includeHeartbeat = false, unsigned channel = 0) {
    meshtastic_StoreAndForward message = meshtastic_StoreAndForward_init_zero;
    message.rr = rr;
    if (includeHistory) {
      message.which_variant = meshtastic_StoreAndForward_history_tag;
      message.variant.history.window = window;
    }
    if (includeHeartbeat) {
      message.which_variant = meshtastic_StoreAndForward_heartbeat_tag;
      message.variant.heartbeat.period = 900;
    }
    return reportFrame(from,to,id,message,channel);
  }

  std::vector<uint8_t> reportFrame(uint32_t from, uint32_t to, uint32_t id,
                                   const meshtastic_StoreAndForward &message, uint8_t channel=0) {
    uint8_t nested[244]{};
    size_t nestedSize = 0;
    assert(mt::store_forward::StoreForwardWire::encode(
        message, nested, sizeof(nested), &nestedSize));
    return frame(from, to, id, meshtastic_PortNum_STORE_FORWARD_APP,
                 nested, nestedSize, 0x84, 0, false, channel);
  }

  void rx(const std::vector<uint8_t> &wire, uint32_t at,
          uint32_t epoch = 200000) {
    setNow(at, epoch);
    service.rx(wire.data(), wire.size(), -70, at, 99, true);
  }

  void pollRuntime(uint32_t at, uint32_t epoch = 200000) {
    setNow(at, epoch);
    runtime.poll(service, settings, true, false, psramReady, at, 101);
  }

  void sendQueued(uint32_t at) {
    setNow(at);
    service.poll(at, 303);
  }

  void completeSend(uint32_t at) {
    radio.completionReady = true;
    setNow(at);
    service.poll(at, 404);
  }
};

meshtastic_Data decodeRadioData(Fixture &fixture) {
  assert(fixture.radio.sends > 0 && fixture.radio.lastSize >= 17);
  bench::RouteMetadata metadata{};
  assert(fixture.service.codec.decode(fixture.radio.lastFrame,
                                     fixture.radio.lastSize, metadata));
  assert(metadata.channelDecoded);
  meshtastic_Data data = meshtastic_Data_init_zero;
  data.portnum = static_cast<meshtastic_PortNum>(metadata.portnum);
  data.payload.size = metadata.payloadLength;
  memcpy(data.payload.bytes, metadata.payload, metadata.payloadLength);
  data.request_id = metadata.request_id;
  data.reply_id = metadata.reply_id;
  data.emoji = metadata.emoji;
  data.want_response = metadata.wantResponse;
  data.has_bitfield = metadata.hasBitfield;
  data.bitfield = metadata.bitfield;
  return data;
}

meshtastic_StoreAndForward decodeStoreForward(const meshtastic_Data &data) {
  assert(data.portnum == meshtastic_PortNum_STORE_FORWARD_APP);
  meshtastic_StoreAndForward message = meshtastic_StoreAndForward_init_zero;
  assert(mt::store_forward::StoreForwardWire::decode(
      data.payload.bytes, data.payload.size, &message));
  return message;
}

void testCustomTextAdmissionAndFiltering() {
  Fixture fixture(true, false, 12);
  auto first = fixture.textFrame(Sender, 10, "kept", 0x99887766u, true);
  fixture.rx(first, 10, 200000);
  assert(fixture.runtime.snapshot().memory.usedRecords == 1);
  assert(fixture.runtime.snapshot().admitted == 1);
  fixture.rx(first, 20, 200001);
  assert(fixture.runtime.snapshot().memory.usedRecords == 1);
  assert(fixture.runtime.snapshot().admitted == 1);

  auto wrongPort = fixture.frame(Sender, UINT32_MAX, 11,
      meshtastic_PortNum_POSITION_APP,
      reinterpret_cast<const uint8_t *>("wrong"), 5);
  fixture.rx(wrongPort, 30);
  assert(fixture.runtime.snapshot().rejected == 1);

  const uint8_t unknownRr[] = {0x08, 0xff, 0x01};
  auto unknown = fixture.frame(Sender, Self, 12,
      meshtastic_PortNum_STORE_FORWARD_APP, unknownRr, sizeof(unknownRr));
  fixture.rx(unknown, 40);
  assert(fixture.runtime.snapshot().rejected == 2);

  auto opaque = first;
  opaque[13] = uint8_t(opaque[13] + 1);
  const auto rejected = fixture.runtime.snapshot().rejected;
  fixture.rx(opaque, 50);
  assert(fixture.runtime.snapshot().rejected == rejected);
  assert(fixture.runtime.snapshot().memory.usedRecords == 1);

  printf("PASS custom RF TEXT admission, duplicate suppression, invalid port/rr rejection, opaque channel exclusion\n");
}

void testLegacyTextHistoryRequest() {
  const uint8_t text[] = {'S', 'F', 0, 'x'};
  for (size_t length : {2u, 3u, 4u}) {
    Fixture f(true, false, 12);
    const auto wire=f.frame(Requester, Self, 88, meshtastic_PortNum_TEXT_MESSAGE_APP, text, length);
    f.rx(wire, 1000);
    f.rx(wire, 1001);
    f.pollRuntime(1010);
    f.sendQueued(1260);
    assert(f.radio.sends==1 && f.runtime.snapshot().memory.usedRecords==0);
    const auto reply=decodeStoreForward(decodeRadioData(f));
    assert(reply.rr==meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY);
  }
  Fixture f(true, false, 12);
  f.rx(f.textFrame(Requester, 89, "SF"),1000); // A broadcast is ordinary text.
  const uint8_t longer[] = {'S', 'F', 'x'};
  f.rx(f.frame(Requester, Self, 90, meshtastic_PortNum_TEXT_MESSAGE_APP, longer, sizeof(longer)),1010);
  assert(f.runtime.snapshot().memory.usedRecords==2);
}

void testHistoryQueueAndReplayMetadata() {
  Fixture fixture(true, false, 12);
  auto fromRequester = fixture.textFrame(Requester, 20, "private self");
  auto fromOther = fixture.textFrame(Sender, 21, "saved original", 0x11223344u, true);
  fixture.rx(fromRequester, 10, 200000);
  fixture.rx(fromOther, 20, 200001);
  assert(fixture.runtime.snapshot().memory.usedRecords == 2);

  auto history = fixture.storeForwardFrame(Requester, Self, 22,
      meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY, true, 2);
  fixture.rx(history, 1000, 200002);
  fixture.pollRuntime(1000, 200002);
  assert(fixture.runtime.snapshot().queued == 1);
  assert(!fixture.runtime.snapshot().pending);
  assert(fixture.radio.sends == 0);
  fixture.sendQueued(1250);
  assert(fixture.radio.sends == 1);
  auto control = decodeStoreForward(decodeRadioData(fixture));
  assert(control.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY);
  assert(control.which_variant == meshtastic_StoreAndForward_history_tag);
  assert(control.variant.history.history_messages == 1); // requester excluded
  assert(control.variant.history.last_request == 0);
  assert((fixture.radio.lastFrame[12] & 8u) == 0); // no ACK request
  fixture.completeSend(1300);
  fixture.sendQueued(5999);
  assert(fixture.radio.sends == 1); // five-second stock pacing
  fixture.pollRuntime(6000, 200002);
  assert(fixture.runtime.snapshot().pending);
  fixture.pollRuntime(6001, 200002);
  assert(fixture.runtime.snapshot().queued == 2);
  fixture.sendQueued(6251);
  assert(fixture.radio.sends == 2);
  const auto replayData = decodeRadioData(fixture);
  const auto replay = decodeStoreForward(replayData);
  assert(replay.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_BROADCAST);
  assert(replay.original_id == 21);
  assert(replay.variant.text.size == strlen("saved original"));
  assert(memcmp(replay.variant.text.bytes, "saved original", replay.variant.text.size) == 0);
  assert(fixture.radio.lastFrame[4] == uint8_t(Sender));
  assert(fixture.radio.lastFrame[13] == fixture.channelHash);
  assert(replayData.reply_id == 0x11223344u && replayData.emoji);
  assert((fixture.radio.lastFrame[12] & 0x10u) != 0);
  fixture.completeSend(6300);
  fixture.sendQueued(10000);
  assert(fixture.radio.sends == 2); // module queue does not retry
  printf("PASS history response and five-second paced replay preserve original message metadata without ACK/retry\n");
}

void testOfflineHistoryWindowUsesUptimeDomain() {
  Fixture fixture(true, false, 8);
  fixture.rx(fixture.textFrame(Sender, 60, "old boot-time text"), 10000, 0);
  fixture.rx(fixture.textFrame(Sender + 1, 61, "recent boot-time text"), 200000, 0);
  assert(fixture.runtime.snapshot().memory.usedRecords == 2);
  const auto *oldRecord=fixture.runtime.localArchive()->recordAt(0);
  const auto *recentRecord=fixture.runtime.localArchive()->recordAt(1);
  assert(oldRecord&&oldRecord->time==10&&!oldRecord->hasRxTime);
  assert(recentRecord&&recentRecord->time==200&&!recentRecord->hasRxTime);

  auto history=fixture.storeForwardFrame(Requester,Self,62,
      meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY,true,1);
  fixture.rx(history,201000,0);
  fixture.pollRuntime(201000,0);
  assert(fixture.runtime.snapshot().queued==1&&!fixture.runtime.snapshot().pending);
  fixture.sendQueued(201250);
  const auto response=decodeStoreForward(decodeRadioData(fixture));
  assert(response.rr==meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY);
  assert(response.variant.history.history_messages==1);
  printf("PASS offline RF history timestamp and request window share uptime-seconds domain; quality remains unanchored\n");
}

void testOversizeReplayDoesNotCancelTail() {
  Fixture fixture(true, false, 8);
  const std::string tooLong(230, 'x');
  fixture.rx(fixture.textFrame(Sender, 70, tooLong.c_str()), 10, 200000);
  fixture.rx(fixture.textFrame(Sender, 71, "tail survives"), 20, 200001);
  assert(fixture.runtime.snapshot().memory.usedRecords == 2);

  auto history = fixture.storeForwardFrame(Requester, Self, 72,
      meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY, true, 2);
  fixture.rx(history, 1000, 200002);
  fixture.pollRuntime(1000, 200002);
  fixture.sendQueued(1250);
  assert(fixture.radio.sends == 1);
  fixture.completeSend(1300);

  fixture.pollRuntime(6000, 200002); // first, too-large replay is skipped
  assert(fixture.runtime.snapshot().dropped == 1);
  assert(fixture.runtime.snapshot().active);
  fixture.pollRuntime(11000, 200002); // next saved record remains selectable
  assert(fixture.runtime.snapshot().pending);
  fixture.pollRuntime(11001, 200002);
  assert(fixture.runtime.snapshot().queued == 2);
  fixture.sendQueued(11251);
  assert(fixture.radio.sends == 2);
  const auto replay = decodeStoreForward(decodeRadioData(fixture));
  assert(replay.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_BROADCAST);
  assert(replay.original_id == 71);
  assert(replay.variant.text.size == strlen("tail survives"));
  assert(memcmp(replay.variant.text.bytes, "tail survives", replay.variant.text.size) == 0);
  printf("PASS an unrepresentable nonfinal replay is skipped and the transfer tail continues\n");
}

void testAbortCancelsPendingTransferAndReadinessRecovers() {
  Fixture fixture(true, false, 8);
  fixture.rx(fixture.textFrame(Sender, 80), 10);
  assert(fixture.runtime.snapshot().memory.usedRecords == 1);
  auto history = fixture.storeForwardFrame(Requester, Self, 81,
      meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY, true, 2);
  fixture.rx(history, 100, 200000);
  assert(fixture.runtime.snapshot().active && fixture.runtime.snapshot().pending);

  auto abort = fixture.storeForwardFrame(Requester, Self, 82,
      meshtastic_StoreAndForward_RequestResponse_CLIENT_ABORT);
  fixture.rx(abort, 110, 200000);
  assert(!fixture.runtime.snapshot().active && !fixture.runtime.snapshot().pending);
  fixture.pollRuntime(120);
  assert(fixture.runtime.snapshot().queued == 0 && fixture.radio.sends == 0);

  fixture.psramReady = false;
  fixture.pollRuntime(140);
  assert(!fixture.runtime.snapshot().enabled);
  assert(!fixture.runtime.snapshot().memory.available);
  fixture.psramReady = true;
  fixture.pollRuntime(150);
  assert(fixture.runtime.snapshot().enabled);
  assert(fixture.runtime.snapshot().memory.available);
  assert(fixture.runtime.snapshot().memory.usedRecords == 0);
  printf("PASS abort cancels a staged response; PSRAM readiness edge restores an empty archive\n");
}

void testTransferChannel() {
  Fixture fixture(true, false, 20, true);
  fixture.rx(fixture.textFrame(Sender, 201), 50);
  const auto request = meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY;
  const auto stop = meshtastic_StoreAndForward_RequestResponse_CLIENT_ABORT;
  fixture.rx(fixture.storeForwardFrame(Requester, Self, 202, request, true), 100);
  assert(fixture.runtime.snapshot().active);
  fixture.rx(fixture.storeForwardFrame(Requester, Self, 203, request, true, 1, false, 1), 110);
  fixture.rx(fixture.storeForwardFrame(Requester, Self, 204, stop, false, 1, false, 1), 120);
  assert(fixture.runtime.snapshot().active);
  fixture.rx(fixture.storeForwardFrame(Requester, Self, 205, stop), 130);
  assert(!fixture.runtime.snapshot().active);
  puts("PASS transfer retains its opening channel across other traffic from the requester");
}

void testBackpressuredControlIsNotRemembered() {
  Fixture fixture(true, false, 8);
  auto ping = fixture.storeForwardFrame(Requester, Self, 90,
      meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS);
  auto retryPing = fixture.storeForwardFrame(Requester, Self, 91,
      meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS);
  fixture.rx(ping, 100);
  assert(fixture.runtime.snapshot().pending);
  auto deferred = fixture.storeForwardFrame(Requester, Self, 92,
      meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS);
  fixture.rx(deferred, 105);
  assert(fixture.runtime.snapshot().dropped == 0);
  fixture.rx(retryPing, 110); // both the immediate and the deferred slot are occupied
  assert(fixture.runtime.snapshot().dropped == 1);

  for (uint32_t t = 120; t < 4800 && fixture.radio.sends < 2; t += 60) {
    fixture.pollRuntime(t);
    fixture.sendQueued(t + 10);
    if (fixture.radio.sends) fixture.completeSend(t + 20);
  }
  assert(fixture.runtime.snapshot().queued == 2 && fixture.radio.sends == 2);
  fixture.rx(retryPing, 6000);
  assert(fixture.runtime.snapshot().pending);
  printf("PASS a control dropped under pending-slot backpressure remains eligible on retry\n");
}

void testServerRejectsBroadcastRequestsButClientAcceptsHeartbeat() {
  Fixture server(true, false, 8);
  server.rx(server.textFrame(Sender, 28), 20);
  const auto broadcastHistory = server.storeForwardFrame(Requester, UINT32_MAX, 29,
      meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY, true, 2);
  server.rx(broadcastHistory, 100);
  assert(!server.runtime.snapshot().active && !server.runtime.snapshot().pending);
  const auto broadcastPing = server.storeForwardFrame(Requester, UINT32_MAX, 30,
      meshtastic_StoreAndForward_RequestResponse_CLIENT_PING);
  server.rx(broadcastPing, 110);
  assert(!server.runtime.snapshot().active && !server.runtime.snapshot().pending);
  assert(server.runtime.snapshot().queued == 0 && server.radio.sends == 0);

  const auto unicastHistory = server.storeForwardFrame(Requester, Self, 31,
      meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY, true, 2);
  server.rx(unicastHistory, 120);
  assert(server.runtime.snapshot().active && server.runtime.snapshot().pending);

  Fixture client(false, false, 8);
  const auto broadcastHeartbeat = client.storeForwardFrame(Sender, UINT32_MAX, 32,
      meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT,
      false, 1, true);
  client.rx(broadcastHeartbeat, 130);
  assert(client.runtime.snapshot().heartbeatAt == 130);
  assert(!client.runtime.snapshot().pending);
  printf("PASS server ignores broadcast CLIENT_HISTORY/CLIENT_PING; client accepts broadcast heartbeat\n");
}

void testClientRouterInformation() {
  Fixture client(false, false, 8);
  meshtastic_StoreAndForward msg{};
  msg.rr=meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS;
  msg.which_variant=meshtastic_StoreAndForward_stats_tag;
  msg.variant.stats.messages_max=3000; msg.variant.stats.requests=81;
  msg.variant.stats.requests_history=17; msg.variant.stats.heartbeat=true;
  msg.variant.stats.return_max=100; msg.variant.stats.return_window=360;
  client.rx(client.reportFrame(Sender,Self,311,msg),50);
  auto info=client.runtime.snapshot().clientInfo;
  assert(info.records==3000 && info.requests==81 && info.historyRequests==17 && info.heartbeat &&
         info.returnMax==100 && info.returnWindowMinutes==360);
  msg.rr=meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY;
  msg.which_variant=meshtastic_StoreAndForward_history_tag;
  msg.variant.history.window=14400001;
  client.rx(client.reportFrame(Sender,UINT32_MAX,312,msg),80);
  client.rx(client.reportFrame(Sender,Requester,313,msg),90);
  assert(client.runtime.snapshot().clientInfo.returnWindowMinutes==360);
  client.rx(client.reportFrame(Sender,Self,314,msg),100);
  info=client.runtime.snapshot().clientInfo;
  assert(info.returnWindowMinutes==240 && info.records==3000 && !client.runtime.snapshot().pending);
  client.runtime.poll(client.service, client.settings, false, false, true, 101, 1);
  info=client.runtime.snapshot().clientInfo;
  assert(info.records==0 && !info.heartbeat && info.returnMax==25);
  puts("PASS directed router information uses wire units and resets with its trust domain");
}

void testStatsPublicDenialAndClientPong() {
  Fixture server(true, true, 5);
  auto publicHistory = server.storeForwardFrame(Requester, Self, 31,
      meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY, true, 1);
  server.rx(publicHistory, 100, 200000);
  server.pollRuntime(100);
  server.sendQueued(350);
  const auto denial = decodeRadioData(server);
  assert(denial.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP);
  assert(memcmp(denial.payload.bytes, "S&F not permitted on the public channel.",
                strlen("S&F not permitted on the public channel.")) == 0);
  server.completeSend(400);

  auto stats = server.storeForwardFrame(Requester, Self, 32,
      meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS);
  server.rx(stats, 500);
  server.pollRuntime(500);
  server.sendQueued(750);
  const auto statsData = decodeRadioData(server);
  const auto statsMessage = decodeStoreForward(statsData);
  assert(statsMessage.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS);
  assert(statsMessage.variant.stats.messages_total == 5);
  assert(statsMessage.variant.stats.messages_max == 5);
  server.completeSend(800);

  Fixture client(false, false, 5);
  auto ping = client.storeForwardFrame(Sender, Self, 41,
      meshtastic_StoreAndForward_RequestResponse_ROUTER_PING);
  client.rx(ping, 100);
  client.pollRuntime(100);
  client.sendQueued(350);
  const auto pong = decodeStoreForward(decodeRadioData(client));
  assert(pong.rr == meshtastic_StoreAndForward_RequestResponse_CLIENT_PONG);
  client.completeSend(400);
  printf("PASS public-channel denial, CLIENT_STATS, and client ROUTER_PING to CLIENT_PONG\n");
}

void testRouterRoleTransitions() {
  Fixture f(false);
  auto change=[&](unsigned role,unsigned now){
    f.service.stop();
    auto c=f.service.config;
    assert(c.setDeviceRole(role,meshtastic_Config_DeviceConfig_RebroadcastMode_ALL));
    assert(f.service.apply(c)&&f.service.start(true,now)&&f.service.armEndpoint(true));
    f.pollRuntime(now);
  };
  auto packet=10u;
  for(unsigned role:{2u,11u,12u,0u}){
    change(role,++packet*100);
    const bool router=role==2||role==11;
    assert(f.runtime.snapshot().enabled&&f.runtime.snapshot().server==router);
    assert((f.runtime.localArchive()!=nullptr)==router);
    meshtastic_ModuleConfig_StoreForwardConfig raw{};
    assert(f.settings.get(raw)&&raw.enabled&&!raw.is_server);
    f.rx(f.textFrame(Sender,packet),packet*100+10);
    if(router){
      assert(f.runtime.snapshot().memory.usedRecords==1);
      f.rx(f.storeForwardFrame(Requester,Self,packet+100,meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY),packet*100+20);
      assert(f.runtime.snapshot().active&&f.runtime.snapshot().pending);
    }
  }
  change(2,2000);f.psramReady=false;f.pollRuntime(2010);assert(!f.runtime.snapshot().enabled);
  change(0,2030);assert(f.runtime.snapshot().enabled&&!f.runtime.snapshot().server&&!f.runtime.localArchive());
  puts("PASS ROUTER and ROUTER_LATE select server with raw is_server false; client transition clears pending replay and recovers without PSRAM");
}
void testConfigEpochAndQueueFailure() {
  Fixture fixture(true, false, 4);
  fixture.rx(fixture.textFrame(Sender, 50), 10);
  assert(fixture.runtime.snapshot().memory.usedRecords == 1);

  // Change only the Store Forward record capacity: runtime resets volatile data.
  meshtastic_ModuleConfig_StoreForwardConfig values =
      meshtastic_ModuleConfig_StoreForwardConfig_init_zero;
  values.enabled = true;
  values.is_server = true;
  values.records = 7;
  values.history_return_max = 20;
  values.history_return_window = 240;
  mt::modules::StoreForwardSettings changed;
  assert(changed.set(values));
  fixture.settings = changed;
  fixture.pollRuntime(20);
  assert(fixture.runtime.snapshot().memory.capacity == 7);
  assert(fixture.runtime.snapshot().memory.usedRecords == 0);

  fixture.rx(fixture.textFrame(Sender, 51), 30);
  assert(fixture.runtime.snapshot().memory.usedRecords == 1);
  fixture.service.stop();
  auto config = fixture.service.config;
  auto node = config.node();
  uint8_t rawKey[32]{};
  memset(rawKey, 0x72, sizeof(rawKey));
  mt::PrimaryChannel::Selection key{};
  assert(mt::PrimaryChannel::normalize(rawKey, sizeof(rawKey), key, "LongFast"));
  node.setKey(key.hash, key.bytes, key.length);
  mt::PrimaryChannel::clear(key);
  config.node(node);
  assert(fixture.service.apply(config));
  assert(fixture.service.start(true, 40) && fixture.service.armEndpoint(true));
  fixture.pollRuntime(40);
  assert(fixture.runtime.snapshot().memory.usedRecords == 0); // core-channel epoch reset

  // Fill the common Service module queue first, then make runtime stage a
  // response. Runtime must leave it pending and never claim TX queued.
  mt::modules::StoreForwardPacket blocker;
  meshtastic_StoreAndForward blockerMessage = meshtastic_StoreAndForward_init_zero;
  blockerMessage.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_BUSY;
  assert(mt::modules::StoreForwardPacket::control(Self, Requester, 0, 3,
                                                  blockerMessage, blocker));
  uint32_t id = 0;
  assert(fixture.service.submitStoreForward(blocker, 50, 808, id) == 0);
  const auto encodesBefore = fixture.privateRuntime.storeForwardEncodes;
  const auto startsBefore = fixture.radio.starts;
  const auto stopsBefore = fixture.radio.stops;
  fixture.rx(fixture.storeForwardFrame(Requester, Self, 60,
      meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS), 100);
  fixture.pollRuntime(100);
  assert(fixture.runtime.snapshot().pending);
  assert(fixture.runtime.snapshot().queued == 0);
  assert(fixture.privateRuntime.storeForwardEncodes == encodesBefore);
  assert(fixture.radio.sends == 0);
  assert(fixture.radio.starts == startsBefore && fixture.radio.stops == stopsBefore);
  fixture.sendQueued(350);
  assert(fixture.radio.sends == 1);
  fixture.completeSend(400);
  fixture.pollRuntime(401);
  assert(fixture.runtime.snapshot().queued == 1);
  assert(!fixture.runtime.snapshot().pending);
  assert(fixture.privateRuntime.storeForwardEncodes == encodesBefore + 1);
  fixture.sendQueued(651);
  assert(fixture.radio.sends == 2);
  printf("PASS settings/core-channel epoch clears and queue failure does not claim a TX\n");
}
}  // namespace

int main() {
  testLegacyTextHistoryRequest();
  testTransferChannel();
  testCustomTextAdmissionAndFiltering();
  testHistoryQueueAndReplayMetadata();
  testOfflineHistoryWindowUsesUptimeDomain();
  testOversizeReplayDoesNotCancelTail();
  testAbortCancelsPendingTransferAndReadinessRecovers();
  testBackpressuredControlIsNotRemembered();
  testServerRejectsBroadcastRequestsButClientAcceptsHeartbeat();
  testClientRouterInformation();
  testStatsPublicDenialAndClientPong();
  testRouterRoleTransitions();
  testConfigEpochAndQueueFailure();
  puts("RESULT PASS StoreForwardRuntime integration fixture");
}
