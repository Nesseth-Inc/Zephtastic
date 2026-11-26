// SPDX-License-Identifier: GPL-3.0-only
// M21 persisted mute/admin/delivery tests. Memory-only simulated radio.
#include "node_mute_fixture.h"
#include "known_channel_fixture.h"
#include <iostream>

static unsigned groups=0;
#define PASS(name) do{++groups;std::cout<<"PASS " name "\n";}while(0)
static uint32_t crc32(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static void seal(std::vector<uint8_t> &raw){put32(raw.data()+raw.size()-4,crc32(raw.data(),raw.size()-4));}

int main(){using Cache=mt::NodeCache;using Journal=mt::NodeJournal;
  {
    MuteFixture f;f.seed();f.usb.out.clear();f.toggle(MutePeer,0x21000001,false,10);
    assert(f.usb.error(0x21000001)==meshtastic_Routing_Error_NONE&&f.service.nodes.muted(MutePeer)&&f.backend.writes==1);
    const auto *update=f.usb.node(MutePeer);assert(update&&update->is_muted&&update->has_user&&!update->is_favorite&&!update->is_ignored);
    f.ble.out.clear();f.handshake(true,11);const auto *cross=f.ble.node(MutePeer);assert(cross&&cross->is_muted);
    Cache restored;Journal reboot;assert(!reboot.load(f.backend,restored,MuteLocal)&&restored.muted(MutePeer));
    f.ble.out.clear();f.toggle(MutePeer,0x21000002,true,12);assert(f.ble.error(0x21000002)==0&&!f.service.nodes.muted(MutePeer));
    Cache final;Journal again;assert(!again.load(f.backend,final,MuteLocal)&&!final.muted(MutePeer)&&!f.radio.sends);
    PASS("canonical USB/BLE toggle persists before current NodeInfo publication and cross-transport handshake readback");
  }
  {
    MuteFixture f;f.seed();f.seed(MutePeer2,"Second peer");f.toggle(MutePeer,0x21000100,false,20);
    const auto writes=f.backend.writes;assert(f.service.nodes.muted(MutePeer));
    f.usb.out.clear();f.toggle(MutePeer,0x21000100,false,21);assert(f.usb.error(0x21000100)==0&&f.backend.writes==writes&&f.service.nodes.muted(MutePeer)&&f.usb.node(MutePeer));
    f.usb.out.clear();f.toggle(MutePeer2,0x21000100,false,22);assert(f.usb.error(0x21000100)==meshtastic_Routing_Error_BAD_REQUEST&&f.backend.writes==writes&&!f.service.nodes.muted(MutePeer2));
    f.usb.out.clear();f.toggle(MutePeer,0x21000101,false,23);assert(f.usb.error(0x21000101)==0&&!f.service.nodes.muted(MutePeer)&&f.backend.writes==writes+1);
    // A different transport has its own request namespace and can deliberately toggle.
    f.ble.out.clear();f.toggle(MutePeer,0x21000101,true,24);assert(f.ble.error(0x21000101)==0&&f.service.nodes.muted(MutePeer));
    const auto beforeReset=f.backend.writes;f.companion.resetStream();f.usb.out.clear();f.toggle(MutePeer,0x21000101,false,25);
    assert(f.usb.error(0x21000101)==0&&!f.service.nodes.muted(MutePeer)&&f.backend.writes==beforeReset+1);
    PASS("eight-entry per-transport stream replay ledger makes exact same-ID retry inert, rejects collision, and defines new-ID/cross-transport/reset toggles");
  }
  {
    std::vector<std::vector<uint8_t>> invalid;
    auto canonical=MuteFixture::toggleWire(MutePeer);
    auto unknown=canonical;unknown.insert(unknown.end(),{0xc0,0x07,0x00});invalid.push_back(unknown);
    auto duplicate=canonical;duplicate.insert(duplicate.end(),canonical.begin(),canonical.end());invalid.push_back(duplicate);
    auto session=canonical;session.insert(session.end(),{0xaa,0x06,0x01,0x01});invalid.push_back(session);
    auto alternate=canonical;auto favorite=MuteFixture::adminWire([]{meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_favorite_node_tag;a.set_favorite_node=MutePeer2;return a;}());
    alternate.insert(alternate.end(),favorite.begin(),favorite.end());invalid.push_back(alternate);
    invalid.push_back({0x88,0x03,0xa1,0x86,0x95,0xbb,0x88,0x00}); // overlong target
    invalid.push_back({0x88,0x03,0x80,0x80,0x80,0x80,0x10});      // uint32 overflow
    invalid.push_back({0x88,0x03,0x80});                            // truncated
    for(uint8_t model:{uint8_t(81),uint8_t(127)})for(bool useBle:{false,true}){
      for(size_t i=0;i<invalid.size();++i){MuteFixture f(model);f.seed();f.seed(MutePeer2,"Second peer");const auto before=f.snapshot();
        const uint32_t request=0x21000200+uint32_t(i);
        f.rawAdmin(invalid[i],request,useBle,MuteLocal,30+uint32_t(i));auto &host=useBle?f.ble:f.usb;
        assert(host.error(request)==meshtastic_Routing_Error_BAD_REQUEST);f.assertUnchanged(before);
        host.out.clear();f.toggle(MutePeer,request,useBle,60);assert(host.error(request)==0&&f.service.nodes.muted(MutePeer)&&f.backend.writes==1);
      }
      for(uint32_t target:{0u,1u,MuteLocal,0xfffffffeu,0xffffffffu,0x11112222u}){MuteFixture f(model);f.seed();const auto before=f.snapshot();
        const uint32_t request=0x22000000u+(uint32_t(model)<<16)+(useBle?0x100u:0u)+(target&0xffu);
        f.toggle(target,request,useBle,40);auto &host=useBle?f.ble:f.usb;assert(host.error(request)==meshtastic_Routing_Error_BAD_REQUEST);
        f.assertUnchanged(before);
      }
      MuteFixture zero(model);zero.seed();zero.seed(MutePeer2,"Second peer");const auto before=zero.snapshot();
      zero.toggle(MutePeer,0,useBle,50);auto &host=useBle?zero.ble:zero.usb;
      assert(host.error(0)==meshtastic_Routing_Error_BAD_REQUEST);zero.assertUnchanged(before);
    }
    PASS("both boards/transports reject missing/self/reserved/zero-ID, unknown, duplicate, alternate, session, malformed, overlong and overflow inputs with exact state nonmutation and no replay poisoning");
  }
  {
    mt::test::KnownChannelFixtureClock knownChannelClock;MuteFixture f;knownChannelClock.bind(f.service);
    f.seed();assert(f.service.start(false,1)&&f.service.armEndpoint(true));const auto writes=f.backend.writes;
    f.toggle(MutePeer,0x21000400,false,2);
    assert(f.usb.error(0x21000400)==0&&f.backend.writes==writes+1&&f.service.nodes.muted(MutePeer)&&
      f.service.receiving()&&f.service.endpointArmed()&&!f.service.faulted()&&!f.radio.sends);
    PASS("idle armed mute edit persists once and restores exact RX and arm intent");
  }
  {
    mt::test::KnownChannelFixtureClock clock;MuteFixture f;clock.bind(f.service);f.seed();
    assert(f.service.start(false,1)&&f.service.armEndpoint(true));
    meshtastic_ToRadio text{};text.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=text.packet;
    p.id=0x21000402;p.to=MutePeer;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;p.decoded.payload.size=1;p.decoded.payload.bytes[0]='q';
    f.input(text,false,2);assert(f.service.endpointFree()==3);const auto before=f.snapshot();
    f.toggle(MutePeer,0x21000403,false,3);
    assert(f.usb.error(0x21000403)==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED);f.assertUnchanged(before);
    PASS("accepted queued message blocks mute edit without storage or policy mutation");
  }
  {
    mt::test::KnownChannelFixtureClock clock;MuteFixture f;clock.bind(f.service);f.seed();
    assert(f.service.start(false,1));const auto writes=f.backend.writes;f.radio.stopResult=-EIO;
    f.toggle(MutePeer,0x21000401,false,3);
    assert(f.usb.error(0x21000401)==meshtastic_Routing_Error_NO_INTERFACE&&f.backend.writes==writes&&
      !f.service.nodes.muted(MutePeer)&&f.service.receiving()&&f.radio.receiving&&f.service.faulted()&&!f.radio.sends);
    PASS("failed radio stop retains unknown receive state and faults before storage or live mute mutation");
  }
  for(int failure:{1,2}){
    MuteFixture f;f.seed();assert(f.journal.flush(f.backend,f.service.nodes,0,true,true)==1);const auto baseWrites=f.backend.writes;f.backend.failure=failure;
    f.toggle(MutePeer,0x21000500+failure,false,5);assert(f.usb.error(0x21000500+failure)==meshtastic_Routing_Error_NO_INTERFACE&&
      !f.service.nodes.ready()&&!f.journal.ready()&&!f.service.nodes.muted(MutePeer)&&f.backend.writes==baseWrites+1);
    f.backend.failure=0;Cache restored;Journal reboot;assert(!reboot.load(f.backend,restored,MuteLocal)&&restored.muted(MutePeer)==(failure==2));
  }
  PASS("before-write and acknowledged-after-write failures never publish uncertain live policy; restart selects actual durable generation");
  for(unsigned lost:{1u,2u,3u}){
    MuteFixture f;f.seed();assert(f.service.start(false,1));f.radio.startResult=-EIO;
    f.usb.dropNodeInfo=lost==1;f.usb.dropRouting=lost==2;f.usb.dropDegraded=lost==3;f.usb.out.clear();
    const uint32_t request=0x21000510+lost;f.toggle(MutePeer,request,false,2);
    assert(f.service.nodes.muted(MutePeer)&&f.backend.writes==1&&f.service.faulted()&&f.usb.dropped==1);
    const auto writes=f.backend.writes;f.radio.startResult=0;f.usb.dropNodeInfo=f.usb.dropRouting=f.usb.dropDegraded=false;f.usb.out.clear();
    f.toggle(MutePeer,request,false,3);
    assert(f.usb.error(request)==meshtastic_Routing_Error_NONE&&f.usb.node(MutePeer)&&f.service.nodes.muted(MutePeer)&&f.backend.writes==writes&&
      f.usb.notice(request,"NodeDB saved")&&f.usb.notice(request,"interface faulted"));
  }
  PASS("verified mute commit replay preserves current NodeInfo, success and degraded-interface outcome after each lost output class");
  {
    MuteFixture base;base.seed();assert(base.journal.flush(base.backend,base.service.nodes,0,true,true)==1);
    auto v2=base.backend.entries.at("mtbase/nodes0");v2[3]=2;seal(v2);
    for(uint8_t version:{uint8_t(1),uint8_t(2)}){
      auto legacy=v2;legacy[3]=version;seal(legacy);MuteBackend backend;backend.entries["mtbase/nodes0"]=legacy;Cache cache;Journal journal;
      assert(!journal.load(backend,cache,MuteLocal)&&!cache.muted(MutePeer)&&!backend.writes);
      assert(!journal.edit(backend,cache,MutePeer,Cache::Edit::ToggleMute,1,true)&&cache.muted(MutePeer)&&backend.writes==1&&backend.entries.at("mtbase/nodes0")==legacy&&backend.entries.at("mtbase/nodes1")[3]==3);
    }
    auto forbidden=v2;forbidden[16+Cache::Entry::LocalFlags]|=Cache::Entry::Muted;seal(forbidden);MuteBackend bad;bad.entries["mtbase/nodes0"]=forbidden;Cache invalid;Journal reject;
    assert(reject.load(bad,invalid,MuteLocal)==-EILSEQ&&!invalid.ready());
    PASS("v1/v2 migrate read-only to unmuted then alternate-slot v3; v2 cannot reinterpret its formerly reserved mute bit");
  }
  {
    MuteFixture base;base.seed();assert(base.journal.flush(base.backend,base.service.nodes,0,true,true)==1);auto v2=base.backend.entries.at("mtbase/nodes0");v2[3]=2;seal(v2);
    for(unsigned cut=0;cut<Journal::RecordSize;++cut){MuteBackend backend;backend.entries["mtbase/nodes0"]=v2;Cache cache;Journal journal;assert(!journal.load(backend,cache,MuteLocal));backend.cut=int(cut);
      assert(journal.edit(backend,cache,MutePeer,Cache::Edit::ToggleMute,1,true)==-EIO&&!cache.ready()&&!cache.muted(MutePeer)&&backend.entries.at("mtbase/nodes0")==v2);
      backend.cut=-1;Cache recovered;Journal reboot;assert(reboot.load(backend,recovered,MuteLocal)==-EILSEQ&&!recovered.ready()&&!recovered.muted(MutePeer));}
    PASS("all 1492 v2-to-v3 partial-write cut points freeze prior live state and reject ambiguous reboot fallback");
  }
  {
    Cache cache;MuteFixture f;f.seed();assert(f.service.nodes.edit(MutePeer,MuteLocal,Cache::Edit::ToggleMute)==1);
    auto refreshed=*f.service.nodes.get(unsigned(f.service.nodes.find(MutePeer)));strcpy(reinterpret_cast<char *>(refreshed.bytes+Cache::Entry::Name),"Refreshed muted peer");refreshed.bytes[Cache::Entry::LocalFlags]=0;
    assert(f.service.nodes.accept(refreshed,MuteLocal,2,0)>=0&&f.service.nodes.muted(MutePeer)&&!f.service.nodes.ignored(MutePeer)&&f.service.nodes.protectedCount()==0);
    for(unsigned i=0;i<Cache::Capacity;++i)f.seed(0x60000000u+i,"Evictable peer");
    assert(f.service.nodes.count()==Cache::Capacity&&f.service.nodes.find(MutePeer)<0);
    PASS("mute survives received identity refresh but is neither ignore/favorite nor protected from bounded cache eviction");
  }
  for(unsigned lost:{1u,2u}){
    MuteFixture f;f.seed();f.usb.dropNodeInfo=lost==1;f.usb.dropRouting=lost==2;f.toggle(MutePeer,0x21000580+lost,false,1);
    assert(f.service.nodes.muted(MutePeer)&&f.backend.writes==1&&f.usb.dropped==1);const auto writes=f.backend.writes;
    f.usb.dropNodeInfo=f.usb.dropRouting=false;f.usb.out.clear();f.toggle(MutePeer,0x21000580+lost,false,2);
    assert(f.usb.error(0x21000580+lost)==0&&f.usb.node(MutePeer)&&f.service.nodes.muted(MutePeer)&&f.backend.writes==writes);
  }
  PASS("lost NodeInfo or routing output cannot make an exact same-ID retry reverse the verified toggle");
  {
    MuteFixture f;f.seed();for(unsigned i=0;i<9;++i){f.usb.out.clear();f.toggle(MutePeer,0x21000700+i,false,10+i);assert(f.usb.error(0x21000700+i)==0);}
    assert(f.service.nodes.muted(MutePeer)&&f.backend.writes==9);f.usb.out.clear();f.toggle(MutePeer,0x21000700,false,30);
    assert(f.usb.error(0x21000700)==0&&!f.service.nodes.muted(MutePeer)&&f.backend.writes==10);
    PASS("ninth successful toggle evicts the oldest of exactly eight replay entries; an older ID has no replay guarantee");
  }
  {
    MuteFixture f;f.seed();f.toggle(MutePeer,0x21000800,false,1);assert(f.service.nodes.muted(MutePeer));
    assert(!mt::NodeAdmin::edit(f.service,f.radio,f.journal,f.backend,MutePeer,Cache::Edit::Remove,2));const auto removedWrites=f.backend.writes;
    f.usb.out.clear();f.toggle(MutePeer,0x21000800,false,3);assert(f.usb.error(0x21000800)==meshtastic_Routing_Error_BAD_REQUEST&&f.backend.writes==removedWrites);
    f.seed();f.usb.out.clear();f.toggle(MutePeer,0x21000800,false,4);
    assert(f.usb.error(0x21000800)==0&&!f.service.nodes.muted(MutePeer)&&f.backend.writes==removedWrites&&f.usb.node(MutePeer));
    PASS("remembered retry refuses a removed peer and stays inert after same-node re-admission within the stream epoch");
  }
  {
    MuteFixture f;f.seed();f.toggle(MutePeer,0x21000810,false,1);for(unsigned i=0;i<Cache::Capacity;++i)f.seed(0x61000000u+i,"Evictable peer");
    assert(f.service.nodes.find(MutePeer)<0);const auto writes=f.backend.writes;f.usb.out.clear();f.toggle(MutePeer,0x21000810,false,2);
    assert(f.usb.error(0x21000810)==meshtastic_Routing_Error_BAD_REQUEST&&f.backend.writes==writes);f.seed();f.usb.out.clear();f.toggle(MutePeer,0x21000810,false,3);
    assert(f.usb.error(0x21000810)==0&&!f.service.nodes.muted(MutePeer)&&f.backend.writes==writes);
    PASS("remembered retry does not recreate an evicted peer and stays inert after same-node cache re-admission");
  }
  {
    // Each mode places an observed DTR fall at a distinct boundary: after RX
    // extraction, after the second producer-excluding validation but before
    // parsing, and immediately before the final byte could dispatch admin.
    for(unsigned mode=1;mode<=3;++mode) {
      MuteFixture f;f.seed();f.toggle(MutePeer,0x21000820,false,1);assert(f.service.nodes.muted(MutePeer));
      mt::UsbSessionEpoch epoch;unsigned resets=0;bool measured=true,dtr=true,epochChanged=false;std::vector<uint8_t> rx,tx;
      auto line=[&]{const auto state=epoch.bind(measured,dtr,[&]{rx.clear();tx.clear();epochChanged=true;},
          [&]{tx.clear();epochChanged=true;},[&]{rx.clear();tx.clear();});
        f.usb.sessionMeasured=state.measured;f.usb.sessionConnected=state.connected;f.usb.sessionGeneration=state.generation;return state;};
      auto driver=[&](const std::vector<uint8_t> &bytes){const auto state=line();if(!state.measured||state.connected)rx.insert(rx.end(),bytes.begin(),bytes.end());};
      auto take=[&](uint32_t now,unsigned inject){
        const auto first=line();const bool firstReset=epochChanged;epochChanged=false;std::vector<uint8_t> bytes;
        if(!first.measured||first.connected)bytes.swap(rx);
        if(firstReset){++resets;f.companion.resetStream();}
        if(inject==1)dtr=false;
        const auto current=line();const bool valid=(!current.measured||current.connected)&&current.generation==first.generation;
        if(epochChanged){epochChanged=false;++resets;f.companion.resetStream();}
        if(inject==2&&valid){dtr=false;(void)line();}
        if(valid)for(size_t i=0;i<bytes.size();++i){
          if(inject==3&&i+1==bytes.size()){dtr=false;(void)line();}
          f.companion.byte(bytes[i],now,first.generation,mt::Companion::WorkspaceProof{true});
        }
      };
      f.usb.enforceSession=true;(void)line();
      f.companion.byte(0x94,2,epoch.generation(),mt::Companion::WorkspaceProof{true});dtr=false;tx={1,2,3};driver({0x94,0xc3,0x00});take(2,0);
      assert(resets==1&&rx.empty()&&tx.empty());
      dtr=true;meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=0x21000821+mode;
      driver(MuteFixture::framed(MuteFixture::inputWire(hello)));f.usb.out.clear();take(3,0);
      assert(resets==2&&!f.usb.out.empty()&&f.usb.out.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag);
      meshtastic_ToRadio toggle{};toggle.which_payload_variant=meshtastic_ToRadio_packet_tag;toggle.packet.to=MuteLocal;
      toggle.packet.id=0x21000830+mode;toggle.packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
      toggle.packet.decoded.portnum=meshtastic_PortNum_ADMIN_APP;auto admin=MuteFixture::toggleWire(MutePeer);
      memcpy(toggle.packet.decoded.payload.bytes,admin.data(),admin.size());toggle.packet.decoded.payload.size=admin.size();
      const auto request=MuteFixture::framed(MuteFixture::inputWire(toggle));const auto writes=f.backend.writes;
      driver(request);f.usb.out.clear();take(4,mode);
      assert(f.backend.writes==writes&&f.service.nodes.muted(MutePeer)&&f.usb.error(toggle.packet.id)==-1);
      // A fresh session may use the same ID deliberately. Successful mutation
      // proves the rejected old frame created neither state nor replay entry.
      dtr=true;driver(MuteFixture::framed(MuteFixture::inputWire(hello)));take(5,0);
      driver(request);f.usb.out.clear();take(6,0);
      assert(f.usb.error(toggle.packet.id)==0&&!f.service.nodes.muted(MutePeer)&&f.backend.writes==writes+1);
    }
    PASS("session-linearized USB acceptance drops low/old-generation input at extraction, post-validation and pre-dispatch boundaries without mutation or replay, while preserving new-generation input");
  }
  {
    MuteFixture f(81);f.seed();f.toggle(MutePeer,0x21000840,false,1);
    assert(f.service.nodes.muted(MutePeer)&&f.backend.writes==1);
    f.usb.unmeasuredHandshakeBoundary=true;
    // A canonical-looking framed handshake inside an accepted outer text
    // payload is data, never a transport boundary.
    meshtastic_ToRadio outer{};outer.which_payload_variant=meshtastic_ToRadio_packet_tag;outer.packet.to=MuteLocal;
    outer.packet.id=0x21000841;outer.packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    outer.packet.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
    const uint8_t embedded[]{0x94,0xc3,0x00,0x02,0x18,0x01};
    memcpy(outer.packet.decoded.payload.bytes,embedded,sizeof(embedded));outer.packet.decoded.payload.size=sizeof(embedded);
    f.input(outer,false,2);assert(f.companion.localMessages==1&&f.usb.handshakeBoundaries==0);
    const auto replayWrites=f.backend.writes;f.usb.out.clear();f.toggle(MutePeer,0x21000840,false,3);
    assert(f.usb.error(0x21000840)==0&&f.service.nodes.muted(MutePeer)&&f.backend.writes==replayWrites);
    // Preserve the exact review-6 adversarial stream: nested candidates cannot
    // matter because only a complete top-level frame is ever inspected.
    const std::vector<uint8_t> counterexample{0x94,0xc3,0x00,0x0d,0x94,0xc3,0x00,0x10,0x94,0xc3,0x00,0x06,0x18,0x94,0xc3,0x80,0x80,0x01};
    for(uint8_t byte:counterexample)f.companion.byte(byte,4,0,mt::Companion::WorkspaceProof{true});
    assert(f.usb.handshakeBoundaries==0&&f.service.nodes.muted(MutePeer)&&f.backend.writes==replayWrites);
    // Serial/JTAG has no disconnect event. An abandoned partial frame must age
    // through the established two-second framing timeout before a canonical
    // top-level handshake can explicitly own the fresh epoch.
    const std::vector<uint8_t> partial{0x94,0xc3,0x00,0x40,0x08,0x01,0x12};
    for(uint8_t byte:partial)f.companion.byte(byte,5,0,mt::Companion::WorkspaceProof{true});
    meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=0x18652194;
    const auto helloWire=MuteFixture::inputWire(hello);
    assert((helloWire==std::vector<uint8_t>{0x18,0x94,0xc3,0x94,0xc3,0x01}));
    f.usb.out.clear();for(uint8_t byte:MuteFixture::framed(helloWire))f.companion.byte(byte,3006,0,mt::Companion::WorkspaceProof{true});
    assert(f.usb.handshakeBoundaries==1&&!f.usb.out.empty()&&
      f.usb.out.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag&&
      f.usb.out.back().config_complete_id==hello.want_config_id);
    f.usb.out.clear();f.toggle(MutePeer,0x21000840,false,3007);
    assert(f.usb.error(0x21000840)==0&&!f.service.nodes.muted(MutePeer)&&f.backend.writes==2);
    // Unknown trailing wire data is not a canonical epoch delimiter. The
    // compatibility handshake can still parse, but must not clear replay.
    auto noncanonical=MuteFixture::inputWire(hello);noncanonical.insert(noncanonical.end(),{0xa0,0x06,0x01});
    for(uint8_t byte:MuteFixture::framed(noncanonical))f.companion.byte(byte,3008,0,mt::Companion::WorkspaceProof{true});
    assert(f.usb.handshakeBoundaries==1);const auto writes=f.backend.writes;
    f.usb.out.clear();f.toggle(MutePeer,0x21000840,false,3009);
    assert(f.usb.error(0x21000840)==0&&!f.service.nodes.muted(MutePeer)&&f.backend.writes==writes);
    PASS("model81 unmeasured Serial/JTAG uses only a canonical top-level handshake after framing timeout to clear replay; embedded/nested/noncanonical bytes cannot delimit");
  }
  {
    MuteFixture f;f.seed();f.toggle(MutePeer,0x21000600,false,1);assert(f.service.nodes.muted(MutePeer));f.handshake(false,2);f.handshake(true,3);
    f.usb.out.clear();f.ble.out.clear();f.usb.previews.clear();f.ble.previews.clear();assert(f.service.start(false,4));
    auto direct=f.textFrame(MutePeer,MuteLocal,0x31000001,"stored muted direct");f.receiveBoth(direct,5);
    assert(f.companion.receivedMessages==1&&f.wireless.receivedMessages==1&&f.usb.previews.size()==1&&f.ble.previews.size()==1&&
      f.usb.previews[0].from==MutePeer&&f.usb.previews[0].to==MuteLocal&&!f.usb.previews[0].local);
    auto broadcast=f.textFrame(MutePeer,0xffffffffu,0x31000002,"broadcast retains attention");f.receiveBoth(broadcast,6);
    assert(f.companion.receivedMessages==2&&f.wireless.receivedMessages==2&&f.usb.previews.back().to==0xffffffffu&&f.ble.previews.back().to==0xffffffffu);
    f.usb.out.clear();f.ble.out.clear();f.usb.previews.clear();f.ble.previews.clear();assert(!mt::NodeAdmin::edit(f.service,f.radio,f.journal,f.backend,MutePeer,Cache::Edit::Ignore,7));
    auto ignored=f.textFrame(MutePeer,MuteLocal,0x31000003,"ignored is stronger");f.receiveBoth(ignored,8);
    assert(f.usb.out.empty()&&f.ble.out.empty()&&f.usb.previews.empty()&&f.ble.previews.empty()&&f.companion.receivedMessages==2&&f.wireless.receivedMessages==2&&!f.radio.sends);
    PASS("real Companion preserves two-client direct/broadcast delivery and destination facts while ignored peer remains stronger pre-fanout policy");
  }
  std::cout<<"PASS "<<groups<<" node-mute groups; NodeJournal="<<sizeof(Journal)<<" Record="<<Journal::RecordSize<<" Companion="<<sizeof(mt::Companion)<<"\n";
}
