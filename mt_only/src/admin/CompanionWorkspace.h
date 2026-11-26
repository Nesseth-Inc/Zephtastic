// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "meshtastic/mesh.pb.h"
#include "meshtastic/admin.pb.h"
#include <stddef.h>
#include <stdint.h>

namespace mt { class Companion; }

namespace mt::companion {

namespace test { struct CompanionWorkspaceProbe; }

// Proposal only: a caller-owned decode/encode workspace. This contains no
// transport framing, parser, ownership, replay, sequence, or session state.
class CompanionWorkspace {
public:
  struct Scratch {
    meshtastic_ToRadio input{};
    meshtastic_FromRadio output{};
    meshtastic_AdminMessage admin{};
    meshtastic_AdminMessage reply{};
  };

  // A proof token is an explicit assertion from the outer service-lock owner.
  // It is not a mutex and does not acquire or verify the platform lock.
  class OuterLockHeldProof {
    bool held_;
  public:
    explicit constexpr OuterLockHeldProof(bool held) noexcept:held_(held){}
    constexpr bool held()const noexcept{return held_;}
  };

  class Lease {
    friend class CompanionWorkspace;
    CompanionWorkspace *workspace_=nullptr;
    explicit Lease(CompanionWorkspace *workspace) noexcept:workspace_(workspace){}
  public:
    Lease() noexcept=default;
    Lease(const Lease &)=delete;
    Lease &operator=(const Lease &)=delete;
    Lease(Lease &&)=delete;
    Lease &operator=(Lease &&)=delete;
    ~Lease(){if(workspace_)workspace_->release();}

    explicit operator bool()const noexcept{return workspace_!=nullptr;}
    Scratch *get() noexcept{return workspace_?&workspace_->scratch_:nullptr;}
    const Scratch *get()const noexcept{return workspace_?&workspace_->scratch_:nullptr;}
  };

private:
  Scratch scratch_{};
  bool leased_=false;

  static void wipe(void *value,size_t size) noexcept {
    volatile uint8_t *bytes=static_cast<volatile uint8_t *>(value);
    while(size--)*bytes++=0;
  }
  void wipeAll() noexcept {
    // The complete aggregate covers each sensitive structure plus any ABI
    // padding between members, so no stale bytes survive a lease.
    wipe(&scratch_,sizeof(scratch_));
  }
  void release() noexcept {
    // Wipe before making the lease available to another transport.
    wipeAll();leased_=false;
  }

  friend struct test::CompanionWorkspaceProbe;
  friend class ::mt::Companion;
  Scratch &scratch() noexcept{return scratch_;}

public:
  CompanionWorkspace(){wipeAll();}
  CompanionWorkspace(const CompanionWorkspace &)=delete;
  CompanionWorkspace &operator=(const CompanionWorkspace &)=delete;
  ~CompanionWorkspace(){wipeAll();}

  // Called once at a scratch-using public entrypoint while the owning
  // application service lock is held. Nested/overlapping calls fail closed.
  Lease acquire(OuterLockHeldProof proof) noexcept {
    if(!proof.held()||leased_)return Lease{};
    leased_=true;
    return Lease(this);
  }
};

// Compiler intrinsic keeps the invariant available with Zephyr minimal C++.
static_assert(__is_trivially_copyable(CompanionWorkspace::Scratch));
static_assert(offsetof(CompanionWorkspace::Scratch,input)==0);

} // namespace mt::companion
