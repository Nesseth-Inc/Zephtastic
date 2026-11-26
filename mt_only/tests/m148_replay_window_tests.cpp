// SPDX-License-Identifier: GPL-3.0-only
#include "pki/PkiReplayWindow.h"
#include "monocypher.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
using mt::pki::PkiReplayWindow;

struct PublicPeer {
  uint8_t scalar[32]{};
  uint8_t publicKey[32]{};
  uint32_t node = 0;

  explicit PublicPeer(unsigned seed) {
    for (size_t i = 0; i < sizeof(scalar); ++i)
      scalar[i] = uint8_t(seed + unsigned(i * 31));
    scalar[0] &= 248;
    scalar[31] = uint8_t((scalar[31] & 127) | 64);
    crypto_x25519_public_key(publicKey, scalar);
    node = bench::MtSignature::nodeId(publicKey);
    assert(node >= 2 && node <= 0xfffffffdu);
  }
  ~PublicPeer() { crypto_wipe(scalar, sizeof(scalar)); }
};

void bindPeer(PkiReplayWindow &window, size_t index, const PublicPeer &peer) {
  assert(window.bind(index, peer.node, peer.publicKey, 0x12345678u));
}

void identityAndInvalidInputs() {
  PkiReplayWindow window;
  PublicPeer a(11), b(19);
  uint8_t zero[32]{};
  assert(!window.bind(PkiReplayWindow::PeerLimit, a.node, a.publicKey, 77));
  assert(!window.bind(0, a.node, nullptr, 77));
  assert(!window.bind(0, a.node, a.publicKey, a.node));
  assert(!window.bind(0, 0, a.publicKey, 77));
  assert(!window.bind(0, 1, a.publicKey, 77));
  assert(!window.bind(0, 0xfffffffeu, a.publicKey, 77));
  assert(!window.bind(0, 0xffffffffu, a.publicKey, 77));
  assert(!window.bind(0, a.node + 1, a.publicKey, 77));
  assert(!window.bind(0, a.node, zero, 77));
  assert(window.contains(0, a.node, a.publicKey, 1) == PkiReplayWindow::Result::Unknown);

  bindPeer(window, 0, a);
  assert(window.contains(0, a.node, a.publicKey, 0) == PkiReplayWindow::Result::Unknown);
  assert(window.commitAuthenticated(0, a.node, a.publicKey, 0) == PkiReplayWindow::Result::Unknown);
  assert(window.contains(0, b.node, b.publicKey, 1) == PkiReplayWindow::Result::Unknown);
  assert(window.contains(0, a.node, b.publicKey, 1) == PkiReplayWindow::Result::Unknown);
  assert(window.contains(1, a.node, a.publicKey, 1) == PkiReplayWindow::Result::Unknown);
  assert(!window.bind(0, b.node, b.publicKey, 0x12345678u)); // No silent replacement.
  assert(window.bind(0, a.node, a.publicKey, 0x12345678u)); // Exact bind preserves state.
  assert(window.commitAuthenticated(0, a.node, a.publicKey, 1) == PkiReplayWindow::Result::Fresh);
  assert(window.bind(0, a.node, a.publicKey, 0x12345678u));
  assert(window.contains(0, a.node, a.publicKey, 1) == PkiReplayWindow::Result::Seen);
  assert(!window.bind(1, a.node, a.publicKey, 0x12345678u)); // No duplicate identity slots.
}

void boundedOldestEvictionAndDuplicateAge() {
  PkiReplayWindow window;
  PublicPeer a(23);
  bindPeer(window, 0, a);
  for (uint32_t id = 1; id <= PkiReplayWindow::IdLimit; ++id)
    assert(window.commitAuthenticated(0, a.node, a.publicKey, id) == PkiReplayWindow::Result::Fresh);
  assert(window.contains(0, a.node, a.publicKey, 1) == PkiReplayWindow::Result::Seen);
  assert(window.commitAuthenticated(0, a.node, a.publicKey, 1) == PkiReplayWindow::Result::Seen);
  assert(window.commitAuthenticated(0, a.node, a.publicKey, 65) == PkiReplayWindow::Result::Fresh);
  assert(window.contains(0, a.node, a.publicKey, 1) == PkiReplayWindow::Result::Fresh);
  assert(window.contains(0, a.node, a.publicKey, 2) == PkiReplayWindow::Result::Seen);
  assert(window.contains(0, a.node, a.publicKey, 65) == PkiReplayWindow::Result::Seen);
  assert(window.commitAuthenticated(0, a.node, a.publicKey, 66) == PkiReplayWindow::Result::Fresh);
  assert(window.contains(0, a.node, a.publicKey, 2) == PkiReplayWindow::Result::Fresh);
  assert(window.contains(0, a.node, a.publicKey, 3) == PkiReplayWindow::Result::Seen);
}

void peerIsolationUnbindAndClear() {
  PkiReplayWindow window;
  PublicPeer a(31), b(41), c(53), d(67), replacement(79);
  const PublicPeer *peers[] = {&a, &b, &c, &d};
  for (size_t i = 0; i < PkiReplayWindow::PeerLimit; ++i) {
    bindPeer(window, i, *peers[i]);
    assert(window.commitAuthenticated(i, peers[i]->node, peers[i]->publicKey, 77) == PkiReplayWindow::Result::Fresh);
  }
  for (size_t i = 0; i < PkiReplayWindow::PeerLimit; ++i) {
    assert(window.contains(i, peers[i]->node, peers[i]->publicKey, 77) == PkiReplayWindow::Result::Seen);
    assert(window.contains(i, peers[i]->node, peers[i]->publicKey, 78) == PkiReplayWindow::Result::Fresh);
  }
  assert(window.unbind(1));
  assert(!window.unbind(1));
  assert(window.contains(1, b.node, b.publicKey, 77) == PkiReplayWindow::Result::Unknown);
  bindPeer(window, 1, replacement);
  assert(window.contains(1, replacement.node, replacement.publicKey, 77) == PkiReplayWindow::Result::Fresh);
  assert(window.contains(0, a.node, a.publicKey, 77) == PkiReplayWindow::Result::Seen);

  window.clear();
  const auto *raw = reinterpret_cast<const uint8_t *>(&window);
  for (size_t i = 0; i < sizeof(window); ++i) assert(raw[i] == 0);
  assert(window.contains(0, a.node, a.publicKey, 77) == PkiReplayWindow::Result::Unknown);
  bindPeer(window, 0, a);
  assert(window.contains(0, a.node, a.publicKey, 77) == PkiReplayWindow::Result::Fresh);
}
}  // namespace

int main() {
  static_assert(PkiReplayWindow::PeerLimit == 4 && PkiReplayWindow::IdLimit == 64,
                "Stock bounded RAM replay policy is fixed at four peers by 64 IDs");
  static_assert(sizeof(PkiReplayWindow) <= 1280, "Replay window RAM bound");
  identityAndInvalidInputs();
  boundedOldestEvictionAndDuplicateAge();
  peerIsolationUnbindAndClear();
  std::printf("PkiReplayWindow sizeof=%zu bytes\n", sizeof(PkiReplayWindow));
  std::puts("PASS exact peer binding, invalid inputs, duplicate age, bounded eviction, peer isolation, unbind and volatile clear");
}
