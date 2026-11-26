// SPDX-License-Identifier: GPL-3.0-only
#include "pki_runtime_fixture.h"
#include "pki/PkiStartup.h"
#include "pki/PkiFreshUsbIngress.h"
#include <array>

namespace {
struct FixtureSettings : FreshProvisioningSettings {
  Memory &memory;
  explicit FixtureSettings(Memory &m) : memory(m) {}
  int begin() override { return 0; }
  void end() override {}
  int read(const char *k, uint8_t *o, size_t n) override { return memory.read(k, o, n); }
  int persist(const char *k, const uint8_t *b, size_t n) override { return memory.write(k, b, n); }
};
struct Context : ProvisioningContext, PkiStorageContext {
  bool held = false;
  bool tryEnter() override {
    if (held)
      return false;
    held = true;
    return true;
  }
  void leave() override {
    assert(held);
    held = false;
  }
  bool quiescent() const override { return held; }
  bool bootQuiescent() const override { return held; }
  uint64_t millis() const override { return 100; }
  bool candidateAllowed(const IdentityBinding &) const override { return true; }
  bool peerAllowed(const IdentityBinding &, const ProvisionPeer &) const override { return true; }
};
struct Raw : ExpandedContactFlash {
  std::array<uint8_t, ExpandedContacts::Bytes> bytes;
  unsigned feeds = 0;
  Raw() { bytes.fill(0xff); }
  void progress() override { ++feeds; }
  int read(unsigned off, void *out, size_t n) override {
    assert(off + n <= bytes.size());
    memcpy(out, bytes.data() + off, n);
    return 0;
  }
  int program(unsigned off, const void *in, size_t n) override {
    assert(off + n <= bytes.size());
    const auto *p = static_cast<const uint8_t *>(in);
    for (size_t i = 0; i < n; ++i) {
      assert((bytes[off + i] & p[i]) == p[i]);
      bytes[off + i] &= p[i];
    }
    return 0;
  }
  int eraseBank(unsigned bank) override {
    assert(bank < 4);
    memset(bytes.data() + 8192 * bank, 0xff, 8192);
    return 0;
  }
};
static PkiPeerPin makePin(unsigned i, uint8_t secret[32]) {
  uint8_t seed[4];
  put(seed, i);
  crypto_blake2b(secret, 32, seed, sizeof(seed));
  secret[0] &= 248;
  secret[31] = (secret[31] & 127) | 64;
  PkiPeerPin pin;
  crypto_x25519_public_key(pin.publicKey, secret);
  pin.node = bench::MtSignature::nodeId(pin.publicKey);
  return pin;
}
void runtimeTest() {
  Memory memory;
  FixtureSettings settings(memory);
  Context context;
  Entropy entropy;
  IdentityBinding local;
  PkiFreshProvisioning enroll(settings, context, entropy);
  assert(!enroll.stage(local) && !enroll.confirm(local));
  Raw raw;
  uint8_t secret[32];
  {
    ExpandedContacts wide(raw);
    PkiStorage s(settings, memory, context, entropy);
    s.attachExpanded(wide);
    assert(!s.prepare() && !s.activate(local.node) && !s.enableExpanded(local));
    for (unsigned i = 0; i < 198; ++i)
      assert(!s.putOwnerPeer(local, makePin(i, secret)));
  }
  ExpandedContacts wide(raw);
  PkiStorage s(settings, memory, context, entropy);
  s.attachExpanded(wide);
  assert(!s.prepare() && !s.activate(local.node));
  RuntimePinDirectory pins;
  assert(s.preparedRuntimePeers(pins) && pins.count() == 198);
  Backend backend;
  SignEntropy signer;
  mt::experimental::PkiCrypto crypto(backend);
  MultiPeerRuntime runtime(crypto, backend, *s.runtimeIdentity(), s, s, &signer);
  raw.feeds = 0;
  assert(runtime.open(local.node, pins, &raw) && raw.feeds == 198);
  mt::Endpoint ep;
  const auto plain = data("expanded high-index fixture");
  auto submit = [&](const PkiPeerPin &peer, uint32_t id, bool reliable) {
    return runtime.submit(ep, 0, peer.node, id, plain.data(), plain.size(), 0, reliable, peer.publicKey, 32,
                          true, 100);
  };
  for (unsigned i : {0u, 127u, 128u, 197u}) {
    const auto row = makePin(i, secret);
    assert(runtime.acceptsPeerKey(row.node, row.publicKey, 32) && !submit(row, 90000 + i, false));
    auto *job = ep.next(100);
    assert(job && job->frame.len > 16 && bench::le32(job->frame.bytes) == row.node);
    uint8_t decrypted[227];
    size_t n = 0;
    assert(crypto.decrypt(secret, local.publicKey, local.node, 90000 + i, job->frame.bytes + 16,
                          job->frame.len - 16, decrypted, sizeof(decrypted), n) &&
           n == plain.size() && !memcmp(decrypted, plain.data(), n));
    ep.started(*job, 0, 100, 100, 0);
    ep.complete(*job, 101, 0);
    runtime.sweep(ep);
  }
  const auto removed = makePin(128, secret);
  assert(runtime.revoke(removed.node));
  assert(submit(removed, 80001, false) < 0 && !s.active(removed.node, removed.publicKey));
  const auto last = makePin(197, secret);
  assert(!submit(last, 80002, true));
  const auto encrypted = backend.encrypts;
  assert(!submit(last, 80002, true) && backend.encrypts == encrypted);
  ep.cancelPrivate();
  runtime.sweep(ep);
  const auto replacement = makePin(198, secret);
  assert(!s.putOwnerPeer(local, replacement) && runtime.adoptPersistedPeer(replacement) &&
         !submit(replacement, 80003, false));
  assert(s.addTrustedPeer(local, makePin(200, secret)) == -ENOTSUP);
  puts("PASS 198 validated crypto sessions, index 127/128/197 plaintext independently decrypted, reliable "
       "retry, revoke and reuse");
}
struct CandidatePoll {
  mt::Service &service;
  PkiStartup &privateStartup;
  struct PrivateStore {
    PkiStorage &pki;
    PkiStorage &storage() { return pki; }
  } privateStorage;
  struct Owner {
    mt::Service &service;
    unsigned pauses = 0, resumes = 0, holds = 0;
    bool ready() const { return true; }
    bool peerAllowed(const IdentityBinding &, const PkiPeerPin &) const { return true; }
    bool pause() {
      ++pauses;
      return service.beginPrivateAdministration();
    }
    bool resume() {
      ++resumes;
      return service.endPrivateAdministration();
    }
    void hold() { ++holds; }
  } peerContext;
  bool initialized = true, storageReady = true;
  struct {
    bool ready() const { return true; }
  } journal;
  struct {
    bool open() const { return false; }
  } settingsEdit;
  struct {
    bool pending() const { return false; }
  } rebootSchedule;
  int rxQueue = 0;
  unsigned k_msgq_num_used_get(const int *) const { return 0; }
  CandidatePoll(mt::Service &svc, PkiStartup &startup, PkiStorage &store)
      : service(svc), privateStartup(startup), privateStorage{store}, peerContext{svc} {}
  void poll() {
#include "native-verified-candidate.inc"
  }
};
void signedDiscovery(bool expanded) {
  Memory memory;
  FixtureSettings settings(memory);
  Context ctx;
  Entropy entropy;
  IdentityBinding local;
  PkiFreshProvisioning enroll(settings, ctx, entropy);
  assert(!enroll.stage(local) && !enroll.confirm(local));
  Raw raw;
  {
    ExpandedContacts wide(raw);
    PkiStorage s(settings, memory, ctx, entropy);
    s.attachExpanded(wide);
    assert(!s.prepare() && !s.activate(local.node));
    if (expanded)
      assert(!s.enableExpanded(local));
  }
  Backend backend;
  mt::experimental::PkiCrypto crypto(backend);
  SignEntropy signer;
  Radio radio;
  mt::test::KnownChannelFixtureClock clock;
  mt::Service service(radio, true, local.node, 81, true, false);
  clock.bind(service);
  mt::Config cfg;
  cfg.bytes[4] = 1;
  auto node = cfg.node();
  node.setPublic();
  cfg.node(node);
  assert(service.apply(cfg) && service.requirePrivateRuntime());
  ExpandedContacts wide(raw);
  PkiStorage store(settings, memory, ctx, entropy);
  store.attachExpanded(wide);
  PkiStartup startup(store, crypto, backend, signer);
  uint32_t selected = 0;
  assert(startup.prepare(selected) && selected == local.node && startup.activate(service, true));
  assert(service.start(false, 0) && service.armEndpoint(true));
  CandidatePoll owner(service, startup, store);
  Key peer(131);
  Memory peerData;
  IdentityJournal peerIdentity;
  MonocypherIdentityDeriver derive;
  assert(!peerIdentity.load(peerData, derive));
  IdentityCandidate key(peer.binding, peer.scalar);
  assert(!peerIdentity.store(peerData, derive, key, true));
  KeyRef ref;
  IdentityBinding binding;
  assert(peerIdentity.binding(binding, ref));
  NodeInfoProfile profile;
  profile.hardwareModel = 81;
  strcpy(profile.longName, "Synthetic contact");
  strcpy(profile.shortName, "TEST");
  SigningEnvelope envelope;
  envelope.hasBitfield = true;
  SignedData signedData;
  assert(PkiSigner::bootstrap(peerIdentity, ref, binding, signer, 3004, local.node, profile, envelope,
                              signedData) == SigningResult::Ok);
  uint8_t frame[255];
  size_t n = 0;
  assert(service.codec.encodeSignedNodeInfo(binding.node, local.node, 3004, signedData.bytes, signedData.size,
                                            frame, sizeof(frame), n));
  assert(clock.set(1000));
  service.rx(frame, n, -42, 1000, 17);
  uint32_t candidate = 0;
  uint8_t pub[32];
  assert(service.peekVerifiedPeerCandidate(candidate, pub) && candidate == binding.node &&
         !memcmp(pub, binding.publicKey, 32));
  const auto nvsBefore = memory.writes;
  const auto mediaBefore = raw.bytes;
  const auto stops = radio.stops;
  owner.poll();
  assert(!service.peekVerifiedPeerCandidate(candidate, pub) && !service.faulted() &&
         !owner.peerContext.holds);
  assert(owner.peerContext.pauses == unsigned(!expanded) && owner.peerContext.resumes == unsigned(!expanded));
  if (expanded) {
    assert(nvsBefore == memory.writes && raw.bytes == mediaBefore && radio.stops == stops);
    assert(!store.active(binding.node, binding.publicKey) && service.privateDormant());
    PkiPeerPin pin;
    pin.node = binding.node;
    memcpy(pin.publicKey, binding.publicKey, 32);
    assert(service.beginPrivateAdministration() && !store.putOwnerPeer(local, pin) &&
           startup.adoptPersistedPeer(pin) && service.endPrivateAdministration());
  } else
    assert(memory.writes > nvsBefore);
  assert(store.active(binding.node, binding.publicKey) && !service.privateDormant());
  service.stop();
  ExpandedContacts reopened(raw);
  PkiStorage after(settings, memory, ctx, entropy);
  after.attachExpanded(reopened);
  assert(!after.prepare() && !after.activate(local.node) && after.active(binding.node, binding.publicKey));
  puts(
      expanded
          ? "PASS actual signed NodeInfo consumed with zero pauses/writes/faults, explicit 198-profile pin "
            "durable"
          : "PASS ordinary 16-pin signature-verified owner poll persists and publishes after its RF barrier");
}
struct UsbHost : Host {
  unsigned requests = 0;
  uint32_t epoch = 0;
  int freshProvision(const uint8_t *p, size_t n, uint32_t e, char *out, size_t cap) override {
    ++requests;
    epoch = e;
    assert(n == strlen("zt_pki_contacts_expand_v1 status") &&
           !memcmp(p, "zt_pki_contacts_expand_v1 status", n));
    return snprintf(out, cap, "pki_contacts_expand_v1 rc=0 capacity=198 count=197 expanded=1 gen=201");
  }
};
void usbTest() {
  Radio radio;
  mt::Service service(radio, true, 0, 81, true, false);
  mt::companion::CompanionWorkspace workspace;
  UsbHost host;
  service.route.configureIdentity(1234);
  for (unsigned owner : {0u, 1u, 2u, 3u}) {
    mt::Companion phone(service, host, 81, workspace, uint8_t(owner));
    auto sendFrame = [&](const meshtastic_ToRadio &msg, uint32_t epoch) {
      uint8_t raw[512];
      auto stream = pb_ostream_from_buffer(raw + 4, sizeof(raw) - 4);
      assert(pb_encode(&stream, meshtastic_ToRadio_fields, &msg));
      raw[0] = 0x94;
      raw[1] = 0xc3;
      raw[2] = uint8_t(stream.bytes_written >> 8);
      raw[3] = uint8_t(stream.bytes_written);
      for (unsigned i = 0; i < 4 + stream.bytes_written; ++i)
        phone.byte(raw[i], 100, epoch, mt::Companion::WorkspaceProof{true});
    };
    meshtastic_ToRadio msg{};
    msg.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    msg.want_config_id = 8;
    sendFrame(msg, 77);
    msg = {};
    msg.which_payload_variant = meshtastic_ToRadio_packet_tag;
    auto &p = msg.packet;
    p.to = 1234;
    p.id = 91;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    const char query[] = "zt_pki_contacts_expand_v1 status";
    p.decoded.payload.size = sizeof(query) - 1;
    memcpy(p.decoded.payload.bytes, query, sizeof(query) - 1);
    host.out.clear();
    unsigned before = host.requests;
    sendFrame(msg, 77);
    assert(host.requests == before + (owner == 0));
    if (owner == 0) {
      assert(host.epoch == 77 && !host.out.empty() && host.out.back().packet.decoded.request_id == 91);
      p.hop_limit = 1;
      sendFrame(msg, 77);
      assert(host.requests == before + 1);
    }
  }
  puts("PASS canonical USB epoch accepted and response correlated, routed and BLE/TCP/serial owner forms "
       "refused");
}
} // namespace
int main() {
  runtimeTest();
  usbTest();
  signedDiscovery(false);
  signedDiscovery(true);
}
