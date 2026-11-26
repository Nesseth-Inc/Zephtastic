// SPDX-License-Identifier: GPL-3.0-only
// Actual Service and NodeAdmin with in-memory storage and fake radio only.
#include "pki_runtime_fixture.h"
#include "../src/NodeAdmin.h"
#include <cassert>
#include <cerrno>
#include <cstdio>

using namespace mt;
using namespace mt::pki;

namespace {
Bytes canonicalQueuedSet() {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;
  admin.set_owner=meshtastic_User_init_zero;
  admin.session_passkey.size=8;
  memset(admin.session_passkey.bytes,0x41,8);
  uint8_t payload[233]{};auto ps=pb_ostream_from_buffer(payload,sizeof(payload));
  assert(pb_encode(&ps,meshtastic_AdminMessage_fields,&admin));
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_ADMIN_APP;
  data.want_response=true;data.has_bitfield=true;data.bitfield=2;
  data.payload.size=ps.bytes_written;memcpy(data.payload.bytes,payload,data.payload.size);
  uint8_t wire[227]{};auto ds=pb_ostream_from_buffer(wire,sizeof(wire));
  assert(pb_encode(&ds,meshtastic_Data_fields,&data));
  return Bytes(wire,wire+ds.bytes_written);
}

struct NodeStore final : SettingsBackend {
  std::map<std::string,Bytes> rows;
  unsigned writes=0;
  int writeFault=0;
  bool corruptReadback=false;
  int read(const char *key,uint8_t *out,size_t cap)override {
    const auto it=rows.find(key);
    if(it==rows.end())return -ENOENT;
    if(it->second.size()>cap)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());return int(it->second.size());
  }
  int write(const char *key,const uint8_t *p,size_t n)override {
    ++writes;
    if(writeFault==1)return -EIO;
    rows[key]=Bytes(p,p+n);
    if(corruptReadback)rows[key][100]^=1;
    return writeFault==2?-EIO:0;
  }
};

struct Fixture : ::Fixture {
  NodeStore nodeStore;
  NodeJournal journal;
  uint32_t now=100;
  explicit Fixture(bool router=false) {
    s.stop();
#if defined(CONFIG_MT_RF_ADMIN_GET)
    assert(s.bindPrivateAdminMutation(
      [](void *,uint32_t,uint32_t,const uint8_t[32],uint32_t,const uint8_t *,size_t,
         uint32_t,uint32_t &routingError)->bool {
        routingError=0;return true;
      },this));
#endif
    Config config=s.config;
    config.setTxEnabled(true);
    if(router)assert(config.setDeviceRole(2,5));
    assert(s.apply(config)&&s.start(false,now)&&s.armEndpoint(true));
    assert(journal.load(nodeStore,s.nodes,local.binding.node)==0);
    NodeCache::Entry entry{};
    entry.identity(peer.binding.node,127,1);
    assert(NodeCache::Entry::text(entry.bytes+NodeCache::Entry::Name,
                                  NodeCache::Entry::Name,"synthetic",10));
    assert(NodeCache::Entry::text(entry.bytes+NodeCache::Entry::Short,
                                  NodeCache::Entry::Short,"syn",4));
    assert(s.nodes.accept(entry,local.binding.node,-20,1)>=0);
  }
  int edit(NodeCache::Edit op,bool blocked=false) {
    return NodeAdmin::edit(s,radio,journal,nodeStore,peer.binding.node,op,++now,blocked);
  }
};

static void put32(uint8_t *p,uint32_t value) {
  for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));
}

void pendingAcceptedEndpointRefusesWithoutClearingOrWriting() {
  Fixture f;
  assert(f.s.storageStopSafe());
  uint8_t packet[17]{};
  put32(packet,f.peer.binding.node);put32(packet+4,f.local.binding.node);put32(packet+8,901);
  const unsigned stops=f.radio.stops;
  assert(f.s.submitEndpoint(0,901,packet,sizeof(packet),f.now)==0);
  const bool armed=f.s.endpointArmed();
  assert(!f.s.storageStopSafe());
  assert(f.edit(NodeCache::Edit::Favorite)==-EBUSY);
  assert(f.nodeStore.writes==0&&f.radio.stops==stops&&f.s.receiving()&&
         !f.s.faulted()&&f.s.endpointArmed()==armed&&!f.s.storageStopSafe());
}

void failedStopDoesNotCommitOrFalselyClearLiveRx() {
  Fixture f;
  f.radio.stopError=-EIO;
  const unsigned writes=f.nodeStore.writes;
  assert(f.edit(NodeCache::Edit::Ignore)==-EIO);
  assert(f.nodeStore.writes==writes&&f.journal.ready()&&f.s.faulted()&&
         f.s.receiving()&&f.s.endpointArmed()&&f.radio.healthValue==1&&
         !f.s.nodes.ignored(f.peer.binding.node));
}

#if defined(CONFIG_MT_RF_ADMIN_GET)
void queuedAuthenticatedMutationRefusesWithoutClearingOrWriting() {
  Fixture f;const Bytes request=canonicalQueuedSet();
  const unsigned stops=f.radio.stops;
  assert(f.s.beginPrivateAdministration());
  assert(f.s.privateAdminMutationRequest(f.local.binding.node,f.peer.binding.node,
      f.peer.binding.publicKey,902,2,true,request.data(),request.size(),f.now));
  assert(f.s.privateAdminMutationQueued());
  assert(f.s.endPrivateAdministration());
  assert(f.s.receiving()&&f.s.endpointArmed()&&f.s.privateAdminMutationQueued());
  const unsigned writes=f.nodeStore.writes;
  assert(f.edit(NodeCache::Edit::Favorite)==-EBUSY);
  assert(f.nodeStore.writes==writes&&f.radio.stops==stops+1&&f.s.receiving()&&
         f.s.endpointArmed()&&!f.s.faulted()&&f.s.privateAdminMutationQueued());
}
#endif

void storageFailuresNeverPublishOrRestoreAndResumeFailureIsDistinct() {
  for(int fault:{1,2}) {
    Fixture f;f.nodeStore.writeFault=fault;
    assert(f.edit(NodeCache::Edit::Favorite)==-EIO);
    assert(f.nodeStore.writes==1&&!f.journal.ready()&&!f.s.nodes.ready()&&
           !f.s.nodes.get(unsigned(f.s.nodes.find(f.peer.binding.node)))->favorite()&&
           !f.s.receiving()&&!f.s.endpointArmed()&&f.radio.healthValue==0);
  }
  {
    Fixture f;f.nodeStore.corruptReadback=true;
    assert(f.edit(NodeCache::Edit::Ignore)==-EIO);
    assert(f.nodeStore.writes==1&&!f.journal.ready()&&!f.s.nodes.ready()&&
           !f.s.nodes.ignored(f.peer.binding.node)&&!f.s.receiving()&&!f.s.endpointArmed());
  }
  {
    Fixture f;f.radio.startError=-EIO;
    assert(f.edit(NodeCache::Edit::Favorite)==NodeAdmin::CommittedRxFault);
    assert(f.nodeStore.writes==1&&f.journal.ready()&&
           f.s.nodes.get(unsigned(f.s.nodes.find(f.peer.binding.node)))->favorite()&&
           !f.s.receiving()&&!f.s.endpointArmed()&&f.s.faulted()&&
           f.radio.healthValue==0);
  }
}

void finalPeerRemovalLeavesNoAuthorityRuntimeStopped() {
  Fixture f;
  assert(f.s.receiving()&&f.s.endpointArmed()&&f.runtime.ready(f.local.binding.node));
  assert(f.edit(NodeCache::Edit::Remove)==0);
  assert(f.nodeStore.writes==1&&f.journal.generation()==1&&
         f.s.nodes.find(f.peer.binding.node)<0&&!f.runtime.ready(f.local.binding.node)&&
         !f.s.receiving()&&!f.s.endpointArmed()&&!f.s.faulted()&&
         f.radio.healthValue==0);
}

void databaseReset() {
  for(bool router:{false,true})for(bool keep:{false,true}){
    Fixture f(router);
    const auto id=f.peer.binding.node;
    assert(f.edit(NodeCache::Edit::Favorite)==0);
    NodeCache::Entry other{};other.identity(888,127,1);
    assert(f.s.nodes.accept(other,f.local.binding.node,-20,1)>=0);
    bool finalized=false;
    const int result=NodeAdmin::reset(f.s,f.radio,f.journal,f.nodeStore,keep,f.now,nullptr,
        [&](){assert(f.s.storageQuiescent());finalized=true;return 0;});
    assert(result==0&&finalized&&f.s.nodes.find(888)<0);
    assert((f.s.nodes.find(id)>=0)==(router||keep));
    NodeCache reload;NodeJournal reloaded;
    assert(reloaded.load(f.nodeStore,reload,f.local.binding.node)==0);
    assert((reload.find(id)>=0)==(router||keep)&&reload.find(888)<0);
    assert(f.s.route.identity()==f.local.binding.node);
  }
  Fixture f;
  f.nodeStore.writeFault=1;bool finalized=false;
  assert(NodeAdmin::reset(f.s,f.radio,f.journal,f.nodeStore,false,f.now,nullptr,
         [&](){finalized=true;return 0;})<0);
  assert(!finalized&&!f.s.receiving());
}
}  // namespace

int main() {
  databaseReset();
  puts("PASS NodeDB reset preserves role-selected favorites, retires other peers, reloads and rejects uncertain writes");
  pendingAcceptedEndpointRefusesWithoutClearingOrWriting();
  puts("PASS accepted endpoint work blocks local NodeDB storage edits without being cleared");
  failedStopDoesNotCommitOrFalselyClearLiveRx();
  puts("PASS uncertain native stop faults closed without falsely reporting RX off or saving policy");
#if defined(CONFIG_MT_RF_ADMIN_GET)
  queuedAuthenticatedMutationRefusesWithoutClearingOrWriting();
  puts("PASS queued authenticated Admin SET blocks NodeDB pause without losing mutation");
#endif
  storageFailuresNeverPublishOrRestoreAndResumeFailureIsDistinct();
  puts("PASS write/readback failure freezes policy; committed resume fault remains distinct");
  finalPeerRemovalLeavesNoAuthorityRuntimeStopped();
  puts("PASS final trusted-peer removal commits NodeDB and leaves no-authority runtime RF-off");
}
