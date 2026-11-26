// SPDX-License-Identifier: GPL-3.0-only
// M148: actual pinned Zephyr NVS/settings code over synthetic NOR only.
#define main m69_existing_cases_main
#include "cases.cpp"
#undef main

#include "pki/PkiNonceCounter.h"

namespace {
using namespace mt::pki;

class SettingsCounterStore final : public PkiNonceCounterStore {
  mt::ZephyrSettings &settings_;
  static const char *name(unsigned slot) {
    return slot == 0 ? "mtpki/nonces0" : slot == 1 ? "mtpki/nonces1" : nullptr;
  }
public:
  explicit SettingsCounterStore(mt::ZephyrSettings &s) : settings_(s) {}
  int read(unsigned slot, uint8_t *out, size_t size) override {
    const char *key = name(slot);
    return key ? settings_.read(key, out, size) : -EINVAL;
  }
  int write(unsigned slot, const uint8_t *data, size_t size) override {
    const char *key = name(slot);
    return key ? settings_.write(key, data, size) : -EINVAL;
  }
};

static PkiNonceCounterBinding counterBinding(const Fixture &f) {
  PkiNonceCounterBinding b{};
  b.node = f.local.binding.node;
  memcpy(b.publicKey, f.local.binding.publicKey, sizeof(b.publicKey));
  b.legacyGeneration = 1;
  b.legacyCrc = 0x5a31c7e9u; // Synthetic anchor for the standalone allocator fixture.
  return b;
}

static void provisionAllLegacySlots(Fixture &f) {
  f.provision(); // Populates identity/state0 and ledger0/ledger1.
  IdentityJournal identity;
  assert(identity.load(f.settings, f.deriver) == 0);
  IdentityBinding next = f.local.binding;
  next.generation = 2;
  IdentityCandidate second(next, f.local.scalar);
  assert(identity.store(f.settings, f.deriver, second, true) == 0);
}

static void assertSixDiscoveredAndCapture(uint8_t out[6][PkiLedger::RecordSize],
                                          size_t sizes[6]) {
  static const char *names[6] = {
    "mtidentity/state0", "mtidentity/state1", "mtpki/ledger0",
    "mtpki/ledger1", "mtpki/nonces0", "mtpki/nonces1"
  };
  static const size_t lengths[6] = {
    IdentityJournal::RecordSize, IdentityJournal::RecordSize,
    PkiLedger::RecordSize, PkiLedger::RecordSize,
    PkiNonceCounter::RecordBytes, PkiNonceCounter::RecordBytes
  };
  NvsBootSettings boot;
  assert(boot.begin() == 0);
  // Other valid settings (the GC workload adds one unrelated name) are allowed;
  // every one of the six supported identity/ledger/counter names must resolve.
  assert(boot.diagnostic().names >= 6);
  for (unsigned i = 0; i < 6; ++i) {
    sizes[i] = lengths[i];
    assert(boot.read(names[i], out[i], lengths[i]) == int(lengths[i]));
    uint8_t wrong[ PkiNonceCounter::RecordBytes ]{};
    if (i >= 4) assert(boot.read(names[i], wrong, sizeof(wrong) - 1) == -EACCES);
  }
  boot.end();
}

static void assertLegacyStateLoads(Fixture &f) {
  NvsBootSettings boot;
  assert(boot.begin() == 0);
  IdentityJournal identity;
  PkiLedger ledger;
  assert(identity.load(boot, f.deriver) == 0);
  assert(ledger.load(boot) == 0);
  IdentityBinding current; KeyRef ref;
  assert(identity.binding(current, ref));
  assert(current.node == f.local.binding.node && current.generation == 2);
  assert(!memcmp(current.publicKey, f.local.binding.publicKey, 32));
  uint8_t peer[32]{};
  assert(ledger.lookup(f.peer.binding.node, peer));
  assert(!memcmp(peer, f.peer.binding.publicKey, 32));
  eraseIdentityBytes(peer, sizeof(peer));
  boot.end();
}

static void initializeAndFillBlock(SettingsCounterStore &store,
                                   const PkiNonceCounterBinding &binding) {
  PkiNonceCounter counter(store);
  assert(counter.initialize(binding, true) == 0);
  assert(counter.open(binding, true) == 0);
  uint32_t value = 0;
  for (uint32_t expected = 1; expected <= 256; ++expected) {
    assert(counter.reserve(value) == 0 && value == expected);
    if (expected == 1) assert(counter.reservedThrough() == 256);
  }
  counter.close();
}

static void assertNvsGcPreservesRows(Fixture &f,
                                     const PkiNonceCounterBinding &binding) {
  uint8_t before[6][PkiLedger::RecordSize]{};
  uint8_t after[6][PkiLedger::RecordSize]{};
  size_t beforeSizes[6]{}, afterSizes[6]{};
  assertSixDiscoveredAndCapture(before, beforeSizes);

  uint8_t noise[128];
  for (size_t i = 0; i < sizeof(noise); ++i) noise[i] = uint8_t(17 * i + 9);
  std::array<uint8_t, 32768> flashBefore{}, flashAfter{};
  model_copy_flash(flashBefore.data());
  const unsigned firstWrites = model_writes();
  const unsigned long count = 1000;
  for (unsigned long i = 0; i < count; ++i) {
    noise[0] = uint8_t(i);
    assert(f.settings.write("m148/noise", noise, sizeof(noise)) == 0);
  }
  assert(model_writes() > firstWrites);
  model_copy_flash(flashAfter.data());
  bool observedErase = false;
  for (size_t i = 0; i < flashBefore.size(); ++i)
    if (uint8_t(~flashBefore[i]) & flashAfter[i]) observedErase = true;
  assert(observedErase); // Prove the pinned NVS performed at least one sector GC.
  model_remount();
  assertSixDiscoveredAndCapture(after, afterSizes);
  for (unsigned i = 0; i < 6; ++i) {
    assert(beforeSizes[i] == afterSizes[i]);
    assert(!memcmp(before[i], after[i], beforeSizes[i]));
  }
  assertLegacyStateLoads(f);
  SettingsCounterStore remounted(f.settings);
  PkiNonceCounter counter(remounted);
  assert(counter.open(binding, true) == 0);
  uint32_t value = 0;
  assert(counter.reserve(value) == 0 && value == 257);
  counter.close();
  printf("PASS NVS GC retained 6 exact identity/ledger/counter rows across %lu unrelated updates; counter continued at 257\n", count);
}

static void assertCounterWriteCuts(SettingsCounterStore &store,
                                   const PkiNonceCounterBinding &binding) {
  uint8_t base[6][PkiLedger::RecordSize]{};
  size_t sizes[6]{};
  assertSixDiscoveredAndCapture(base, sizes);
  model_snapshot();

  // Determine every raw flash write in the next high-water settings update.
  model_remount();
  PkiNonceCounter clean(store);
  assert(clean.open(binding, true) == 0);
  model_no_fault();
  uint32_t exposed = 0;
  const unsigned eventsBefore = model_events();
  assert(clean.reserve(exposed) == 0 && exposed == 257);
  const unsigned writeEvents = model_events() - eventsBefore;
  assert(writeEvents > 0);
  clean.close();

  unsigned trials = 0, held = 0, continued = 0;
  for (unsigned event = 1; event <= writeEvents; ++event) {
    for (int phase = 0; phase <= 2; ++phase) {
      model_restore();
      PkiNonceCounter attempt(store);
      const int opened = attempt.open(binding, true);
      if (opened) { ++held; ++trials; continue; }
      model_fail(event, phase);
      uint32_t maybeExposed = 0;
      const int rc = attempt.reserve(maybeExposed);
      if (rc != 0) assert(maybeExposed == 0);
      model_no_fault();
      attempt.close();

      model_remount();
      PkiNonceCounter after(store);
      const int reopen = after.open(binding, true);
      if (reopen) {
        ++held;
      } else {
        uint32_t next = 0;
        assert(after.reserve(next) == 0);
        // Values 1..256 were exposed before the baseline snapshot. A faulted
        // call can expose zero or 257, depending on whether NVS reported the
        // fully committed readback as successful.
        const uint32_t maximumExposed = rc == 0 ? 257u : 256u;
        assert(next > maximumExposed);
        ++continued;
      }
      after.close();
      ++trials;
    }
  }
  assert(trials == writeEvents * 3 && held + continued == trials);
  printf("PASS counter NVS raw-write cuts: %u before/partial/after trials; %u safely held, %u resumed strictly above exposed values\n",
         trials, held, continued);
}

static void counterCases(unsigned sectors) {
  model_reset(sectors);
  Fixture f;
  provisionAllLegacySlots(f);
  const auto binding = counterBinding(f);
  SettingsCounterStore store(f.settings);
  initializeAndFillBlock(store, binding);

  // A normal reboot burns all unused values in the persisted 1..256 block.
  model_remount();
  PkiNonceCounter first(store);
  assert(first.open(binding, true) == 0);
  uint32_t value = 0;
  assert(first.reserve(value) == 0 && value == 257);
  first.close();
  model_remount();
  PkiNonceCounter second(store);
  assert(second.open(binding, true) == 0);
  assert(second.reserve(value) == 0 && value == 513);
  second.close();
  assertLegacyStateLoads(f);
  printf("PASS %u-sector two-slot counter initialization and reboot burns 256-value reservations\n", sectors);

  // Reset to exactly the durable end-of-block state for independent fault trials.
  model_reset(sectors);
  Fixture faultFixture;
  provisionAllLegacySlots(faultFixture);
  const auto faultBinding = counterBinding(faultFixture);
  SettingsCounterStore faultStore(faultFixture.settings);
  initializeAndFillBlock(faultStore, faultBinding);
  assertCounterWriteCuts(faultStore, faultBinding);

  model_reset(sectors);
  Fixture gcFixture;
  provisionAllLegacySlots(gcFixture);
  const auto gcBinding = counterBinding(gcFixture);
  SettingsCounterStore gcStore(gcFixture.settings);
  initializeAndFillBlock(gcStore, gcBinding);
  assertNvsGcPreservesRows(gcFixture, gcBinding);
}
} // namespace

int main() {
  counterCases(4);
  counterCases(8);
  return 0;
}
