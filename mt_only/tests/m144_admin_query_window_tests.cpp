// SPDX-License-Identifier: GPL-3.0-only
// Synthetic public keys and protobuf responses only; no radio or identity use.
#include "AdminQueryWindow.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

using mt::admin::AdminQueryWindow;
using mt::admin::remote_protocol::Operation;
using mt::admin::remote_protocol::Query;

namespace {
constexpr uint32_t LocalNode = 0x12345678u;
constexpr uint32_t PeerNode = 0x23456789u;
constexpr uint32_t OtherPeer = 0x3456789au;
constexpr uint32_t Lifetime = 1000;

struct Wire {
  uint8_t bytes[mt::admin::remote_protocol::MaxDataBytes]{};
  size_t size = 0;
};

void fillKey(uint8_t key[32], uint8_t marker) {
  memset(key, marker, 32);
}

Wire response(const Query &query, uint32_t originalRequestId,
              uint32_t dataRequestId = 0, bool wrongVariant = false,
              int channelIndex = -1) {
  meshtastic_AdminMessage admin{};
  admin.session_passkey.size = mt::admin::remote_protocol::SessionPasskeyBytes;
  memset(admin.session_passkey.bytes, 0xA6, admin.session_passkey.size);
  if (wrongVariant || query.operation == Operation::Owner) {
    admin.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    admin.get_owner_response = meshtastic_User_init_zero;
  } else if (query.operation == Operation::Channel) {
    admin.which_payload_variant = meshtastic_AdminMessage_get_channel_response_tag;
    admin.get_channel_response.index = channelIndex >= 0
        ? channelIndex : static_cast<int32_t>(query.selector - 1);
    admin.get_channel_response.settings = meshtastic_ChannelSettings_init_zero;
  } else if (query.operation == Operation::Config) {
    admin.which_payload_variant = meshtastic_AdminMessage_get_config_response_tag;
    admin.get_config_response.which_payload_variant =
        meshtastic_Config_lora_tag;
    admin.get_config_response.payload_variant.lora = meshtastic_Config_LoRaConfig_init_zero;
  } else {
    assert(false && "unsupported test query kind");
  }

  uint8_t payload[mt::admin::remote_protocol::MaxPayloadBytes]{};
  auto payloadStream = pb_ostream_from_buffer(payload, sizeof(payload));
  assert(pb_encode(&payloadStream, meshtastic_AdminMessage_fields, &admin));

  meshtastic_Data data{};
  data.portnum = meshtastic_PortNum_ADMIN_APP;
  data.payload.size = static_cast<pb_size_t>(payloadStream.bytes_written);
  memcpy(data.payload.bytes, payload, data.payload.size);
  data.has_bitfield = true;
  data.bitfield = 0;
  data.request_id = dataRequestId ? dataRequestId : originalRequestId;
  Wire wire{};
  auto stream = pb_ostream_from_buffer(wire.bytes, sizeof(wire.bytes));
  assert(pb_encode(&stream, meshtastic_Data_fields, &data));
  wire.size = stream.bytes_written;
  return wire;
}

void recordAndMergeRequireExactCorrelation() {
  AdminQueryWindow window;
  uint8_t key[32], otherKey[32];
  fillKey(key, 0x31); fillKey(otherKey, 0x52);
  const Query query{Operation::Channel, 3};
  assert(window.recordConfirmedEnqueue(PeerNode, key, 100, query, 0x01, 10, Lifetime) ==
         AdminQueryWindow::RecordResult::Recorded);
  assert(window.recordConfirmedEnqueue(PeerNode, key, 100, query, 0x04, 20, 2000) ==
         AdminQueryWindow::RecordResult::MergedOwner);
  assert(window.size() == 1);
  assert(window.recordConfirmedEnqueue(PeerNode, key, 100,
      Query{Operation::Channel, 4}, 0x08, 30, Lifetime) ==
      AdminQueryWindow::RecordResult::IdCollision);
  assert(window.recordConfirmedEnqueue(PeerNode, otherKey, 100, query,
      0x08, 30, Lifetime) == AdminQueryWindow::RecordResult::IdCollision);
  assert(window.recordConfirmedEnqueue(OtherPeer, key, 100, query,
      0x08, 30, Lifetime) == AdminQueryWindow::RecordResult::IdCollision);
  assert(window.size() == 1);
  assert(window.canRecord(PeerNode, key, 100, query, 0x08, 30, Lifetime) ==
         AdminQueryWindow::RecordResult::MergedOwner);
  assert(window.canRecord(PeerNode, key, 100, Query{Operation::Channel, 4},
      0x08, 30, Lifetime) == AdminQueryWindow::RecordResult::IdCollision);
  assert(window.size() == 1); // preflight never merges owner bits

  const Wire wire = response(query, 100);
  // The first false probe models a response whose durable replay commit fails.
  assert(window.responseOwners(LocalNode, PeerNode, key, 100, wire.bytes,
      wire.size, 31, false) == 0x05 && window.size() == 1);
  assert(window.responseOwners(LocalNode, PeerNode, key, 100, wire.bytes,
      wire.size, 31, false) == 0x05 && window.size() == 1);
  // Service calls consume=true only after Fresh replay commit.
  assert(window.responseOwners(LocalNode, PeerNode, key, 100, wire.bytes,
      wire.size, 31, true) == 0x05 && window.size() == 0);
  assert(window.responseOwners(LocalNode, PeerNode, key, 100, wire.bytes,
      wire.size, 32, true) == 0);
}

void matchRequiresLivePeerAndExactFreshResponseCorrelation() {
  uint8_t key[32], changedKey[32];
  fillKey(key, 0x63); fillKey(changedKey, 0x74);
  const Query query{Operation::Channel, 5};
  AdminQueryWindow window;
  assert(window.recordConfirmedEnqueue(PeerNode, key, 201, query, 0x02, 100, Lifetime) ==
         AdminQueryWindow::RecordResult::Recorded);
  const Wire good = response(query, 201);
  auto tryReject = [&](const Wire &wire, uint32_t from, uint32_t to,
                       const uint8_t authenticatedKey[32]) {
    assert(window.responseOwners(to, from, authenticatedKey, 201, wire.bytes,
        wire.size, 101, false) == 0);
    assert(window.size() == 1);
  };
  tryReject(good, OtherPeer, LocalNode, key); // wrong peer node
  tryReject(good, PeerNode, LocalNode, changedKey); // current authenticated key rotated
  tryReject(response(query, 201, 202), PeerNode, LocalNode, key); // wrong Data.request_id
  tryReject(response(query, 201, 0, true), PeerNode, LocalNode, key); // wrong variant
  tryReject(response(query, 201, 0, false, 2), PeerNode, LocalNode, key); // wrong channel

  assert(window.responseOwners(LocalNode, PeerNode, key, 0, good.bytes,
      good.size, 101, false) == 0); // zero response packet ID
  uint8_t zeroKey[32]{};
  assert(window.responseOwners(LocalNode, PeerNode, zeroKey, 201, good.bytes,
      good.size, 101, false) == 0);

  assert(window.responseOwners(LocalNode, PeerNode, key, 201, good.bytes,
      good.size, 102, true) == 0x02 && window.size() == 0);
}

void boundedCapacityCancellationAndRevocation() {
  AdminQueryWindow window;
  uint8_t key[32]; fillKey(key, 0x85);
  const Query query{Operation::Owner, 0};
  for (uint32_t i = 0; i < AdminQueryWindow::SlotCount; ++i) {
    assert(window.recordConfirmedEnqueue(PeerNode + i, key, 300 + i,
        query, uint32_t(1) << i, 50, Lifetime) == AdminQueryWindow::RecordResult::Recorded);
  }
  assert(window.size() == 4);
  assert(window.canRecord(OtherPeer, key, 400, query, 0x10, 50, Lifetime) ==
         AdminQueryWindow::RecordResult::Full);
  assert(window.size() == 4);
  assert(window.recordConfirmedEnqueue(OtherPeer, key, 400, query, 0x10,
      50, Lifetime) == AdminQueryWindow::RecordResult::Full);

  assert(window.cancelOwners(0x01) == 1 && window.size() == 3);
  assert(window.cancelPeer(PeerNode + 1) == 1 && window.size() == 2);
  assert(window.cancelOwners(0x04 | 0x08) == 2 && window.size() == 0);
  assert(window.recordConfirmedEnqueue(PeerNode, key, 500, query, 0x20,
      60, Lifetime) == AdminQueryWindow::RecordResult::Recorded);
  assert(window.cancelPeer(PeerNode) == 1 && window.size() == 0); // disconnect or key revocation
}

void exactRequestCancellationPreservesUnrelatedQueries() {
  AdminQueryWindow window;
  uint8_t key[32], otherKey[32];
  fillKey(key, 0xA1); fillKey(otherKey, 0xB2);
  const Query query{Operation::Owner, 0};

  assert(window.recordConfirmedEnqueue(PeerNode, key, 701, query, 0x01,
      10, Lifetime) == AdminQueryWindow::RecordResult::Recorded);
  assert(window.recordConfirmedEnqueue(PeerNode, key, 701, query, 0x04,
      11, Lifetime) == AdminQueryWindow::RecordResult::MergedOwner);
  assert(window.recordConfirmedEnqueue(PeerNode, key, 702, query, 0x02,
      12, Lifetime) == AdminQueryWindow::RecordResult::Recorded);
  assert(window.recordConfirmedEnqueue(OtherPeer, key, 704, query, 0x08,
      13, Lifetime) == AdminQueryWindow::RecordResult::Recorded);
  assert(window.recordConfirmedEnqueue(PeerNode, otherKey, 703, query, 0x10,
      14, Lifetime) == AdminQueryWindow::RecordResult::Recorded);
  assert(window.size() == 4);

  const uint8_t zeroKey[32]{};
  assert(!window.cancelRequest(PeerNode, key, 0));
  assert(!window.cancelRequest(1, key, 701));
  assert(!window.cancelRequest(0xfffffffeu, key, 701));
  assert(!window.cancelRequest(PeerNode, nullptr, 701));
  assert(!window.cancelRequest(PeerNode, zeroKey, 701));
  assert(!window.cancelRequest(OtherPeer, key, 702)); // wrong peer
  assert(!window.cancelRequest(PeerNode, otherKey, 701)); // wrong key for target
  assert(!window.cancelRequest(PeerNode, key, 799)); // wrong request ID
  assert(window.size() == 4);

  assert(window.cancelRequest(PeerNode, key, 701));
  assert(window.size() == 3); // the merged slot's two owners are removed together
  assert(!window.cancelRequest(PeerNode, key, 701));
  assert(window.cancelRequest(PeerNode, key, 702));
  assert(window.cancelRequest(OtherPeer, key, 704));
  assert(window.cancelRequest(PeerNode, otherKey, 703));
  assert(window.size() == 0);
}

void expirationUsesWrapSafeElapsedTimeAndInputsAreBounded() {
  AdminQueryWindow window;
  uint8_t key[32]; fillKey(key, 0x96);
  const Query query{Operation::Config, 5};
  const uint32_t start = 0xfffffff0u;
  assert(window.recordConfirmedEnqueue(PeerNode, key, 601, query, 0x40,
      start, 100) == AdminQueryWindow::RecordResult::Recorded);
  assert(window.expire(start + 99) == 0 && window.size() == 1);
  assert(window.expire(start + 100) == 1 && window.size() == 0);

  const uint8_t zeroKey[32]{};
  assert(window.recordConfirmedEnqueue(PeerNode, key, 0, query, 1, 0, Lifetime) ==
         AdminQueryWindow::RecordResult::Invalid);
  assert(window.recordConfirmedEnqueue(PeerNode, zeroKey, 602, query, 1, 0, Lifetime) ==
         AdminQueryWindow::RecordResult::Invalid);
  assert(window.recordConfirmedEnqueue(PeerNode, key, 603,
      Query{Operation::Invalid, 0}, 1, 0, Lifetime) == AdminQueryWindow::RecordResult::Invalid);
  assert(window.recordConfirmedEnqueue(PeerNode, key, 604, query, 0, 0, Lifetime) ==
         AdminQueryWindow::RecordResult::Invalid);
  assert(window.recordConfirmedEnqueue(PeerNode, key, 605, query, 1, 0, 0) ==
         AdminQueryWindow::RecordResult::Invalid);
  assert(window.recordConfirmedEnqueue(PeerNode, key, 606, query, 1, 0,
      0x80000000u) == AdminQueryWindow::RecordResult::Invalid);
  assert(window.size() == 0);
}
}  // namespace

int main() {
  printf("AdminQueryWindow sizeof=%zu bytes\n", sizeof(AdminQueryWindow));
  recordAndMergeRequireExactCorrelation();
  puts("PASS Admin query owner merge and request-ID collision rules");
  matchRequiresLivePeerAndExactFreshResponseCorrelation();
  puts("PASS Admin query matches only current peer/key and exact canonical response");
  boundedCapacityCancellationAndRevocation();
  puts("PASS Admin query capacity, owner cancellation, and peer revocation");
  exactRequestCancellationPreservesUnrelatedQueries();
  puts("PASS Admin query exact cancellation preserves unrelated and merged ownership");
  expirationUsesWrapSafeElapsedTimeAndInputsAreBounded();
  puts("PASS Admin query wrap-safe expiry and bounded input validation");
}
