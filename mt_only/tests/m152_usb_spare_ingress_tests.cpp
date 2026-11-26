// SPDX-License-Identifier: GPL-3.0-only
// Real Companion ToRadio ingress coverage for the one exact USB spare query.
#define CONFIG_MT_PKI_STOCK_TRIAL 1
#include "Companion.h"
#include "pki/PsaPkiBackend.h"

extern "C" psa_status_t mbedtls_psa_external_get_random(
    mbedtls_psa_external_random_context_t *, uint8_t *, size_t, size_t *length) {
  *length = 0;
  return PSA_ERROR_INSUFFICIENT_ENTROPY;
}

#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>

namespace {
constexpr uint32_t LocalNode = 0x12345678u;
constexpr uint8_t Model = 127;
constexpr char Command[] = "zt_storage_spare_status_v1";
constexpr char Reply[] = "storage_spare_status_v1 bytes=131072 scanned=131072 ff=131072 nonff=0 complete=1 rc=0 read_only=1";
constexpr char CapacityCommand[] = "zt_storage_capacity_status_v1";
constexpr char CapacityReply[] = "capacity_v1 nrc=0 nf=12000 scanned=131072 sectors=32 rc=0 ff=4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096";

struct TestRadio final : mt::Radio {
  unsigned sends = 0;
  bool ready() override { return true; }
  int start() override { return 0; }
  int restore() override { return 0; }
  int stop() override { return 0; }
  int health() override { return 1; }
  bool busy() override { return false; }
  uint32_t airtime(size_t) override { return 1; }
  int send(uint8_t *, size_t) override { ++sends; return -EPERM; }
  bool completion(int &result) override { result = 0; return false; }
};

struct TestHost final : mt::Companion::Host {
  unsigned provisionCalls = 0;
  unsigned saveCalls = 0;
  unsigned previews = 0;
  unsigned acceptedFrameChecks = 0;
  uint32_t epoch = 0;
  std::vector<uint8_t> provisioned;
  std::vector<meshtastic_FromRadio> emitted;

  bool ready() const override { return true; }
  int save(const mt::Config &) override { ++saveCalls; return -EPERM; }
  bool acceptTransportFrame(uint32_t capturedEpoch) override {
    ++acceptedFrameChecks;
    return capturedEpoch == 7;
  }
  bool send(const uint8_t *bytes, size_t size) override {
    assert(bytes && size >= 4 && bytes[0] == 0x94 && bytes[1] == 0xc3);
    assert(size_t(bytes[2] * 256u + bytes[3]) == size - 4);
    meshtastic_FromRadio message{};
    auto input = pb_istream_from_buffer(bytes + 4, size - 4);
    assert(pb_decode(&input, meshtastic_FromRadio_fields, &message) && !input.bytes_left);
    emitted.push_back(message);
    return true;
  }
  void preview(uint32_t, uint32_t, uint32_t, unsigned, const uint8_t *, size_t,
               int16_t, int8_t, uint32_t, bool) override { ++previews; }
  int provision(const uint8_t *bytes, size_t size, uint32_t capturedEpoch,
                char *out, size_t capacity) override {
    ++provisionCalls;
    epoch = capturedEpoch;
    provisioned.assign(bytes, bytes + size);
    const bool capacityQuery = size == sizeof(CapacityCommand) - 1 &&
        memcmp(bytes, CapacityCommand, sizeof(CapacityCommand) - 1) == 0;
    const char *reply = capacityQuery ? CapacityReply : Reply;
    const size_t length = capacityQuery ? sizeof(CapacityReply) - 1 : sizeof(Reply) - 1;
    if (!out || capacity <= length) return -ENOSPC;
    memcpy(out, reply, length);
    return int(length);
  }
};

struct Fixture {
  TestRadio radio;
  mt::Service service{radio, false, LocalNode, Model, false, false};
  TestHost host;
  mt::companion::CompanionWorkspace workspace;
  mt::Companion companion;

  explicit Fixture(uint8_t owner = 0)
      : companion(service, host, Model, workspace, owner) {
    sendHandshake();
    host.emitted.clear();
  }

  void sendFrame(const meshtastic_ToRadio &message, uint32_t epoch = 7,
                 uint32_t now = 10) {
    uint8_t encoded[512]{};
    auto output = pb_ostream_from_buffer(encoded, sizeof(encoded));
    assert(pb_encode(&output, meshtastic_ToRadio_fields, &message));
    const auto proof = mt::Companion::WorkspaceProof{true};
    companion.byte(0x94, now, epoch, proof);
    companion.byte(0xc3, now, epoch, proof);
    companion.byte(uint8_t(output.bytes_written >> 8), now, epoch, proof);
    companion.byte(uint8_t(output.bytes_written), now, epoch, proof);
    for (size_t i = 0; i < output.bytes_written; ++i)
      companion.byte(encoded[i], now, epoch, proof);
  }

  void sendHandshake() {
    meshtastic_ToRadio handshake{};
    handshake.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    handshake.want_config_id = 0x4d152;
    sendFrame(handshake, 7, 1);
    assert(companion.handshakes == 1);
  }

  void sendText(const uint8_t *payload, size_t size, uint32_t id,
                bool canonical = true, uint32_t epoch = 7) {
    meshtastic_ToRadio message{};
    message.which_payload_variant = meshtastic_ToRadio_packet_tag;
    auto &packet = message.packet;
    packet.to = LocalNode;
    packet.id = id;
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    packet.decoded.payload.size = pb_size_t(size);
    memcpy(packet.decoded.payload.bytes, payload, size);
    if (!canonical) packet.decoded.request_id = 1;
    sendFrame(message, epoch, 10 + id);
  }

  meshtastic_Routing_Error lastError() const {
    assert(!host.emitted.empty());
    const auto &packet = host.emitted.back().packet;
    assert(packet.decoded.portnum == meshtastic_PortNum_ROUTING_APP);
    meshtastic_Routing routing{};
    auto input = pb_istream_from_buffer(packet.decoded.payload.bytes,
                                       packet.decoded.payload.size);
    assert(pb_decode(&input, meshtastic_Routing_fields, &routing) && !input.bytes_left);
    return routing.error_reason;
  }

  void assertQueueStatus(size_t index, uint32_t id) const {
    assert(index < host.emitted.size());
    const auto &message = host.emitted[index];
    assert(message.which_payload_variant == meshtastic_FromRadio_queueStatus_tag);
    assert(message.queueStatus.mesh_packet_id == id);
    assert(message.queueStatus.maxlen == 1); // Fixture Service is non-normal mode.
    assert(message.queueStatus.free <= message.queueStatus.maxlen);
  }
};

void testExactSpareCommandUsesConnectedUsbProvisionDispatchOnly() {
  assert(mt::pki::UsbProvisionParser::handles(
      reinterpret_cast<const uint8_t *>(Command), sizeof(Command) - 1));
  constexpr char ExistingPrefix[] = "zt_pki_existing_v1";
  assert(mt::pki::UsbProvisionParser::handles(
      reinterpret_cast<const uint8_t *>(ExistingPrefix), sizeof(ExistingPrefix) - 1));
  constexpr char NearMiss[] = "zt_storage_spare_status_v1x";
  assert(!mt::pki::UsbProvisionParser::handles(
      reinterpret_cast<const uint8_t *>(NearMiss), sizeof(NearMiss) - 1));
  Fixture f;
  f.sendText(reinterpret_cast<const uint8_t *>(Command), sizeof(Command) - 1, 100);
  assert(f.host.provisionCalls == 1);
  assert(f.host.saveCalls == 0);
  assert(f.host.epoch == 7);
  assert(f.host.acceptedFrameChecks == 2); // handshake and exact local command
  assert(f.host.provisioned.size() == sizeof(Command) - 1);
  assert(memcmp(f.host.provisioned.data(), Command, sizeof(Command) - 1) == 0);
  assert(f.host.previews == 0);
  assert(f.companion.localMessages == 0 && f.companion.deniedTx == 0);
  assert(f.radio.sends == 0);
  assert(f.host.emitted.size() == 2);
  f.assertQueueStatus(0, 100);
  assert(f.host.emitted[1].which_payload_variant == meshtastic_FromRadio_packet_tag);
  const auto &response = f.host.emitted[1].packet;
  assert(response.from == LocalNode && response.to == LocalNode);
  assert(response.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP);
  assert(response.decoded.request_id == 100);
  assert(response.decoded.payload.size == sizeof(Reply) - 1);
  assert(memcmp(response.decoded.payload.bytes, Reply, sizeof(Reply) - 1) == 0);
}

void testCapacityCommandUsesSameConnectedUsbReadOnlyDispatch() {
  assert(mt::pki::UsbProvisionParser::handles(
      reinterpret_cast<const uint8_t *>(CapacityCommand), sizeof(CapacityCommand) - 1));
  constexpr char NearMiss[] = "zt_storage_capacity_status_v1x";
  assert(!mt::pki::UsbProvisionParser::handles(
      reinterpret_cast<const uint8_t *>(NearMiss), sizeof(NearMiss) - 1));
  Fixture f;
  f.sendText(reinterpret_cast<const uint8_t *>(CapacityCommand), sizeof(CapacityCommand) - 1, 105);
  assert(f.host.provisionCalls == 1 && f.host.saveCalls == 0 && f.host.epoch == 7);
  assert(f.host.provisioned.size() == sizeof(CapacityCommand) - 1);
  assert(memcmp(f.host.provisioned.data(), CapacityCommand, sizeof(CapacityCommand) - 1) == 0);
  assert(f.host.previews == 0 && f.companion.localMessages == 0 && f.companion.deniedTx == 0);
  assert(f.radio.sends == 0 && f.host.emitted.size() == 2);
  f.assertQueueStatus(0, 105);
  const auto &response = f.host.emitted[1].packet;
  assert(f.host.emitted[1].which_payload_variant == meshtastic_FromRadio_packet_tag);
  assert(response.from == LocalNode && response.to == LocalNode);
  assert(response.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP);
  assert(response.decoded.request_id == 105);
  assert(response.decoded.payload.size == sizeof(CapacityReply) - 1);
  assert(memcmp(response.decoded.payload.bytes, CapacityReply, sizeof(CapacityReply) - 1) == 0);
}

void testNearMissRemainsOrdinaryLocalTextNotProvisioning() {
  Fixture f;
  constexpr char NearMiss[] = "zt_storage_spare_status_v1x";
  f.sendText(reinterpret_cast<const uint8_t *>(NearMiss), sizeof(NearMiss) - 1, 101);
  assert(f.host.provisionCalls == 0);
  assert(f.host.saveCalls == 0);
  assert(f.host.previews == 1);
  assert(f.companion.localMessages == 1);
  assert(f.radio.sends == 0);
  assert(f.host.emitted.size() == 3);
  f.assertQueueStatus(0, 101);
  assert(f.host.emitted[1].which_payload_variant == meshtastic_FromRadio_packet_tag);
  assert(f.host.emitted[1].packet.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP);
  assert(f.host.emitted[1].packet.id == 101);
  assert(f.host.emitted[1].packet.decoded.request_id == 0);
  assert(f.host.emitted[2].which_payload_variant == meshtastic_FromRadio_packet_tag);
  assert(f.host.emitted[2].packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag);
  assert(f.host.emitted[2].packet.decoded.portnum == meshtastic_PortNum_ROUTING_APP);
  assert(f.host.emitted[2].packet.decoded.request_id == 101);
}

void testOtherOwnerAndNoncanonicalExactCommandAreRejected() {
  Fixture otherOwner(1);
  otherOwner.sendText(reinterpret_cast<const uint8_t *>(Command), sizeof(Command) - 1, 102);
  assert(otherOwner.host.provisionCalls == 0);
  assert(otherOwner.host.saveCalls == 0);
  assert(otherOwner.host.previews == 0);
  assert(otherOwner.host.emitted.size() == 3);
  otherOwner.assertQueueStatus(0, 102);
  assert(otherOwner.host.emitted[1].which_payload_variant == meshtastic_FromRadio_clientNotification_tag);
  assert(otherOwner.host.emitted[1].clientNotification.has_reply_id);
  assert(otherOwner.host.emitted[1].clientNotification.reply_id == 102);
  assert(otherOwner.host.emitted[1].clientNotification.level == meshtastic_LogRecord_Level_ERROR);
  assert(otherOwner.lastError() == meshtastic_Routing_Error_BAD_REQUEST);
  assert(otherOwner.radio.sends == 0);

  Fixture malformed;
  malformed.sendText(reinterpret_cast<const uint8_t *>(Command), sizeof(Command) - 1,
                     103, false);
  assert(malformed.host.provisionCalls == 0);
  assert(malformed.host.saveCalls == 0);
  assert(malformed.host.previews == 0);
  assert(malformed.host.emitted.size() == 3);
  malformed.assertQueueStatus(0, 103);
  assert(malformed.host.emitted[1].which_payload_variant == meshtastic_FromRadio_clientNotification_tag);
  assert(malformed.host.emitted[1].clientNotification.has_reply_id);
  assert(malformed.host.emitted[1].clientNotification.reply_id == 103);
  assert(malformed.host.emitted[1].clientNotification.level == meshtastic_LogRecord_Level_ERROR);
  assert(malformed.lastError() == meshtastic_Routing_Error_BAD_REQUEST);
  assert(malformed.radio.sends == 0);

  Fixture staleEpoch;
  staleEpoch.sendText(reinterpret_cast<const uint8_t *>(Command), sizeof(Command) - 1,
                      104, true, 8);
  assert(staleEpoch.host.provisionCalls == 0);
  assert(staleEpoch.host.saveCalls == 0);
  assert(staleEpoch.host.emitted.empty());
  assert(staleEpoch.radio.sends == 0);
}
} // namespace

int main() {
  testExactSpareCommandUsesConnectedUsbProvisionDispatchOnly();
  testCapacityCommandUsesSameConnectedUsbReadOnlyDispatch();
  testNearMissRemainsOrdinaryLocalTextNotProvisioning();
  testOtherOwnerAndNoncanonicalExactCommandAreRejected();
  std::puts("PASS_M153_CAPACITY_AND_M152_USB_SPARE_COMPANION_INGRESS");
  return 0;
}
