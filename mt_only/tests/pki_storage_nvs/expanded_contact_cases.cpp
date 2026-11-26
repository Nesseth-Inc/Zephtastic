// SPDX-License-Identifier: GPL-3.0-only
// Production PKI, actual Zephyr NVS against synthetic NOR, and a separate
// one-way 32 KiB flash model. Compiled with CONFIG_MT_XIAO_EXPANDED_CONTACTS.
#include "pki/NvsProvisioningSettings.h"
#include "pki/PkiFreshUsbIngress.h"
#include "pki/MultiPeerRuntime.h"
#include "pki/PkiStartup.h"
#include "bridge.h"
#include "monocypher.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace mt::pki;
namespace {
struct Gate : ProvisioningContext, PkiStorageContext {
  int depth = 0;
  bool tryEnter() override {
    ++depth;
    return true;
  }
  void leave() override {
    assert(depth);
    --depth;
  }
  bool quiescent() const override { return depth > 0; }
  bool bootQuiescent() const override { return quiescent(); }
  uint64_t millis() const override { return 123; }
  bool candidateAllowed(const IdentityBinding &) const override { return true; }
  bool peerAllowed(const IdentityBinding &, const ProvisionPeer &) const override { return true; }
};
struct Entropy : EntropySource {
  bool fill(uint8_t *p, size_t n) override {
    for (size_t i = 0; i < n; ++i)
      p[i] = uint8_t(17 + 21 * i);
    return true;
  }
};
struct Media : ExpandedContactFlash {
  std::array<uint8_t, ExpandedContacts::Bytes> bytes;
  int failAt = -1, events = 0, erases = 0, writes = 0;
  int phase = 0;
  Media() { bytes.fill(0xff); }
  int read(unsigned pos, void *p, size_t n) override {
    assert(pos + n <= bytes.size());
    memcpy(p, bytes.data() + pos, n);
    return 0;
  }
  int program(unsigned pos, const void *p, size_t n) override {
    assert(pos + n <= bytes.size() && !(pos % 4) && !(n % 4));
    auto in = static_cast<const uint8_t *>(p);
    ++events;
    if (events == failAt && phase == 0)
      return -EIO;
    const size_t written = events == failAt && phase == 1 ? n / 2 : n;
    for (size_t i = 0; i < written; ++i) {
      assert((bytes[pos + i] & in[i]) == in[i]);
      bytes[pos + i] &= in[i];
    }
    ++writes;
    return events == failAt ? -EIO : 0;
  }
  int eraseBank(unsigned bank) override {
    assert(bank < ExpandedContacts::Banks);
    ++events;
    if (events == failAt && phase == 0)
      return -EIO;
    const size_t erased = events == failAt && phase == 1 ? 4096 : ExpandedContacts::BankBytes;
    memset(bytes.data() + bank * ExpandedContacts::BankBytes, 0xff, erased);
    ++erases;
    return events == failAt ? -EIO : 0;
  }
};
PkiPeerPin pin(unsigned index) {
  PkiPeerPin p;
  uint8_t s[32];
  uint32_t i = index;
  crypto_blake2b(s, sizeof(s), reinterpret_cast<const uint8_t *>(&i), sizeof(i));
  crypto_x25519_public_key(p.publicKey, s);
  p.node = bench::MtSignature::nodeId(p.publicKey);
  return p;
}
Gate gate;
Entropy entropy;
NvsBootSettings boot;
mt::ZephyrSettings settings;
NvsProvisioningSettings setup;
IdentityBinding enroll() {
  PkiFreshProvisioning fresh(setup, gate, entropy);
  IdentityBinding b{};
  assert(!fresh.stage(b) && !fresh.confirm(b));
  return b;
}
struct Domain {
  ExpandedContacts expanded;
  PkiStorage pki;
  explicit Domain(Media &media) : expanded(media), pki(boot, settings, gate, entropy, nullptr) {
    pki.attachExpanded(expanded);
  }
  int bootDomain(const IdentityBinding &id) {
    const int rc = pki.prepare();
    return rc ? rc : pki.activate(id.node);
  }
};
void full(unsigned imported) {
  model_reset(8);
  auto id = enroll();
  Media media;
  auto first = pin(1);
  uint32_t through = 0, extra = 0;
  {
    Domain d(media);
    assert(!d.bootDomain(id));
    for (unsigned i = 1; i <= imported; ++i)
      assert(!d.pki.putOwnerPeer(id, pin(i)));
    if (imported == 16)
      assert(d.pki.putOwnerPeer(id, pin(17)) == -ENOSPC);
    IdentityBinding actual;
    KeyRef ref;
    assert(d.pki.runtimeIdentity()->binding(actual, ref));
    assert(d.pki.reserve(ref, id.publicKey, id.node, 501, extra));
    through = d.pki.nonceReservedThrough();
    if (imported) {
      assert(d.pki.commitAuthenticated(first.node, first.publicKey, 44));
    }
    media.bytes.back() = 0x82;
    const auto before = model_writes();
    assert(d.pki.enableExpanded(id) == -ENOTEMPTY);
    assert(model_writes() == before && !media.writes && !media.erases);
    media.bytes.back() = 0xff;
    auto wrong = id;
    wrong.node ^= 1;
    assert(d.pki.enableExpanded(wrong) == -EXDEV);
    assert(!d.pki.enableExpanded(id) && !media.erases && d.expanded.pins().count() == imported);
    assert(d.expanded.load(settings, id, 0) == -EACCES && d.expanded.ready());
    assert(d.pki.addTrustedPeer(id, pin(1000)) == -ENOTSUP);
    assert(!d.pki.active(pin(1000).node, pin(1000).publicKey));
    assert(d.pki.state() == PkiStorage::State::Active);
    if (imported)
      assert(d.pki.seen(first.node, first.publicKey, 44) == ReplayCheck::Seen);
  }
  model_remount();
  {
    PkiStorage old(boot, settings, gate, entropy, nullptr);
    assert(old.prepare() != 0 && !old.runtimeIdentity());
  }
  {
    Domain d(media);
    assert(!d.bootDomain(id));
    assert(d.expanded.pins().count() == imported);
    for (unsigned i = imported + 1; i <= 198; ++i)
      assert(!d.pki.putOwnerPeer(id, pin(i)));
    const auto atLimit = model_writes();
    const int mediaEvents = media.events;
    assert(d.pki.putOwnerPeer(id, pin(199)) == -ENOSPC && atLimit == model_writes() &&
           media.events == mediaEvents);
    auto duplicate = pin(2);
    duplicate.node = pin(3).node;
    assert(d.pki.putOwnerPeer(id, duplicate) == -EEXIST);
    RuntimePinDirectory all;
    assert(d.pki.preparedRuntimePeers(all) && all.count() == 198);
    const auto last = pin(198);
    assert(d.pki.active(last.node, last.publicKey) &&
           d.pki.commitAuthenticated(last.node, last.publicKey, 77));
    assert(!d.pki.commitAuthenticated(last.node, last.publicKey, 77));
    assert(!d.pki.revokeTrustedPeer(last.node, first.publicKey));
    assert(d.pki.revokeTrustedPeer(last.node, last.publicKey));
    assert(!d.pki.active(last.node, last.publicKey) &&
           !d.pki.commitAuthenticated(last.node, last.publicKey, 77));
    assert(!d.pki.putOwnerPeer(id, pin(199)));
  }
  model_remount();
  {
    Domain d(media);
    assert(!d.bootDomain(id));
    assert(d.expanded.pins().count() == 198);
    auto last = pin(198), replacement = pin(199);
    assert(!d.pki.active(last.node, last.publicKey) && d.pki.active(replacement.node, replacement.publicKey));
    IdentityBinding actual;
    KeyRef ref;
    assert(d.pki.runtimeIdentity()->binding(actual, ref));
    assert(d.pki.reserve(ref, id.publicKey, id.node, 501, extra) && extra > through);
    auto alternate = pin(204);
    alternate.node = first.node;
    assert(!d.pki.putOwnerPeer(id, alternate));
    assert(!d.pki.active(first.node, first.publicKey) && d.pki.active(alternate.node, alternate.publicKey));
    unsigned retired = 0;
    while (d.expanded.pins().count()) {
      const auto row = *d.expanded.pins().at(0);
      assert(d.pki.revokeTrustedPeer(row.node, row.publicKey));
      ++retired;
    }
    assert(retired == 198);
  }
  model_remount();
  Domain empty(media);
  assert(!empty.bootDomain(id) && empty.expanded.pins().count() == 0);
  assert(empty.expanded.retireDomain(settings, pin(201).publicKey) == -EXDEV);
  assert(!empty.expanded.retireDomain(settings, id.publicKey));
  for (auto b : media.bytes)
    assert(b == 0xff);
  printf("full import=%u 198 pins, 199 refused, duplicate/rekey/revoke, monotonic nonce, retirement and "
         "scoped erase verified\n",
         imported);
}
} // namespace
namespace {
void powerCuts(bool initial) {
  model_reset(8);
  const auto id = enroll();
  Media source;
  const auto a = pin(1), b = pin(2);
  {
    Domain d(source);
    assert(!d.bootDomain(id));
    assert(!d.pki.putOwnerPeer(id, a));
    if (!initial) {
      assert(!d.pki.enableExpanded(id));
      for (unsigned n = 3; n <= 7; ++n)
        assert(!d.pki.putOwnerPeer(id, pin(n)));
    }
  }
  model_snapshot();
  auto operation = [&](Domain &d) { return initial ? d.pki.enableExpanded(id) : d.pki.putOwnerPeer(id, b); };
  unsigned rawEvents = 0, nvsEvents = 0, total = 0;
  {
    Media m = source;
    Domain d(m);
    assert(!d.bootDomain(id));
    model_fail(0, 0);
    assert(!operation(d));
    rawEvents = unsigned(m.events - source.events);
    nvsEvents = model_events();
    assert(rawEvents > 3 && nvsEvents > 0);
    // A restored old NVS image must not overrule acknowledged raw authority.
    model_restore();
    Domain replay(m);
    assert(replay.bootDomain(id) != 0 && !replay.pki.runtimeIdentity());
  }
  for (bool raw : {true, false})
    for (unsigned event = 1; event <= (raw ? rawEvents : nvsEvents); ++event) {
      for (int phase = 0; phase < 3; ++phase) {
        model_restore();
        Media m = source;
        {
          Domain d(m);
          assert(!d.bootDomain(id));
          if (raw) {
            m.failAt = m.events + int(event);
            m.phase = phase;
          } else
            model_fail(event, phase);
          const int rc = operation(d);
          if (rc == 0)
            assert(d.expanded.ready() && (initial || d.pki.active(b.node, b.publicKey)));
        }
        model_no_fault();
        model_remount();
        m.failAt = -1;
        Domain r(m);
        const int rc = r.bootDomain(id);
        if (rc)
          assert(!r.pki.runtimeIdentity()); // A malformed settings selector always holds.
        else {
          assert(r.pki.active(a.node, a.publicKey));
          if (initial) {
            assert(!r.pki.active(b.node, b.publicKey));
            if (!r.expanded.ready())
              assert(!operation(r));
            assert(r.expanded.pins().count() == 1);
          } else {
            assert(r.expanded.ready());
            for (unsigned n = 3; n <= 7; ++n) {
              const auto p = pin(n);
              assert(r.pki.active(p.node, p.publicKey));
            }
            if (!r.pki.active(b.node, b.publicKey))
              assert(!operation(r));
            assert(r.expanded.pins().count() == 7);
          }
        }
        ++total;
      }
    }
  printf("cut matrix %s: %u NOR and %u NVS operations, %u before/partial/after trials; rollback witness\n",
         initial ? "migration" : "update", rawEvents, nvsEvents, total);
}
void retirementCuts() {
  model_reset(8);
  const auto id = enroll();
  Media media;
  {
    Domain d(media);
    assert(!d.bootDomain(id) && !d.pki.enableExpanded(id));
    for (unsigned i = 1; i <= 5; ++i)
      assert(!d.pki.putOwnerPeer(id, pin(i)));
  }
  const auto original = media;
  for (unsigned bank = 0; bank < 4; ++bank)
    for (int phase = 0; phase < 3; ++phase) {
      Media test = original;
      Domain d(test);
      test.failAt = test.events + 1 + int(bank);
      test.phase = phase;
      assert(d.expanded.retireDomain(settings, id.publicKey) == -EIO);
      test.failAt = -1;
      Domain retry(test);
      assert(!retry.expanded.retireDomain(settings, id.publicKey));
      for (uint8_t val : test.bytes)
        assert(val == 0xff);
    }
  Media foreign = original;
  const unsigned base = 3 * ExpandedContacts::BankBytes;
  memset(foreign.bytes.data() + base, 0xff, ExpandedContacts::BankBytes);
  foreign.bytes.back() = 0x7e;
  auto unchanged = foreign.bytes;
  Domain d(foreign);
  assert(d.expanded.retireDomain(settings, id.publicKey) == -ENOTEMPTY && foreign.bytes == unchanged);
  // A structurally valid bank from another owner must hold before any erase.
  foreign = original;
  auto *hdr = foreign.bytes.data() + base;
  hdr[12] ^= 0x04;
  PkiFreshRoot::put(hdr + 60, PkiFreshRoot::checksum(hdr, 60));
  unchanged = foreign.bytes;
  assert(d.expanded.retireDomain(settings, id.publicKey) == -EXDEV && foreign.bytes == unchanged);
  puts("12 sectorwise reset cuts and foreign/malformed bank with zero erases");
}
} // namespace
int main() {
  full(0);
  full(16);
  powerCuts(true);
  powerCuts(false);
  retirementCuts();
}
