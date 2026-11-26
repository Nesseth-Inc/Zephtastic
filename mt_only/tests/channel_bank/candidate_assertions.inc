// SPDX-License-Identifier: GPL-3.0-only
// Qwen proposal independently reviewed; execution recorded by root separately.
void test_channel_candidates_mask() {
  constexpr uint8_t DISABLED = 0, PRIMARY = 1, SECONDARY = 2;

  // 1. Edge slots 0 and 7 eligible, matching hash.
  {
    uint8_t roles[8]  = {PRIMARY, DISABLED, DISABLED, DISABLED,
                         DISABLED, DISABLED, DISABLED, SECONDARY};
    uint8_t hashes[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    assert(mt::ChannelCandidates::mask(roles, hashes, 9) == 0x81); // bits 0,7
    assert(mt::ChannelCandidates::mask(roles, hashes, 0) == 0x00);
  }
  // 2. Multiple matching slots, colliding hashes: all eligible bits returned.
  {
    uint8_t roles[8]  = {PRIMARY, SECONDARY, 3, DISABLED,
                         SECONDARY, PRIMARY, DISABLED, PRIMARY};
    uint8_t hashes[8] = {5, 5, 5, 5, 5, 5, 5, 5};
    assert(mt::ChannelCandidates::mask(roles, hashes, 5) == 0xB3); // bits 0,1,4,5,7
  }
  // 3. All disabled: 0 even with matching hashes.
  {
    uint8_t roles[8]  = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t hashes[8] = {7, 7, 7, 7, 7, 7, 7, 7};
    assert(mt::ChannelCandidates::mask(roles, hashes, 7) == 0x00);
  }
  // 4. Hash 0 is a valid value: matches eligible entries when wanted == 0.
  {
    uint8_t roles[8]  = {PRIMARY, DISABLED, SECONDARY, 0, 0, 0, 0, 0};
    uint8_t hashes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    assert(mt::ChannelCandidates::mask(roles, hashes, 0) == 0x05); // bits 0,2
  }
  // 5. Invalid role bytes (3, 128, 255) ignored even with matching hash.
  {
    uint8_t roles[8]  = {3, 255, PRIMARY, SECONDARY,
                         DISABLED, 128, SECONDARY, PRIMARY};
    uint8_t hashes[8] = {4, 4, 4, 4, 4, 4, 4, 4};
    assert(mt::ChannelCandidates::mask(roles, hashes, 4) == 0xCC); // bits 2,3,6,7
  }
  // 6. Null roles or null hashes: 0.
  {
    uint8_t roles[8]  = {PRIMARY, 0, 0, 0, 0, 0, 0, SECONDARY};
    uint8_t hashes[8] = {3, 3, 3, 3, 3, 3, 3, 3};
    assert(mt::ChannelCandidates::mask(nullptr, hashes, 3) == 0x00);
    assert(mt::ChannelCandidates::mask(roles, nullptr, 3) == 0x00);
  }
}
