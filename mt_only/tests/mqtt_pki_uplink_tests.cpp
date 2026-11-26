#include "network/MqttPkiUplink.h"

#include <assert.h>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pb_decode.h"

namespace {
using mt::network::MqttPkiUplink;
unsigned groups = 0;
void pass(const char *name) { ++groups; printf("PASS %s\n", name); }

constexpr uint32_t kLocalNode = 0x12345678;

void frame(uint8_t *out, size_t frameSize = 20) {
  memset(out, 0, frameSize);
  const uint32_t to = 0x87654321, from = 0x12345678, id = 0xaabbccdd;
  for (unsigned i = 0; i < 4; ++i) {
    out[i] = uint8_t(to >> (8 * i));
    out[4 + i] = uint8_t(from >> (8 * i));
    out[8 + i] = uint8_t(id >> (8 * i));
  }
  out[12] = uint8_t(3 | (5 << 5) | 8);  // hop limit/start + want_ack
  out[14] = 0x22; out[15] = 0x33;
  for (size_t i = 16; i < frameSize; ++i) out[i] = uint8_t(i ^ 0xa5u);
}

bool encode(uint32_t localNode, const char *gateway, const uint8_t *f, size_t n,
            char *topic, size_t topicCap, uint8_t *payload, size_t payloadCap,
            size_t *payloadSize) {
  return MqttPkiUplink::encode(localNode, f, n, "meshroot", gateway, topic,
                               topicCap, payload, payloadCap, payloadSize);
}

bool encodeTransit(uint32_t localNode, const char *gateway, const uint8_t *f,
                   size_t n, char *topic, size_t topicCap, uint8_t *payload,
                   size_t payloadCap, size_t *payloadSize) {
  return MqttPkiUplink::encodeTransit(localNode, f, n, "meshroot", gateway,
      topic, topicCap, payload, payloadCap, payloadSize);
}

void rejectsWithoutMutation(uint32_t localNode,
                            const uint8_t *f, size_t n,
                            size_t topicCap = 160, size_t payloadCap = 1024,
                            const char *gateway = "!12345678") {
  char topic[160]; memset(topic, 0x5a, sizeof(topic));
  uint8_t payload[1024]; memset(payload, 0xa5, sizeof(payload));
  size_t written = 0x1234;
  assert(!encode(localNode, gateway, f, n, topic, topicCap, payload, payloadCap, &written));
  assert(written == 0x1234);
  for (char cbyte : topic) assert(static_cast<uint8_t>(cbyte) == 0x5a);
  for (uint8_t byte : payload) assert(byte == 0xa5);
}

void validRoundTrip() {
  uint8_t f[20]; frame(f, sizeof(f));
  char topic[160]{}; uint8_t payload[1024]{}; size_t written = 0;
  assert(encode(kLocalNode, "!12345678", f, sizeof(f), topic, sizeof(topic), payload,
                sizeof(payload), &written));
  assert(strcmp(topic, "meshroot/2/e/PKI/!12345678") == 0);
  assert(written > 0 && written <= sizeof(payload));

  meshtastic_ServiceEnvelope e = meshtastic_ServiceEnvelope_init_zero;
  pb_istream_t input = pb_istream_from_buffer(payload, written);
  assert(pb_decode(&input, meshtastic_ServiceEnvelope_fields, &e));
  assert(input.bytes_left == 0 && e.packet && e.channel_id && e.gateway_id);
  assert(strcmp(e.channel_id, "PKI") == 0);
  assert(strcmp(e.gateway_id, "!12345678") == 0);
  assert(e.packet->from == 0x12345678 && e.packet->to == 0x87654321);
  assert(e.packet->id == 0xaabbccdd && e.packet->pki_encrypted);
  assert(e.packet->which_payload_variant == meshtastic_MeshPacket_encrypted_tag);
  assert(e.packet->encrypted.size == 4);
  assert(memcmp(e.packet->encrypted.bytes, f + 16, 4) == 0);
  assert(e.packet->next_hop == f[14] && e.packet->relay_node == f[15]);
  assert(e.packet->hop_limit == 3 && e.packet->hop_start == 5 && e.packet->want_ack);
  assert(written <= 435 && strlen(topic) < 60);
  pb_release(meshtastic_ServiceEnvelope_fields, &e);
  pass("local-success-pki-ciphertext-preserved");
}

void maximumFrameBudget() {
  uint8_t f[255]; frame(f, sizeof(f));
  char root[32]; memset(root, 'r', 31); root[31] = '\0';
  char topic[60]{}; uint8_t payload[435]{}; size_t written = 0;
  assert(MqttPkiUplink::encode(kLocalNode, f, sizeof(f), root,
      "!12345678", topic, sizeof(topic), payload, sizeof(payload), &written));
  assert(strlen(topic) < 60 && written <= sizeof(payload));
  assert(strcmp(topic + 31, "/2/e/PKI/!12345678") == 0);
  meshtastic_ServiceEnvelope e = meshtastic_ServiceEnvelope_init_zero;
  pb_istream_t input = pb_istream_from_buffer(payload, written);
  assert(pb_decode(&input, meshtastic_ServiceEnvelope_fields, &e));
  assert(input.bytes_left == 0 && e.packet);
  assert(e.packet->encrypted.size == 239);
  assert(memcmp(e.packet->encrypted.bytes, f + 16, 239) == 0);
  assert(e.packet->next_hop == f[14] && e.packet->relay_node == f[15]);
  pb_release(meshtastic_ServiceEnvelope_fields, &e);
  pass("239-byte-private-ciphertext-fits-435-byte-proxy-and-60-byte-topic");
}

void validTransitRoundTrip() {
  uint8_t f[20]; frame(f, sizeof(f));
  const uint32_t remoteFrom = 0x22334455;
  for (unsigned i = 0; i < 4; ++i) f[4 + i] = uint8_t(remoteFrom >> (8 * i));
  char topic[60]{}; uint8_t payload[435]{}; size_t written = 0;
  assert(MqttPkiUplink::encodeTransit(kLocalNode,f,sizeof(f),"meshroot","!12345678",topic,
      sizeof(topic),payload,sizeof(payload),&written,1792660000,true));
  assert(strcmp(topic, "meshroot/2/e/PKI/!12345678") == 0);
  assert(written > 0 && written <= sizeof(payload));

  meshtastic_ServiceEnvelope e = meshtastic_ServiceEnvelope_init_zero;
  pb_istream_t input = pb_istream_from_buffer(payload, written);
  assert(pb_decode(&input, meshtastic_ServiceEnvelope_fields, &e));
  assert(input.bytes_left == 0 && e.packet && e.channel_id && e.gateway_id);
  assert(strcmp(e.channel_id, "PKI") == 0 && strcmp(e.gateway_id, "!12345678") == 0);
  assert(e.packet->has_rx_time&&e.packet->rx_time==1792660000);
  assert(e.packet->from == remoteFrom && e.packet->to == 0x87654321);
  assert(e.packet->id == 0xaabbccdd && e.packet->pki_encrypted);
  assert(e.packet->which_payload_variant == meshtastic_MeshPacket_encrypted_tag);
  assert(e.packet->encrypted.size == 4 && memcmp(e.packet->encrypted.bytes, f + 16, 4) == 0);
  assert(e.packet->next_hop == f[14] && e.packet->relay_node == f[15]);
  assert(e.packet->hop_limit == 3 && e.packet->hop_start == 5 && e.packet->want_ack);
  assert(!e.packet->via_mqtt);
  assert(written <= 435 && strlen(topic) < 60);
  pb_release(meshtastic_ServiceEnvelope_fields, &e);

  // The public local-origin entry point must not accept a relayed frame.
  char unchangedTopic[60]; memset(unchangedTopic, 0x5a, sizeof(unchangedTopic));
  uint8_t unchangedPayload[435]; memset(unchangedPayload, 0xa5, sizeof(unchangedPayload));
  size_t unchangedSize = 0x1234;
  assert(!MqttPkiUplink::encode(kLocalNode, f, sizeof(f), "meshroot", "!12345678",
      unchangedTopic, sizeof(unchangedTopic), unchangedPayload,
      sizeof(unchangedPayload), &unchangedSize));
  assert(unchangedSize == 0x1234);
  for (char c : unchangedTopic) assert(static_cast<uint8_t>(c) == 0x5a);
  for (uint8_t b : unchangedPayload) assert(b == 0xa5);
  pass("typed-transit-origin-and-ciphertext-preserved");
}

void maximumTransitFrameBudget() {
  uint8_t f[255]; frame(f, sizeof(f));
  const uint32_t remoteFrom = 0x33445566;
  for (unsigned i = 0; i < 4; ++i) f[4 + i] = uint8_t(remoteFrom >> (8 * i));
  char root[32]; memset(root, 'r', 31); root[31] = '\0';
  char topic[60]{}; uint8_t payload[435]{}; size_t written = 0;
  assert(MqttPkiUplink::encodeTransit(kLocalNode, f, sizeof(f), root,
      "!12345678", topic, sizeof(topic), payload, sizeof(payload), &written));
  assert(strlen(topic) < 60 && written <= sizeof(payload));
  assert(strcmp(topic + 31, "/2/e/PKI/!12345678") == 0);
  meshtastic_ServiceEnvelope e = meshtastic_ServiceEnvelope_init_zero;
  pb_istream_t input = pb_istream_from_buffer(payload, written);
  assert(pb_decode(&input, meshtastic_ServiceEnvelope_fields, &e));
  assert(input.bytes_left == 0 && e.packet);
  assert(e.packet->from == remoteFrom && e.packet->to == 0x87654321);
  assert(e.packet->encrypted.size == 239);
  assert(memcmp(e.packet->encrypted.bytes, f + 16, 239) == 0);
  assert(e.packet->next_hop == f[14] && e.packet->relay_node == f[15]);
  pb_release(meshtastic_ServiceEnvelope_fields, &e);
  pass("239-byte-transit-ciphertext-fits-existing-proxy-and-topic-bounds");
}

void transitOriginAndInputBounds() {
  uint8_t f[20]; frame(f, sizeof(f));
  char topic[60]; uint8_t payload[435]; size_t written;
  auto rejectsTransit = [&](const uint8_t *bytes, size_t n) {
    memset(topic, 0x5a, sizeof(topic)); memset(payload, 0xa5, sizeof(payload));
    written = 0x1234;
    assert(!encodeTransit(kLocalNode, "!12345678", bytes, n, topic,
        sizeof(topic), payload, sizeof(payload), &written));
    assert(written == 0x1234);
    for (char c : topic) assert(static_cast<uint8_t>(c) == 0x5a);
    for (uint8_t b : payload) assert(b == 0xa5);
  };
  // A transit frame must have a valid remote source and a non-MQTT origin.
  frame(f, sizeof(f)); rejectsTransit(f, sizeof(f)); // still local-origin
  frame(f, sizeof(f));
  const uint32_t badSources[] = {0, 1, 0xfffffffeu, 0xffffffffu};
  for (uint32_t from : badSources) {
    for (unsigned i = 0; i < 4; ++i) f[4 + i] = uint8_t(from >> (8 * i));
    rejectsTransit(f, sizeof(f));
  }
  frame(f, sizeof(f));
  const uint32_t badDestinations[] = {1, kLocalNode, 0xfffffffeu, 0xffffffffu};
  for (uint32_t to : badDestinations) {
    for (unsigned i = 0; i < 4; ++i) f[i] = uint8_t(to >> (8 * i));
    const uint32_t remoteFrom = 0x22334455;
    for (unsigned i = 0; i < 4; ++i) f[4 + i] = uint8_t(remoteFrom >> (8 * i));
    rejectsTransit(f, sizeof(f));
  }
  frame(f, sizeof(f)); f[12] |= 0x10u;
  const uint32_t remoteFrom = 0x22334455;
  for (unsigned i = 0; i < 4; ++i) f[4 + i] = uint8_t(remoteFrom >> (8 * i));
  rejectsTransit(f, sizeof(f));
  uint8_t oversized[256]; frame(oversized, sizeof(oversized));
  for (unsigned i = 0; i < 4; ++i) oversized[4 + i] = uint8_t(remoteFrom >> (8 * i));
  rejectsTransit(oversized, sizeof(oversized));
  pass("transit-origin-destination-via-mqtt-and-size-bounds-fail-closed");
}

void admissionAndBounds() {
  uint8_t f[20]; frame(f, sizeof(f));
  rejectsWithoutMutation(0, f, sizeof(f));
  rejectsWithoutMutation(1, f, sizeof(f));
  rejectsWithoutMutation(0xfffffffeu, f, sizeof(f));
  rejectsWithoutMutation(0xffffffffu, f, sizeof(f));
  rejectsWithoutMutation(kLocalNode ^ 1u, f, sizeof(f));
  rejectsWithoutMutation(kLocalNode, f, 16); rejectsWithoutMutation(kLocalNode, f, 256);
  rejectsWithoutMutation(kLocalNode, f, sizeof(f), 4);  // topic capacity
  rejectsWithoutMutation(kLocalNode, f, sizeof(f), 160, 1);  // payload capacity
  rejectsWithoutMutation(kLocalNode, f, sizeof(f), 160, 1024, "!");
  rejectsWithoutMutation(kLocalNode, f, sizeof(f), 160, 1024, "!abcd");
  rejectsWithoutMutation(kLocalNode, f, sizeof(f), 160, 1024, "!123456789");
  pass("local-id-and-output-bounds-fail-closed");
}

void frameValidation() {
  uint8_t f[20]; frame(f, sizeof(f));
  f[12] |= 0x10; rejectsWithoutMutation(kLocalNode, f, sizeof(f));  // MQTT echo
  frame(f, sizeof(f)); f[12] = uint8_t(6 | (2 << 5)); rejectsWithoutMutation(kLocalNode, f, sizeof(f));
  frame(f, sizeof(f)); f[4] ^= 1; rejectsWithoutMutation(kLocalNode, f, sizeof(f));  // wrong origin
  frame(f, sizeof(f)); memset(f, 0, 4); rejectsWithoutMutation(kLocalNode, f, sizeof(f));  // invalid dest
  const uint32_t badDestinations[] = {1, 0xfffffffeu, 0xffffffffu};
  for (uint32_t to : badDestinations) {
    frame(f, sizeof(f));
    for (unsigned i = 0; i < 4; ++i) f[i] = uint8_t(to >> (8 * i));
    rejectsWithoutMutation(kLocalNode, f, sizeof(f));
  }
  uint8_t oversized[256]; frame(oversized, sizeof(oversized));
  rejectsWithoutMutation(kLocalNode, oversized, sizeof(oversized));  // 240 ciphertext bytes
  pass("frame-shape-origin-and-hop-bounds-fail-closed");
}
}  // namespace

int main() {
  validRoundTrip();
  maximumFrameBudget();
  validTransitRoundTrip();
  maximumTransitFrameBudget();
  transitOriginAndInputBounds();
  admissionAndBounds();
  frameValidation();
  printf("PASS_GROUPS %u\n", groups);
  return 0;
}
