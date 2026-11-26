// SPDX-License-Identifier: GPL-3.0-only
// M26 absent favorite policy tests; memory-only radio and USB/BLE framing.
#include "node_mute_fixture.h"
#include "known_channel_fixture.h"
#include <iostream>
using Cache=mt::NodeCache;
constexpr uint32_t Missing=0x66667777;
static unsigned groups=0;
#define PASS(name) do{++groups;std::cout<<"PASS " name "\n";}while(0)
static std::vector<uint8_t> favoriteWire(uint32_t id,bool set) {
  meshtastic_AdminMessage a{};
  if(set){a.which_payload_variant=meshtastic_AdminMessage_set_favorite_node_tag;a.set_favorite_node=id;}
  else {a.which_payload_variant=meshtastic_AdminMessage_remove_favorite_node_tag;a.remove_favorite_node=id;}
  return MuteFixture::adminWire(a);
}
static void request(MuteFixture &f,bool set,uint32_t id,uint32_t packet,bool ble=false,uint32_t now=1000) {
  f.usb.out.clear();f.ble.out.clear();f.rawAdmin(favoriteWire(id,set),packet,ble,MuteLocal,now);
}
struct Snapshot {
  mt::NodeCache cache;mt::Config config;std::map<std::string,std::vector<uint8_t>> durable;
  unsigned writes,starts,stops,sends,keys;uint32_t generation;int slot;mt::NodeJournal::State journalState;
  size_t history,known;bool dueSoon,dueLater;
  explicit Snapshot(MuteFixture &f):cache(f.service.nodes),config(f.service.config),durable(f.backend.entries),
    writes(f.backend.writes),starts(f.radio.starts),stops(f.radio.stops),sends(f.radio.sends),keys(f.service.signature.keyCount(1000)),
    generation(f.journal.generation()),slot(f.journal.activeSlot()),journalState(f.journal.state()),
    history(f.service.route.historyCount()),known(f.service.route.knownCount()),dueSoon(f.journal.due(50000)),dueLater(f.journal.due(70000)){}
  void unchanged(MuteFixture &f,unsigned cycles=0)const {
    assert(f.service.config.same(config)&&f.backend.entries==durable&&f.backend.writes==writes&&f.journal.generation()==generation&&
      f.journal.activeSlot()==slot&&f.journal.state()==journalState&&f.journal.due(50000)==dueSoon&&f.journal.due(70000)==dueLater);
    assert(f.service.nodes.count()==cache.count()&&f.service.nodes.dirty()==cache.dirty()&&f.service.nodes.ready()==cache.ready()&&
      f.service.nodes.protectedCount()==cache.protectedCount());
    for(unsigned i=0;i<cache.count();++i){const auto &a=*cache.get(i),&b=*f.service.nodes.get(i);
      assert(!memcmp(a.bytes,b.bytes,sizeof(a.bytes))&&a.snr==b.snr&&a.hops==b.hops&&a.heard==b.heard);}
    assert(f.radio.starts==starts+cycles&&f.radio.stops==stops+cycles&&f.radio.sends==sends&&
      f.service.signature.keyCount(1000)==keys&&f.service.route.historyCount()==history&&f.service.route.knownCount()==known);
  }
};
static void histories(MuteFixture &f) {
  const uint8_t publicKey[32]={9};assert(f.service.signature.trustPublicKey(MutePeer,publicKey,100));
  uint8_t wire[17]{};for(unsigned i=0;i<4;++i){wire[i]=0xff;wire[4+i]=uint8_t(MutePeer>>(8*i));wire[8+i]=uint8_t(0x26000001u>>(8*i));}
  wire[12]=(3u<<5)|3u;wire[13]=8;wire[15]=uint8_t(MutePeer);wire[16]=0x42;
  bench::RouteMetadata m{};m.channelDecoded=true;m.portnum=4;m.nodeInfoValid=true;m.encodedDataSize=1;
  assert(f.service.route.ingestWithMetadata(bench::Protocol::Meshtastic,wire,sizeof(wire),m,100,7));f.service.route.clearPending();
  assert(f.service.route.historyCount()==1&&f.service.route.knownCount()==1);
}
int main(){
  for(uint8_t model:{uint8_t(81),uint8_t(127)})for(bool ble:{false,true})for(bool dirty:{false,true}) {
    MuteFixture f(model);f.seed();assert(f.journal.flush(f.backend,f.service.nodes,1,true,true)==1);
    if(dirty){f.seed(MutePeer2,"Unsaved peer");f.journal.observe(f.service.nodes,100);}
    histories(f);const Snapshot before(f);
    for(bool set:{true,false})for(unsigned repeat=0;repeat<3;++repeat){const uint32_t packet=0x26000100+repeat;
      request(f,set,Missing,packet,ble);auto &host=ble?f.ble:f.usb;
      assert(host.error(packet)==0&&!host.node(Missing)&&f.service.nodes.find(Missing)<0);before.unchanged(f);
      for(const auto &v:host.out)assert(v.which_payload_variant!=meshtastic_FromRadio_node_info_tag);
    }
    f.seed(Missing,"Later observation");assert(!f.service.nodes.get(unsigned(f.service.nodes.find(Missing)))->favorite()&&f.backend.writes==before.writes);
  }
  PASS("both models/transports preserve clean and dirty cache, durable generation, TX config and retained histories; repeated absent edits ACK without NodeInfo or deferred policy");
  {
    MuteFixture f;f.seed();f.seed(MutePeer2,"Second peer");
    for(unsigned i=0;i<Cache::ProtectedLimit;++i)assert(f.service.nodes.edit(0x60000000+i,MuteLocal,Cache::Edit::Ignore)==1);
    assert(f.service.nodes.count()==Cache::Capacity&&f.service.nodes.protectedCount()==Cache::ProtectedLimit);const Snapshot before(f);
    request(f,true,Missing,0x26000200);assert(f.usb.error(0x26000200)==0);before.unchanged(f);
    request(f,false,Missing,0x26000201);assert(f.usb.error(0x26000201)==0);before.unchanged(f);
    request(f,true,MutePeer,0x26000202);assert(f.usb.error(0x26000202)==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED);before.unchanged(f);
  }
  PASS("full cache and protected-node cap do not turn absent no-ops into allocation or eviction; existing-peer cap rejection remains");
  for(bool ble:{false,true})for(bool set:{true,false}) {
    for(uint32_t id:{0u,1u,MuteLocal,0xfffffffeu,0xffffffffu}){MuteFixture f;f.seed();const Snapshot before(f);
      request(f,set,id,0x26000300,ble);assert((ble?f.ble:f.usb).error(0x26000300)==meshtastic_Routing_Error_BAD_REQUEST);before.unchanged(f);}
    const auto canonical=favoriteWire(Missing,set);std::vector<std::vector<uint8_t>> invalid;
    auto duplicate=canonical;duplicate.insert(duplicate.end(),canonical.begin(),canonical.end());invalid.push_back(duplicate);
    auto unknown=canonical;unknown.insert(unknown.end(),{0xc0,0x07,0});invalid.push_back(unknown);
    auto session=canonical;session.insert(session.end(),{0xaa,0x06,1,1});invalid.push_back(session);
    auto alternate=canonical;auto other=favoriteWire(Missing,!set);alternate.insert(alternate.end(),other.begin(),other.end());invalid.push_back(alternate);
    auto truncated=canonical;truncated.pop_back();invalid.push_back(truncated);
    auto overlong=canonical;overlong.back()|=0x80;overlong.push_back(0);invalid.push_back(overlong);
    for(const auto &wire:invalid){MuteFixture f;f.seed();const Snapshot before(f);f.rawAdmin(wire,0x26000301,ble);
      assert((ble?f.ble:f.usb).error(0x26000301)==meshtastic_Routing_Error_BAD_REQUEST);before.unchanged(f);
      request(f,set,Missing,0x26000301,ble);assert((ble?f.ble:f.usb).error(0x26000301)==0);before.unchanged(f);}
    MuteFixture f;f.seed();const Snapshot before(f);request(f,set,Missing,0,ble);
    assert((ble?f.ble:f.usb).error(0)==meshtastic_Routing_Error_BAD_REQUEST);before.unchanged(f);
  }
  PASS("self/reserved IDs, zero request, duplicate/unknown/session/alternate/truncated/overlong wire reject without state mutation or poisoning a later valid request");
  for(bool set:{true,false}) {
    MuteFixture f;f.seed();assert(f.journal.flush(f.backend,f.service.nodes,1,true,true)==1);
    f.backend.entries["mtbase/nodes1"]={1,2,3};assert(f.journal.load(f.backend,f.service.nodes,MuteLocal)==-EILSEQ);const Snapshot before(f);
    request(f,set,Missing,0x26000400);assert(f.usb.error(0x26000400)==meshtastic_Routing_Error_NO_INTERFACE);before.unchanged(f);
  }
  PASS("frozen storage still rejects absent favorite requests without a write or false success");
  for(bool set:{true,false}) {
    // This active-TX case supplies a synthetic known-idle channel sample;
    // an unbound/unsupported meter must continue to refuse real admission.
    mt::test::KnownChannelFixtureClock channelClock;
    MuteFixture f;f.seed();channelClock.bind(f.service);
    assert(channelClock.set(1)&&f.service.start(false,1)&&f.service.armEndpoint(true));
    const auto queued=f.textFrame(MuteLocal,0xffffffffu,0x26000500,"Synthetic queued work");
    assert(!f.service.submitMessage(0,queued.data(),queued.size(),false,100));const auto free=f.service.endpointFree();const Snapshot before(f);
    assert(channelClock.set(101));request(f,set,Missing,0x26000501,false,101);assert(f.usb.error(0x26000501)==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED&&f.service.endpointArmed()&&f.service.receiving()&&f.service.endpointFree()==free);before.unchanged(f);
    assert(channelClock.set(300));f.service.poll(300,700);assert(f.service.active()&&f.radio.sends==1);const Snapshot active(f);
    assert(channelClock.set(301));request(f,set,Missing,0x26000502,false,301);assert(f.usb.error(0x26000502)==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED&&f.service.active());active.unchanged(f);
    MuteFixture blocked;blocked.seed();const Snapshot unchanged(blocked);
    assert(mt::NodeAdmin::edit(blocked.service,blocked.radio,blocked.journal,blocked.backend,Missing,set?Cache::Edit::Favorite:Cache::Edit::Unfavorite,1,true)==-EBUSY);unchanged.unchanged(blocked);
  }
  PASS("blocked, armed, queued and active synthetic work retain pre-stop refusal and are never cancelled to acknowledge a no-op");
  for(bool set:{true,false}) {
    MuteFixture f;f.seed();histories(f);f.handshake();f.handshake(true);assert(f.service.start(false,1));
    auto text=f.textFrame(MutePeer,0xffffffffu,0x26000600,"Retained duplicate");f.receiveBoth(text,100);assert(f.usb.previews.size()==1&&f.ble.previews.size()==1);
    const Snapshot before(f);request(f,set,Missing,0x26000601);assert(f.usb.error(0x26000601)==0&&f.service.receiving()&&!f.service.endpointArmed());before.unchanged(f,1);
    f.receiveBoth(text,1001);assert(f.usb.previews.size()==1&&f.ble.previews.size()==1);
  }
  PASS("healthy receiving no-op retains existing stop/restore cycle and preserves both transport duplicate histories");
  for(bool set:{true,false}) {
    MuteFixture health;health.seed();health.radio.receiving=true;const Snapshot before(health);
    request(health,set,Missing,0x26000700);assert(health.usb.error(0x26000700)==meshtastic_Routing_Error_NO_INTERFACE);before.unchanged(health);
    MuteFixture stop;stop.seed();assert(stop.service.start(false,1));stop.radio.stopResult=-EIO;const auto durable=stop.backend.entries;
    // A hard stop error has no verified standby. Faulted live bookkeeping
    // must remain non-quiescent; it cannot authorize a storage write.
    request(stop,set,Missing,0x26000701);assert(stop.usb.error(0x26000701)==meshtastic_Routing_Error_NO_INTERFACE&&stop.service.faulted()&&stop.service.receiving()&&!stop.service.storageQuiescent()&&!stop.backend.writes&&stop.backend.entries==durable);
    MuteFixture restore;restore.seed();assert(restore.service.start(false,1));restore.radio.startResult=-EIO;
    request(restore,set,Missing,0x26000702);assert(restore.usb.error(0x26000702)==meshtastic_Routing_Error_NO_INTERFACE&&restore.service.faulted()&&!restore.service.receiving()&&!restore.backend.writes&&restore.journal.ready()&&restore.service.nodes.ready());
    assert(!restore.usb.notice(0x26000702,"persisted")&&restore.service.nodes.find(Missing)<0);
  }
  PASS("unhealthy radio, failed stop and failed restore remain failures; an uncommitted no-op never claims persisted/degraded success");
  {
    MuteFixture f;f.seed();request(f,true,MutePeer,0x26000800);assert(f.usb.error(0x26000800)==0&&f.usb.node(MutePeer)->is_favorite&&f.backend.writes==1);
    request(f,true,MutePeer,0x26000801);assert(f.usb.error(0x26000801)==0&&f.backend.writes==1);
    request(f,false,MutePeer,0x26000802);assert(f.usb.error(0x26000802)==0&&!f.usb.node(MutePeer)->is_favorite&&f.backend.writes==2);
    assert(f.service.nodes.edit(Missing,MuteLocal,Cache::Edit::Unignore)==-ENOENT&&f.service.nodes.edit(Missing,MuteLocal,Cache::Edit::ToggleMute)==-ENOENT);
    assert(f.service.nodes.edit(Missing,MuteLocal,static_cast<Cache::Edit>(99))==-EINVAL);
    assert(f.service.nodes.edit(Missing,MuteLocal,Cache::Edit::Ignore)==1&&f.service.nodes.ignored(Missing));
  }
  PASS("existing favorite persistence/idempotence and missing ignore/unignore/mute distinctions remain unchanged");
  std::cout<<"PASS "<<groups<<" semantic groups; memory-only, no physical transport or RF\n";
}
