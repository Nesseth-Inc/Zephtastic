// Pure lifecycle checks for the integrated M147 Companion workspace.
// Uses synthetic byte patterns only; it never creates or prints credentials.
#include "CompanionWorkspace.h"
#include <assert.h>
#include <stdint.h>
#include <iostream>

namespace mt::companion::test {
struct CompanionWorkspaceProbe {
  static void fill(CompanionWorkspace &workspace,uint8_t value) {
    auto *p=reinterpret_cast<volatile uint8_t *>(&workspace.scratch_);
    for(size_t i=0;i<sizeof(workspace.scratch_);++i)p[i]=value;
  }
  static bool zero(const CompanionWorkspace &workspace) {
    const auto *p=reinterpret_cast<const volatile uint8_t *>(&workspace.scratch_);
    for(size_t i=0;i<sizeof(workspace.scratch_);++i)if(p[i])return false;
    return true;
  }
};
}

int main() {
  using mt::companion::CompanionWorkspace;
  using mt::companion::test::CompanionWorkspaceProbe;
  CompanionWorkspace workspace;
  assert(CompanionWorkspaceProbe::zero(workspace));

  {
    auto denied=workspace.acquire(CompanionWorkspace::OuterLockHeldProof(false));
    assert(!denied&&denied.get()==nullptr);
    assert(CompanionWorkspaceProbe::zero(workspace));
  }

  {
    auto outer=workspace.acquire(CompanionWorkspace::OuterLockHeldProof(true));
    assert(outer&&outer.get());
    outer.get()->input.which_payload_variant=meshtastic_ToRadio_packet_tag;
    outer.get()->output.which_payload_variant=meshtastic_FromRadio_packet_tag;
    outer.get()->admin.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;
    outer.get()->reply.which_payload_variant=meshtastic_AdminMessage_get_owner_response_tag;
    auto nested=workspace.acquire(CompanionWorkspace::OuterLockHeldProof(true));
    assert(!nested&&nested.get()==nullptr); // Recursive and overlapping leases refuse.
    assert(outer.get()->input.which_payload_variant==meshtastic_ToRadio_packet_tag);
    assert(outer.get()->output.which_payload_variant==meshtastic_FromRadio_packet_tag);
    assert(outer.get()->admin.which_payload_variant==meshtastic_AdminMessage_set_owner_tag);
    assert(outer.get()->reply.which_payload_variant==meshtastic_AdminMessage_get_owner_response_tag);
    CompanionWorkspaceProbe::fill(workspace,0xA5);
  }
  assert(CompanionWorkspaceProbe::zero(workspace)); // All four structures wiped on exit.

  {
    auto recovered=workspace.acquire(CompanionWorkspace::OuterLockHeldProof(true));
    assert(recovered&&recovered.get()); // Rejected nested lease did not poison the owner.
    assert(CompanionWorkspaceProbe::zero(workspace));
    CompanionWorkspaceProbe::fill(workspace,0x5A);
  }
  assert(CompanionWorkspaceProbe::zero(workspace));

  std::cout<<"PASS workspace rejects absent outer-lock proof and nested lease; exit wipes all four message scratch objects\n";
}
