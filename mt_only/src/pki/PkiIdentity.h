// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiKeyProvider.h"
#include "../SettingsJournal.h"

namespace mt::pki {
// Best-effort compiler-resistant clearing of our own buffers. This cannot erase
// backend copies, registers, previous flash records, crash dumps or caller data.
void eraseIdentityBytes(void *data,size_t length);

struct IdentityBinding {
  uint8_t version=1,algorithm=1; // schema 1, clamped X25519 scalar
  uint32_t generation=0,node=0;
  uint8_t publicKey[32]{};
};

// Explicit credential ingress only; no entropy, defaults, export or migration.
// The caller owns and must clear the original supplied scalar. This copy is
// noncopyable and cleared on move/destruction, including rejected candidates.
class IdentityCandidate {
  friend class IdentityJournal;
  IdentityBinding binding_{};
  uint8_t scalar_[32]{};
public:
  IdentityCandidate(const IdentityBinding &,const uint8_t scalar[32]);
  IdentityCandidate(const IdentityCandidate &)=delete;
  IdentityCandidate &operator=(const IdentityCandidate &)=delete;
  IdentityCandidate(IdentityCandidate &&) noexcept;
  IdentityCandidate &operator=(IdentityCandidate &&)=delete;
  ~IdentityCandidate();
};

struct IdentityDeriver {
  virtual ~IdentityDeriver()=default;
  // Trusted backend: derive X25519 public key; never retain scalar or output.
  // A false result, including partial output, is rejected and scratch cleared.
  virtual bool derivePublic(const uint8_t scalar[32],uint8_t output[32])=0;
};
// Concrete production derivation, exercised with the real library in host tests.
// Injection above exists for fault testing, not for a caller's validity claim.
struct MonocypherIdentityDeriver final:IdentityDeriver {
  bool derivePublic(const uint8_t scalar[32],uint8_t output[32]) override;
};

// Dormant infrastructure: no application, ZephyrSettings or transport wiring.
// Single owner, serialized by the future service/provisioning lock. Backend
// must satisfy SettingsBackend's commit/read contract. Reentry is rejected.
class IdentityJournal final:public KeyProvider {
public:
  static constexpr size_t RecordSize=84;
  enum class State:uint8_t {Unloaded,Empty,Ready,CorruptFrozen,IoFrozen,Closed};
private:
  IdentityBinding binding_{};
  uint8_t scalar_[32]{};
  State state_=State::Unloaded;
  int8_t active_=-1;
  bool busy_=false;
  void clearActive();
  int freeze(State,int);
public:
  IdentityJournal()=default;
  IdentityJournal(const IdentityJournal &)=delete;
  IdentityJournal &operator=(const IdentityJournal &)=delete;
  IdentityJournal(IdentityJournal &&)=delete;
  IdentityJournal &operator=(IdentityJournal &&)=delete;
  ~IdentityJournal() override;
  State state()const{return state_;}
  // Exactly one load per instance. No automatic recovery or fallback. Frozen
  // objects cannot reload: explicit future recovery requires a fresh instance.
  int load(SettingsBackend &,IdentityDeriver &);
  // Entire explicit tuple commits to the inactive slot, followed by byte-exact
  // readback. Require Empty/Ready, RF/storage quiescence and exact next generation
  // (first=1); no wrap/no-op elision. Invalid candidates leave the old key usable.
  // Any attempted-write uncertainty freezes use/writes and clears the RAM key.
  // This API is not an authorization mechanism; no live provisioning calls exist.
  int store(SettingsBackend &,IdentityDeriver &,const IdentityCandidate &,bool quiescent);
  bool binding(IdentityBinding &,KeyRef &)const;
  bool active(KeyRef)const override;
  bool withPrivate(KeyRef,PrivateKeyOperation &) override;
  // Permanent per-instance deactivation, without modifying durable records.
  // A borrowed callback cannot close/replace/reenter its owning provider.
  bool close();
};
}
