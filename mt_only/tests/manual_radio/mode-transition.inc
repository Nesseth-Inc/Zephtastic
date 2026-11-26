// SPDX-License-Identifier: GPL-3.0-only
// Root-adapted Qwen proposal; independently executed by the host suite.
void modeTransitionAssertions() {
  Config c; // Valid default-public identity, preset 0, empty stored primary
            // name, default name LongFast.

  // Valid configuration: the unchanged current request round-trips.
  assert(c.setRadio(c.radioRequest()));

  // Empty stored primary name; effective name derived from mode:
  // preset 0 is LongFast.
  assert(c.primaryName()[0] == '\0');
  assert(strcmp(c.effectivePrimaryName(), "LongFast") == 0);

  // Empty-name inherited secondary: role 2, no stored name, no key bytes.
  ChannelRecord secondary;
  assert(secondary.set(2, "", nullptr, 0));
  assert(c.setSecondaryChannel(1, secondary));

  PrimaryChannel::Selection primary;
  PrimaryChannel::Selection secondarySel;

  // Step 1: preset 0. Both selections are public-default because the
  // secondary inherits the primary's key and effective name.
  assert(c.channelSelection(0, primary));
  assert(c.channelSelection(1, secondarySel));
  assert(c.primaryName()[0] == '\0');      // Stored primary name stays empty.
  ChannelRecord stored; assert(c.channelRecord(1, stored) && stored.name()[0] == '\0'); // Stored secondary name empty.
  assert(primary.length == sizeof(bench::MtNodeRecord::PublicKey));
  assert(secondarySel.length == primary.length);
  assert(memcmp(primary.bytes, bench::MtNodeRecord::PublicKey, primary.length) == 0);
  assert(memcmp(primary.bytes, secondarySel.bytes, primary.length) == 0);
  assert(primary.hash == 8);            // LongFast alias hash.
  assert(secondarySel.hash == primary.hash);
  PrimaryChannel::clear(primary);
  PrimaryChannel::clear(secondarySel);

  // Step 2: preset -> manual. setRadio preserves the empty stored name;
  // effective name derived from mode: manual is Custom.
  RadioSettings::Request req = c.radioRequest();
  req.usePreset = false;
  assert(c.setRadio(req));
  assert(c.primaryName()[0] == '\0');
  assert(strcmp(c.effectivePrimaryName(), "Custom") == 0);

  assert(c.channelSelection(0, primary));
  assert(c.channelSelection(1, secondarySel));
  assert(c.primaryName()[0] == '\0');
  assert(c.channelRecord(1, stored) && stored.name()[0] == '\0');
  assert(primary.length == sizeof(bench::MtNodeRecord::PublicKey));
  assert(secondarySel.length == primary.length);
  assert(memcmp(primary.bytes, bench::MtNodeRecord::PublicKey, primary.length) == 0);
  assert(memcmp(primary.bytes, secondarySel.bytes, primary.length) == 0);
  assert(primary.hash == 49);           // Custom alias hash.
  assert(secondarySel.hash == primary.hash);
  PrimaryChannel::clear(primary);
  PrimaryChannel::clear(secondarySel);

  // Step 3: manual -> preset. Back to preset 0, i.e. LongFast.
  req = c.radioRequest();
  req.usePreset = true;
  assert(c.setRadio(req));
  assert(c.primaryName()[0] == '\0');
  assert(strcmp(c.effectivePrimaryName(), "LongFast") == 0);

  assert(c.channelSelection(0, primary));
  assert(c.channelSelection(1, secondarySel));
  assert(c.primaryName()[0] == '\0');
  assert(c.channelRecord(1, stored) && stored.name()[0] == '\0');
  assert(primary.length == sizeof(bench::MtNodeRecord::PublicKey));
  assert(secondarySel.length == primary.length);
  assert(memcmp(primary.bytes, bench::MtNodeRecord::PublicKey, primary.length) == 0);
  assert(memcmp(primary.bytes, secondarySel.bytes, primary.length) == 0);
  assert(primary.hash == 8);
  assert(secondarySel.hash == primary.hash);
  PrimaryChannel::clear(primary);
  PrimaryChannel::clear(secondarySel);

  // Still valid after the full transition cycle.
  assert(c.setRadio(c.radioRequest()));
}
