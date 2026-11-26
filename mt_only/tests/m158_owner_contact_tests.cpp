// SPDX-License-Identifier: GPL-3.0-only
// Native projection tests for typed owner-configured SharedContact values.
#include "OwnerContact.h"

#include "MtSignature.h"
#include "monocypher.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned checks = 0;
unsigned groups = 0;

#define CHECK(expr) do { ++checks; if (!(expr)) { \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
  return false; } } while (0)

constexpr uint32_t kLocal = 0x10203040u;
constexpr uint32_t kPeer = 0x76543210u;  // Deliberately not derived from a key.

meshtastic_SharedContact stockContact(uint32_t node = kPeer) {
  meshtastic_SharedContact c{};
  c.node_num = node;
  c.has_user = true;
  std::snprintf(c.user.id, sizeof(c.user.id), "!%08x", unsigned(node));
  std::snprintf(c.user.long_name, sizeof(c.user.long_name), "Owner configured peer");
  std::snprintf(c.user.short_name, sizeof(c.user.short_name), "OWN");
  c.user.hw_model = meshtastic_HardwareModel_TBEAM;
  c.user.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
  return c;
}

void makeAdmissibleKey(uint8_t key[32]) {
  uint8_t scalar[32]{};
  for (unsigned i = 0; i < sizeof(scalar); ++i)
    scalar[i] = uint8_t(0x37u + 19u * i);
  scalar[0] &= 0xf8;
  scalar[31] = uint8_t((scalar[31] & 0x7f) | 0x40);
  crypto_x25519_public_key(key, scalar);
  crypto_wipe(scalar, sizeof(scalar));
}

bool sameCandidate(const mt::OwnerContactCandidate &a,
                   const mt::OwnerContactCandidate &b) {
  return !std::memcmp(a.node.bytes, b.node.bytes, sizeof(a.node.bytes)) &&
         a.node.snr == b.node.snr && a.node.hops == b.node.hops &&
         a.node.heard == b.node.heard && a.node.viaMqtt == b.node.viaMqtt &&
         !std::memcmp(a.node.lastHeardBytes, b.node.lastHeardBytes,
                      sizeof(a.node.lastHeardBytes)) &&
         !std::memcmp(a.user.id, b.user.id, sizeof(a.user.id)) &&
         !std::memcmp(a.user.long_name, b.user.long_name, sizeof(a.user.long_name)) &&
         !std::memcmp(a.user.short_name, b.user.short_name, sizeof(a.user.short_name)) &&
         a.user.hw_model == b.user.hw_model && a.user.role == b.user.role &&
         a.user.public_key.size == b.user.public_key.size &&
         !std::memcmp(a.user.public_key.bytes, b.user.public_key.bytes,
                      sizeof(a.user.public_key.bytes)) &&
         a.peer.node == b.peer.node &&
         !std::memcmp(a.peer.publicKey, b.peer.publicKey, sizeof(a.peer.publicKey)) &&
         a.shouldIgnore == b.shouldIgnore &&
         a.manualVerificationRequested == b.manualVerificationRequested &&
         a.hasPublicKey == b.hasPublicKey &&
         a.pkiKeyAdmissible == b.pkiKeyAdmissible &&
         a.hasPeerPin == b.hasPeerPin;
}

mt::OwnerContactCandidate sentinel() {
  mt::OwnerContactCandidate out{};
  std::memset(out.node.bytes, 0xa5, sizeof(out.node.bytes));
  out.node.snr = -7; out.node.hops = 5; out.node.heard = true;
  out.node.viaMqtt = true;
  std::memset(out.node.lastHeardBytes, 0x6c, sizeof(out.node.lastHeardBytes));
  std::memset(out.user.id, 'I', sizeof(out.user.id));
  std::memset(out.user.long_name, 'L', sizeof(out.user.long_name));
  std::memset(out.user.short_name, 'S', sizeof(out.user.short_name));
  out.user.hw_model = meshtastic_HardwareModel_TBEAM;
  out.user.role = meshtastic_Config_DeviceConfig_Role_ROUTER;
  out.user.public_key.size = sizeof(out.user.public_key.bytes);
  std::memset(out.user.public_key.bytes, 0x39, sizeof(out.user.public_key.bytes));
  out.peer.node = 0xabcdef01u;
  std::memset(out.peer.publicKey, 0x72, sizeof(out.peer.publicKey));
  out.shouldIgnore = true; out.manualVerificationRequested = true;
  out.hasPublicKey = true; out.pkiKeyAdmissible = true; out.hasPeerPin = true;
  return out;
}

bool invalidUnchanged(const meshtastic_SharedContact &c) {
  auto output = sentinel();
  const auto before = output;
  CHECK(mt::OwnerContact::decode(c, kLocal, output) < 0);
  CHECK(sameCandidate(output, before));
  return true;
}

bool keylessAndPolicy() {
  auto c = stockContact();
  c.should_ignore = true;
  c.manually_verified = true;
  mt::OwnerContactCandidate out{};
  CHECK(mt::OwnerContact::decode(c, kLocal, out) == 0);
  CHECK(out.node.id() == kPeer);
  CHECK(out.node.model() == uint32_t(meshtastic_HardwareModel_TBEAM));
  CHECK(out.node.role() == uint32_t(meshtastic_Config_DeviceConfig_Role_CLIENT));
  CHECK(!out.node.ignored());
  CHECK(!std::strcmp(out.user.long_name, "Owner configured peer"));
  CHECK(!std::strcmp(out.user.short_name, "OWN"));
  CHECK(out.shouldIgnore && out.manualVerificationRequested);
  CHECK(!out.hasPublicKey && !out.pkiKeyAdmissible && !out.hasPeerPin);
  ++groups;
  return true;
}

bool ownerKeyDoesNotNeedNodeIdBinding() {
  auto c = stockContact();
  uint8_t key[32]{}, edwards[32]{};
  makeAdmissibleKey(key);
  CHECK(bench::MtSignature::admissiblePublicKey(key, edwards));
  CHECK(bench::MtSignature::nodeId(key) != c.node_num);
  c.user.public_key.size = sizeof(key);
  std::memcpy(c.user.public_key.bytes, key, sizeof(key));
  mt::OwnerContactCandidate out{};
  CHECK(mt::OwnerContact::decode(c, kLocal, out) == 0);
  CHECK(out.hasPublicKey && out.pkiKeyAdmissible && out.hasPeerPin);
  CHECK(out.peer.node == c.node_num);
  CHECK(!std::memcmp(out.peer.publicKey, key, sizeof(key)));
  CHECK(!out.node.ignored());
  ++groups;
  return true;
}

bool ignoredKeyRetainsContactWithoutPin() {
  auto c = stockContact();
  uint8_t key[32]{};
  makeAdmissibleKey(key);
  c.user.public_key.size = sizeof(key);
  std::memcpy(c.user.public_key.bytes, key, sizeof(key));
  c.should_ignore = true;
  mt::OwnerContactCandidate out{};
  CHECK(mt::OwnerContact::decode(c, kLocal, out) == 0);
  CHECK(!out.node.ignored() && out.shouldIgnore);
  CHECK(out.hasPublicKey && out.pkiKeyAdmissible);
  CHECK(out.node.bytes[mt::NodeCache::Entry::KeyLength] == 32);
  CHECK(!std::memcmp(out.node.bytes + mt::NodeCache::Entry::Key, key, sizeof(key)));
  CHECK(!out.hasPeerPin && out.peer.node == 0);
  ++groups;
  return true;
}

bool inadmissibleKeyCannotCreatePin() {
  auto c = stockContact();
  c.user.public_key.size = sizeof(c.user.public_key.bytes);
  std::memset(c.user.public_key.bytes, 0, sizeof(c.user.public_key.bytes));
  uint8_t edwards[32]{};
  CHECK(!bench::MtSignature::admissiblePublicKey(c.user.public_key.bytes, edwards));
  mt::OwnerContactCandidate out{};
  CHECK(mt::OwnerContact::decode(c, kLocal, out) == 0);
  CHECK(out.hasPublicKey && !out.pkiKeyAdmissible && !out.hasPeerPin);
  CHECK(out.node.bytes[mt::NodeCache::Entry::KeyLength] == 32);
  ++groups;
  return true;
}

bool malformedInputsRefuseWithoutChangingOutput() {
  auto c = stockContact();
  c.node_num = kLocal;
  CHECK(invalidUnchanged(c));
  c = stockContact(); c.node_num = 1;
  CHECK(invalidUnchanged(c));
  c = stockContact(); c.node_num = 0xfffffffeu;
  CHECK(invalidUnchanged(c));
  c = stockContact(); c.has_user = false;
  CHECK(invalidUnchanged(c));
  c = stockContact(); c.user.public_key.size = 16;
  CHECK(invalidUnchanged(c));
  c = stockContact(); c.user.public_key.size = sizeof(c.user.public_key.bytes) + 1;
  CHECK(invalidUnchanged(c));
  c = stockContact(); std::memset(c.user.id, 'I', sizeof(c.user.id));
  CHECK(invalidUnchanged(c));
  c = stockContact(); std::memset(c.user.long_name, 'L', sizeof(c.user.long_name));
  CHECK(invalidUnchanged(c));
  c = stockContact(); std::memset(c.user.short_name, 'S', sizeof(c.user.short_name));
  CHECK(invalidUnchanged(c));
  ++groups;
  return true;
}
}  // namespace

int main() {
  if (!keylessAndPolicy() || !ownerKeyDoesNotNeedNodeIdBinding() ||
      !ignoredKeyRetainsContactWithoutPin() || !inadmissibleKeyCannotCreatePin() ||
      !malformedInputsRefuseWithoutChangingOutput()) return 1;
  std::printf("PASS M158 owner contact projection groups=%u checks=%u\n", groups, checks);
  return 0;
}
