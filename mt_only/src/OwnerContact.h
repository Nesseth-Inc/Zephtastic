// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "NodeUser.h"
#include "meshtastic/admin.pb.h"
#include "pki/PkiPeerSet.h"
#include <pb_encode.h>

namespace mt {

// A validated, bounded projection of one already-decoded local SharedContact.
// This is only a request value: it does not mutate NodeCache, storage, PKI
// membership, or any RF/receive trust state.
struct OwnerContactCandidate {
  NodeCache::Entry node{};
  meshtastic_User user{};
  pki::PkiPeerPin peer{};
  bool shouldIgnore = false;
  // Preserve the stock wire intent for the owner-loop caller. This is not
  // evidence of a signature, key possession, or RF NodeInfo verification.
  bool manualVerificationRequested = false;
  bool hasPublicKey = false;
  bool pkiKeyAdmissible = false;
  // Set only for a usable, nonignored full public key. No CRC/node-ID binding
  // is required for an explicitly owner-configured contact key.
  bool hasPeerPin = false;
};

class OwnerContact {
  struct WipeBytes {
    uint8_t *bytes;
    size_t size;
    ~WipeBytes() {
      volatile uint8_t *p = bytes;
      while (size--) *p++ = 0;
    }
  };

  static bool terminated(const char *text, size_t bound) {
    return memchr(text, 0, bound) != nullptr;
  }

  static bool admissiblePkiKey(const uint8_t key[32]) {
    uint8_t edwards[32]{};
    WipeBytes wipe{edwards, sizeof(edwards)};
    return bench::MtSignature::admissiblePublicKey(key, edwards);
  }

 public:
  // Convert a typed SharedContact without changing output on any refusal.
  // localNode is the already-established local node number. NodeUser::decode
  // remains the single authority for cache name, enum, key-copy, and User
  // field bounds; the small temporary User encoding avoids duplicating those
  // rules here.
  static int decode(const meshtastic_SharedContact &contact, uint32_t localNode,
                    OwnerContactCandidate &output) {
    if (localNode <= 1 || localNode >= 0xfffffffeu || !contact.has_user ||
        contact.node_num <= 1 || contact.node_num >= 0xfffffffeu ||
        contact.node_num == localNode ||
        !terminated(contact.user.id, sizeof(contact.user.id)) ||
        !terminated(contact.user.long_name, sizeof(contact.user.long_name)) ||
        !terminated(contact.user.short_name, sizeof(contact.user.short_name)) ||
        contact.user.public_key.size > sizeof(contact.user.public_key.bytes) ||
        (contact.user.public_key.size != 0 && contact.user.public_key.size != 32)) {
      return -EINVAL;
    }

    uint8_t wire[meshtastic_User_size]{};
    WipeBytes wipe{wire, sizeof(wire)};
    auto stream = pb_ostream_from_buffer(wire, sizeof(wire));
    if (!pb_encode(&stream, meshtastic_User_fields, &contact.user)) return -EINVAL;

    OwnerContactCandidate staged{};
    if (!NodeUser::decode(staged.node, contact.node_num, wire, stream.bytes_written) ||
        !NodeCache::Entry::valid(staged.node.bytes, localNode)) {
      return -EINVAL;
    }

    staged.user = contact.user;
    staged.shouldIgnore = contact.should_ignore;
    staged.manualVerificationRequested = contact.manually_verified;
    staged.hasPublicKey = contact.user.public_key.size == 32;
    if (staged.hasPublicKey) {
      staged.pkiKeyAdmissible = admissiblePkiKey(contact.user.public_key.bytes);
      if (staged.pkiKeyAdmissible && !staged.shouldIgnore) {
        staged.peer.node = contact.node_num;
        memcpy(staged.peer.publicKey, contact.user.public_key.bytes,
               sizeof(staged.peer.publicKey));
        staged.hasPeerPin = true;
      }
    }

    output = staged;
    return 0;
  }
};

}  // namespace mt
