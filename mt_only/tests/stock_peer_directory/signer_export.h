// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "meshtastic/deviceonly.pb.h"
#include "NodeJournal.h"

namespace stock_export {
#include "stock-nodeinfo-flags.inc"
struct NoRadioSlot { uint16_t committedLoraSlot() const { return 0; } };
inline NoRadioSlot *nodeDB=nullptr;
struct TypeConversions {
  static meshtastic_NodeInfo ConvertToNodeInfo(const meshtastic_NodeInfoLite *,const meshtastic_PositionLite *,
                                               const meshtastic_DeviceMetrics *);
  static meshtastic_NodeInfo ConvertToNodeInfoThin(const meshtastic_NodeInfoLite *);
  // These test records carry only stock's node-level trust flags.
  static meshtastic_User ConvertToUser(const meshtastic_NodeInfoLite *) { std::abort(); }
};
#include "stock-nodeinfo-export.inc"
}

static unsigned restoredSignerExport() {
  using Entry=mt::NodeCache::Entry;
  constexpr uint32_t first=0x45000100;
  auto f=std::make_unique<Fixture>();
  f->provision();
  {
    mt::NodeCache before;
    mt::NodeJournal journal;
    assert(!journal.load(f->backend,before,Node));
    for (unsigned i=0;i<4;++i) {
      Entry entry{};
      entry.identity(first+i,127,0);
      assert(Entry::text(entry.bytes+Entry::Name,40,"Peer",5));
      assert(Entry::text(entry.bytes+Entry::Short,5,"P",2));
      entry.bytes[Entry::KeyLength]=32;
      memset(entry.bytes+Entry::Key,0x42,32);
      assert(before.ownerContact(entry,Node,false,i==1,false)>=0);
      if (i==0 || i==3) {
        assert(before.rememberVerifiedSigner(first+i,Node));
      }
      if (i==3) {
        memset(entry.bytes+Entry::Key,0x24,32);
        assert(before.ownerContact(entry,Node,false,true,false)>=0);
      }
    }
    assert(journal.flush(f->backend,before,1,true,true)==1);
  }
  mt::NodeJournal reboot;
  assert(!reboot.load(f->backend,f->service.nodes,Node));
  unsigned comparisons=0;
  for (auto nonce : {69421u,77u}) {
    f->usb.out.clear();
    meshtastic_ToRadio hello{};
    hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
    hello.want_config_id=nonce;
    f->input(hello);
    unsigned seen=0;
    for (const auto &frame : f->usb.out) {
      if (frame.which_payload_variant!=meshtastic_FromRadio_node_info_tag || frame.node_info.num==Node)continue;
      const auto &node=frame.node_info;
      const uint32_t i=node.num-first;
      assert(i<4 && !(seen&(1u<<i)));
      seen|=1u<<i;
      meshtastic_NodeInfoLite lite{};
      lite.num=node.num;
      if (i==0)lite.bitfield|=NODEINFO_BITFIELD_HAS_XEDDSA_SIGNED_MASK;
      if (i==1 || i==3)lite.bitfield|=NODEINFO_BITFIELD_IS_KEY_MANUALLY_VERIFIED_MASK;
      const auto stock=stock_export::TypeConversions::ConvertToNodeInfoThin(&lite);
      assert(node.has_xeddsa_signed==stock.has_xeddsa_signed);
      assert(node.is_key_manually_verified==stock.is_key_manually_verified);
      ++comparisons;
    }
    assert(seen==0xf);
  }
  return comparisons;
}
