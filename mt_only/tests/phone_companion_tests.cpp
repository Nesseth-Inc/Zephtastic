// SPDX-License-Identifier: GPL-3.0-only
// Packet shapes from Meshtastic-Apple 8425daa6 (Channels.swift / ToRadio.swift).
// Real companion/codec/service, mock radio. No hardware or RF.
#include "Companion.h"
#include "known_channel_fixture.h"
#include "SettingsJournal.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>
constexpr uint32_t OURS=0x12345678,STALE=0x87654321,ID=0x77123456;
struct Radio:mt::Radio {
  unsigned starts=0,stops=0,sends=0;std::vector<uint8_t> frame;
  bool ready()override{return true;}int start()override{++starts;return 0;}
  int stop()override{++stops;return 0;}int restore()override{return 0;}int health()override{return 1;}
  bool busy()override{return false;}uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *p,size_t n)override{++sends;frame.assign(p,p+n);return 0;}
  bool completion(int &)override{return false;}
};
struct Host:mt::Companion::Host {
  mt::Service &s;unsigned saves=0;bool storage=true;std::vector<meshtastic_FromRadio> out;
  explicit Host(mt::Service &v):s(v){}
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
  mutable unsigned diagnosticCalls=0;
  size_t bluetoothDiagnostic(char *out,size_t capacity)const override {
    ++diagnosticCalls;return snprintf(out,capacity,"ZT-BLE synthetic diagnostic fixture");
  }
#endif
  bool ready()const override{return storage;}
  int save(const mt::Config &cfg)override {
    ++saves;if(!storage)return -EIO;const bool rx=s.receiving();s.stop();
    if(!s.apply(cfg))return -EINVAL;
    return rx&&cfg.provisioned()&&!s.start(false,0)?-EIO:0;
  }
  bool send(const uint8_t *p,size_t n)override {
    assert(n>=4&&p[0]==0x94&&p[1]==0xc3&&size_t(p[2]*256+p[3])==n-4);
    meshtastic_FromRadio r{};auto b=pb_istream_from_buffer(p+4,n-4);
    assert(pb_decode(&b,meshtastic_FromRadio_fields,&r));out.push_back(r);return true;
  }
  bool result(int code)const {
    for(const auto &r:out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&
      r.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP&&r.packet.decoded.request_id==ID) {
      meshtastic_Routing a{};auto b=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);
      assert(pb_decode(&b,meshtastic_Routing_fields,&a));
      assert(a.which_variant==meshtastic_Routing_error_reason_tag);if(a.error_reason==code)return true;
    }
    return false;
  }
  bool notice(const char *text)const {
    for(const auto &r:out)if(r.which_payload_variant==meshtastic_FromRadio_clientNotification_tag&&
      r.clientNotification.has_reply_id&&r.clientNotification.reply_id==ID&&strstr(r.clientNotification.message,text))return true;
    return false;
  }
};
struct DiagnosticHost:Host {
  enum Mode {DirtyZero,Valid,Capacity,OverCapacity,Huge,Unterminated,WrongLength,Maximum};
  Mode mode=Valid;mutable unsigned calls=0;mutable size_t capacitySeen=0;
  using Host::Host;
  size_t channelDiagnostic(char *out,size_t capacity)const override {
    ++calls;capacitySeen=capacity;assert(capacity>40);
    if(mode==Maximum) {
      memset(out,'7',capacity);memcpy(out,"channel_diag ",13);out[capacity-1]=0;return capacity-1;
    }
    const size_t n=size_t(snprintf(out,capacity,"channel_diag fixture=7 code=0"));
    if(mode==DirtyZero)return 0;
    if(mode==Capacity)return capacity;
    if(mode==OverCapacity)return capacity+1;
    if(mode==Huge)return size_t(-1);
    if(mode==Unterminated){memset(out,'7',capacity);return n;}
    if(mode==WrongLength)return n-1;
    return n;
  }
};
template<class H>struct FixtureBase {
  mt::test::KnownChannelFixtureClock knownChannelClock;
  Radio radio;mt::Service s;H h;mt::companion::CompanionWorkspace workspace;mt::Companion c;bool ble;
  explicit FixtureBase(bool b,uint8_t model=127):s(radio,true,OURS,model,true),h(s),c(s,h,model,workspace,b?1:0),ble(b){knownChannelClock.bind(s);}
  void input(const meshtastic_ToRadio &r) {
    uint8_t bytes[512];auto b=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&b,meshtastic_ToRadio_fields,&r));
    if(ble)c.packet(bytes,b.bytes_written,100,mt::Companion::WorkspaceProof{true});
    else {c.byte(0x94,100,0,mt::Companion::WorkspaceProof{true});c.byte(0xc3,100,0,mt::Companion::WorkspaceProof{true});c.byte(b.bytes_written>>8,100,0,mt::Companion::WorkspaceProof{true});c.byte(b.bytes_written,100,0,mt::Companion::WorkspaceProof{true});for(size_t i=0;i<b.bytes_written;++i)c.byte(bytes[i],100,0,mt::Companion::WorkspaceProof{true});}
  }
  void handshake() {meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=123;input(r);}
  void admin(const meshtastic_AdminMessage &a) {
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
    p.to=OURS;p.from=OURS;p.id=ID;p.want_ack=true;p.priority=meshtastic_MeshPacket_Priority_RELIABLE;
    p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
    auto b=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));assert(pb_encode(&b,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=b.bytes_written;input(r);
  }
  void provision(bool arm) {
    mt::Config cfg;cfg.bytes[4]=1;auto n=cfg.node();n.setPublic();cfg.node(n);
    assert(s.apply(cfg)&&s.start(false,0)&&s.armEndpoint(arm));handshake();h.out.clear();
  }
};
struct PhoneJournal:mt::SettingsBackend {
  std::vector<uint8_t> slots[2];unsigned writes=0;
  static int slot(const char *key){return !strcmp(key,"mtbase/state0")?0:!strcmp(key,"mtbase/state1")?1:-1;}
  int read(const char *key,uint8_t *out,size_t capacity)override {
    const int i=slot(key);if(i<0||slots[i].empty())return -ENOENT;
    if(slots[i].size()>capacity)return -E2BIG;
    memcpy(out,slots[i].data(),slots[i].size());return int(slots[i].size());
  }
  int write(const char *key,const uint8_t *p,size_t n)override {
    const int i=slot(key);if(i<0)return -EINVAL;
    slots[i].assign(p,p+n);++writes;return 0;
  }
};
using Fixture=FixtureBase<Host>;
using DiagnosticFixture=FixtureBase<DiagnosticHost>;
static std::vector<uint8_t> recordWire(const meshtastic_FromRadio &r) {
  std::vector<uint8_t> bytes(512);auto b=pb_ostream_from_buffer(bytes.data(),bytes.size());
  assert(pb_encode(&b,meshtastic_FromRadio_fields,&r));bytes.resize(b.bytes_written);return bytes;
}
static bool channelDiagnostic(const meshtastic_FromRadio &r) {
  return r.which_payload_variant==meshtastic_FromRadio_log_record_tag&&
    !strncmp(r.log_record.message,"channel_diag ",13);
}
static meshtastic_AdminMessage channel() {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;
  auto &c=a.set_channel;c.role=meshtastic_Channel_Role_PRIMARY;c.has_settings=true;
  c.settings.psk.size=1;c.settings.psk.bytes[0]=1;c.settings.has_module_settings=true;return a;
}
static meshtastic_ToRadio text(uint32_t from=OURS) {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
  p.from=from;p.to=0xffffffffu;p.id=ID;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;p.decoded.payload.size=5;memcpy(p.decoded.payload.bytes,"Hello",5);return r;
}
int main() {
  for(bool ble:{false,true}) {
    {
      auto baseline=std::make_unique<Fixture>(ble);const auto original=baseline->s.config;
      baseline->handshake();assert(baseline->h.out.back().config_complete_id==123);
      for(const auto &r:baseline->h.out)assert(!channelDiagnostic(r));
      assert(!baseline->h.saves&&!baseline->radio.starts&&!baseline->radio.stops&&!baseline->radio.sends);
      assert(!memcmp(original.bytes,baseline->s.config.bytes,sizeof(original.bytes)));
      for(auto mode:{DiagnosticHost::DirtyZero,DiagnosticHost::Valid,DiagnosticHost::Capacity,
                     DiagnosticHost::OverCapacity,DiagnosticHost::Huge,DiagnosticHost::Unterminated,
                     DiagnosticHost::WrongLength,DiagnosticHost::Maximum}) {
        auto f=std::make_unique<DiagnosticFixture>(ble);f->h.mode=mode;f->handshake();
        // Legacy callback strings are arbitrary provider output. A handshake
        // must neither invoke this callback nor expose any of its hostile
        // return shapes; output stays byte-for-byte identical to baseline.
        assert(f->h.calls==0&&f->h.capacitySeen==0);
        assert(f->h.out.size()==baseline->h.out.size());
        for(size_t i=0;i<f->h.out.size();++i) {
          const auto &r=f->h.out[i];assert(!channelDiagnostic(r));
          assert(recordWire(r)==recordWire(baseline->h.out[i]));
        }
        assert(f->h.out.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag&&
          f->h.out.back().config_complete_id==123);
        assert(!f->h.saves&&!f->radio.starts&&!f->radio.stops&&!f->radio.sends);
        assert(!f->c.encodeErrors&&!f->c.outputDrops&&f->c.handshakes==1);
        assert(!memcmp(original.bytes,f->s.config.bytes,sizeof(original.bytes)));
      }
    }
    std::cout<<"PASS phone handshake suppresses hostile legacy channel diagnostics and preserves baseline bytes "<<ble<<"\n";
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
    {
      auto baseline=std::make_unique<Fixture>(ble);baseline->handshake();
      auto f=std::make_unique<Fixture>(ble);f->handshake();unsigned reports=0;
      for(const auto &r:f->h.out)if(r.which_payload_variant==meshtastic_FromRadio_log_record_tag&&strstr(r.log_record.message,"ZT-BLE"))++reports;
      assert(reports==0&&f->h.diagnosticCalls==0&&f->h.out.size()==baseline->h.out.size());
      for(size_t i=0;i<f->h.out.size();++i)assert(recordWire(f->h.out[i])==recordWire(baseline->h.out[i]));
      assert(f->h.out.back().config_complete_id==123&&f->radio.sends==0);
    }
    std::cout<<"PASS phone handshake omits legacy Bluetooth diagnostic provider output on USB and BLE "<<ble<<"\n";
#endif
    {
      auto f=std::make_unique<Fixture>(ble);f->admin(channel());
      assert(f->h.result(0)&&f->s.config.node().isPublic()&&!f->h.saves);
      assert(!f->s.receiving()&&!f->s.endpointArmed()&&!f->radio.starts&&!f->radio.sends);
      f->h.out.clear();f->handshake();assert(!f->s.endpointArmed());
      unsigned channels=0;for(const auto &r:f->h.out)if(r.which_payload_variant==meshtastic_FromRadio_channel_tag) {
        ++channels;assert(r.channel.has_settings);assert(r.channel.settings.has_module_settings);
        assert(!r.channel.settings.module_settings.position_precision&&!r.channel.settings.module_settings.is_muted);
        assert(r.channel.role==(r.channel.index?meshtastic_Channel_Role_DISABLED:meshtastic_Channel_Role_PRIMARY));
      }assert(channels==8);
    }
    std::cout<<"PASS phone neutral channel settings and explicit disabled slots "<<ble<<"\n";
    {
      auto f=std::make_unique<Fixture>(ble);f->handshake();unsigned channels=0;
      for(const auto &r:f->h.out)if(r.which_payload_variant==meshtastic_FromRadio_channel_tag) {
        ++channels;assert(r.channel.has_settings);
        assert(r.channel.role==(r.channel.index?meshtastic_Channel_Role_DISABLED:meshtastic_Channel_Role_PRIMARY));
        assert(r.channel.settings.psk.size==(r.channel.index?0:1));
      }assert(channels==8&&!f->s.config.provisioned()&&!f->radio.starts&&!f->radio.sends);
      assert(f->s.config.bytes[4]==0&&!f->s.receiving()&&!f->s.endpointArmed());
      assert(!f->s.start(false,0)&&!f->s.armEndpoint(true));
      f->input(text());assert(f->h.result(4)&&!f->radio.starts&&!f->radio.sends);
      // Existing saved disabled-primary records must not be silently migrated.
      auto saved=f->s.config;saved.bytes[4]=1;auto n=saved.node();n.setKey(0,nullptr,0);saved.node(n);
      assert(saved.valid()&&f->s.apply(saved));f->h.out.clear();f->handshake();
      for(const auto &r:f->h.out)if(r.which_payload_variant==meshtastic_FromRadio_channel_tag)
        assert(r.channel.has_settings&&r.channel.role==meshtastic_Channel_Role_DISABLED&&!r.channel.settings.psk.size);
      assert(!f->s.start(false,0)&&!f->s.armEndpoint(true)&&!f->radio.starts&&!f->radio.sends);
      f->h.out.clear();f->input(text());assert(f->h.result(6)&&f->h.notice("disabled"));
    }
    std::cout<<"PASS phone fresh primary stays RF-off; saved disabled primary is preserved "<<ble<<"\n";
    {
      for(unsigned variant=0;variant<5;++variant) {
        auto f=std::make_unique<Fixture>(ble);auto a=channel();auto &c=a.set_channel;
        if(variant==0)c.settings.module_settings.position_precision=13;
        if(variant==1)c.settings.uplink_enabled=true;
        if(variant==2)c.settings.psk.bytes[0]=0;
        if(variant==3)c.role=meshtastic_Channel_Role_SECONDARY;
        if(variant==4){c.settings.name[0]=1;c.settings.name[1]=0;}
        f->admin(a);assert(f->h.result(32)&&f->h.notice("Channel setup rejected")&&!f->h.saves&&!f->radio.starts);
      }
    }
    std::cout<<"PASS phone unsupported channel settings still reject without mutation "<<ble<<"\n";
    {
      for(const char *name:{"Public","abcdefghijk"}) {
        auto f=std::make_unique<Fixture>(ble);const auto original=f->s.config;auto a=channel();
        strcpy(a.set_channel.settings.name,name);f->admin(a);
        uint8_t expectedHash=0;for(const char *p=name;*p;++p)expectedHash^=uint8_t(*p);
        for(const auto byte:bench::MtNodeRecord::PublicKey)expectedHash^=byte;
        assert(expectedHash&&f->h.result(0)&&f->h.saves==1&&f->s.config.channelConfigured());
        assert(!strcmp(f->s.config.primaryName(),name)&&f->s.config.node().bytes[4]==expectedHash);
        assert(mt::PrimaryChannel::defaultKey(f->s.config.node())&&f->s.codec.channelHash()==expectedHash);
        assert(f->s.config.role()==original.role()&&f->s.config.rebroadcastMode()==original.rebroadcastMode());
        assert(f->s.config.node().interval()==original.node().interval()&&!f->s.receiving()&&!f->s.endpointArmed());
        assert(!f->radio.starts&&!f->radio.sends);
        PhoneJournal backend;mt::SettingsJournal journal,reboot;
        assert(!journal.load(backend)&&!journal.save(backend,f->s.config)&&backend.writes==1);
        assert(!reboot.load(backend)&&reboot.config().same(f->s.config));
        assert(!reboot.save(backend,reboot.config())&&backend.writes==1);
        f->h.out.clear();f->handshake();unsigned primary=0;
        for(const auto &r:f->h.out)if(r.which_payload_variant==meshtastic_FromRadio_channel_tag&&r.channel.index==0) {
          ++primary;assert(r.channel.role==meshtastic_Channel_Role_PRIMARY&&!strcmp(r.channel.settings.name,name));
          assert(r.channel.settings.psk.size==1&&r.channel.settings.psk.bytes[0]==1);
        }
        assert(primary==1&&f->h.saves==1&&!f->radio.starts&&!f->radio.sends);
      }
    }
    std::cout<<"PASS phone canonical named primary hash/getter/journal roundtrip stays RF-off "<<ble<<"\n";
    {
      auto f=std::make_unique<Fixture>(ble);f->provision(true);const auto original=f->s.config;
      f->input(text());assert(f->s.endpointFree()==3);const auto starts=f->radio.starts,stops=f->radio.stops;
      for(int i=1;i<8;++i) {auto a=channel();a.set_channel.index=i;a.set_channel.role=meshtastic_Channel_Role_DISABLED;
        a.set_channel.settings={};a.set_channel.settings.has_module_settings=true;
        f->h.out.clear();f->admin(a);assert(f->h.result(0)&&!f->h.saves&&f->s.endpointArmed()&&!memcmp(original.bytes,f->s.config.bytes,80));
        assert(f->s.endpointFree()==3&&f->radio.starts==starts&&f->radio.stops==stops);}
      auto a=channel();a.set_channel.index=8;a.set_channel.role=meshtastic_Channel_Role_DISABLED;a.set_channel.settings={};
      f->h.out.clear();f->admin(a);assert(f->h.result(32)&&!f->h.saves);
      a=channel();a.set_channel.index=1;a.set_channel.settings.module_settings.position_precision=33;
      f->h.out.clear();f->admin(a);assert(f->h.result(32)&&!f->h.saves&&!f->radio.sends);
      assert(f->s.config.same(original)&&f->s.endpointFree()==3&&f->s.endpointArmed());
      assert(f->radio.starts==starts&&f->radio.stops==stops);
    }
    std::cout<<"PASS phone disabled tail writes are inert; invalid indexes and invalid promotion precision preserve pending state "<<ble<<"\n";
    {
      auto f=std::make_unique<Fixture>(ble);f->provision(true);const auto original=f->s.config;
      auto a=channel();a.set_channel.index=1;strcpy(a.set_channel.settings.name,"Promoted");f->admin(a);
      assert(f->h.result(0)&&f->h.saves==1&&f->s.config.valid()&&!strcmp(f->s.config.primaryName(),"Promoted"));
      assert(f->s.config.role()==original.role()&&f->s.config.rebroadcastMode()==original.rebroadcastMode()&&
        f->s.config.node().interval()==original.node().interval()&&f->s.config.hopLimit()==original.hopLimit());
      mt::PrimaryChannel::Selection primary,old,displaced;
      assert(f->s.config.channelSelection(0,primary)&&primary.publicDefault&&original.channelSelection(0,old)&&f->s.config.channelSelection(1,displaced));
      assert(old.hash==displaced.hash&&old.length==displaced.length&&!memcmp(old.bytes,displaced.bytes,old.length));
      mt::PrimaryChannel::clear(primary);mt::PrimaryChannel::clear(old);mt::PrimaryChannel::clear(displaced);
      for(unsigned i=2;i<8;++i){mt::ChannelRecord before,after;assert(original.channelRecord(i,before)&&f->s.config.channelRecord(i,after)&&!memcmp(before.bytes,after.bytes,before.Size));}
      PhoneJournal backend;mt::SettingsJournal journal,reboot;assert(!journal.load(backend)&&!journal.save(backend,f->s.config)&&!reboot.load(backend)&&reboot.config().same(f->s.config));
      f->h.out.clear();f->handshake();unsigned promoted=0,former=0;
      for(const auto &r:f->h.out)if(r.which_payload_variant==meshtastic_FromRadio_channel_tag){
        if(r.channel.index==0){++promoted;assert(r.channel.role==meshtastic_Channel_Role_PRIMARY&&!strcmp(r.channel.settings.name,"Promoted"));}
        if(r.channel.index==1){++former;assert(r.channel.role==meshtastic_Channel_Role_SECONDARY);}}
      assert(promoted==1&&former==1&&f->h.saves==1&&!f->radio.sends);
    }
    std::cout<<"PASS phone valid secondary-to-primary promotion swaps the old key, persists and reads back without RF "<<ble<<"\n";
    {
      for(uint32_t from:{0u,OURS,STALE,0xffffffffu}) {
        auto f=std::make_unique<Fixture>(ble);f->provision(true);f->input(text(from));
        assert(!f->h.result(32)&&f->s.endpointFree()==3);
        f->input(text(OURS));assert(!f->h.result(32)&&f->s.endpointFree()==3);
        auto conflict=text(STALE);conflict.packet.decoded.payload.bytes[0]='J';f->input(conflict);
        assert(f->h.result(32)&&f->h.notice("Message ID")&&f->s.endpointFree()==3);
        f->s.poll(300,0);assert(f->radio.sends==1);
        auto &wire=f->radio.frame;bench::RouteMetadata m;
        assert(bench::le32(wire.data()+4)==OURS&&wire[12]==(3|3<<5));
        assert(f->s.codec.decode(wire.data(),wire.size(),m)&&m.portnum==1&&m.payloadLength==5&&!memcmp(m.payload,"Hello",5));
      }
    }
    std::cout<<"PASS phone current or stale sender always uses local wire identity "<<ble<<"\n";
    {
      auto f=std::make_unique<Fixture>(ble);f->provision(false);f->input(text(STALE));
      assert(f->h.result(4)&&f->h.notice("disarmed"));
      auto r=text();r.packet.channel=1;f->h.out.clear();f->input(r);assert(f->h.result(6)&&f->h.notice("disabled or unavailable"));
      r=text();r.packet.decoded.want_response=true;f->h.out.clear();f->input(r);assert(f->h.result(32));
      f->s.poll(300,0);assert(!f->radio.sends);
    }
    std::cout<<"PASS phone disarm and unsupported request reasons remain explicit "<<ble<<"\n";
    {
      for(unsigned reaction:{0u,1u}) {
        auto sender=std::make_unique<Fixture>(ble),receiver=std::make_unique<Fixture>(ble);
        sender->provision(true);receiver->provision(false);
        auto r=text();r.packet.decoded.reply_id=0x1234abcd;r.packet.decoded.emoji=reaction;
        if(reaction) {const uint8_t heart[]={0xe2,0x9d,0xa4,0xef,0xb8,0x8f};r.packet.decoded.payload.size=sizeof(heart);memcpy(r.packet.decoded.payload.bytes,heart,sizeof(heart));}
        sender->input(r);assert(!sender->h.result(32)&&sender->s.endpointFree()==3);
        sender->input(r);assert(sender->s.endpointFree()==3); // exact retry coalesces
        sender->s.poll(300,0);assert(sender->radio.sends==1);
        const auto &wire=sender->radio.frame;bench::RouteMetadata m;
        assert(sender->s.codec.decode(wire.data(),wire.size(),m));
        assert(m.reply_id==0x1234abcd&&m.emoji==reaction&&!m.request_id&&!m.wantResponse);
        assert(m.payloadLength==r.packet.decoded.payload.size&&!memcmp(m.payload,r.packet.decoded.payload.bytes,m.payloadLength));
        // A different local identity receives the real generated Public frame.
        receiver->s.route.configureIdentity(STALE);
        receiver->c.receive(wire.data(),wire.size(),-80,7,500,mt::Companion::WorkspaceProof{true});
        assert(receiver->h.out.back().which_payload_variant==meshtastic_FromRadio_packet_tag);
        const auto &received=receiver->h.out.back().packet;
        assert(received.from==OURS&&received.decoded.reply_id==0x1234abcd&&received.decoded.emoji==reaction);
        assert(received.decoded.payload.size==m.payloadLength&&!memcmp(received.decoded.payload.bytes,m.payload,m.payloadLength));
        // Reply target/reaction are part of nonce-protected message identity.
        r.packet.decoded.reply_id++;sender->input(r);assert(sender->h.result(32)&&sender->radio.sends==1);
      }
    }
    std::cout<<"PASS phone reply and UTF-8 reaction preserve wire data and retry identity "<<ble<<"\n";
    {
      auto f=std::make_unique<Fixture>(ble);f->provision(true);auto r=text();
      r.packet.decoded.reply_id=123;r.packet.decoded.emoji=1;r.packet.decoded.payload.size=sizeof(r.packet.decoded.payload.bytes);
      memset(r.packet.decoded.payload.bytes,'x',r.packet.decoded.payload.size);
      f->input(r);assert(f->h.result(meshtastic_Routing_Error_TOO_LARGE)&&f->s.endpointFree()==4&&!f->radio.sends);
      for(unsigned variant=0;variant<3;++variant) {
        r=text();r.packet.id=ID+variant;
        if(variant==0)r.packet.decoded.want_response=true;
        if(variant==1)r.packet.decoded.request_id=123;
        if(variant==2){r.packet.decoded.xeddsa_signature.size=64;memset(r.packet.decoded.xeddsa_signature.bytes,42,64);}
        f->h.out.clear();f->input(r);assert(f->h.out.back().packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP);
        meshtastic_Routing response{};const auto &d=f->h.out.back().packet.decoded;
        auto b=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&b,meshtastic_Routing_fields,&response));
        assert(d.request_id==r.packet.id&&response.error_reason==meshtastic_Routing_Error_BAD_REQUEST&&f->s.endpointFree()==4);
      }
    }
    std::cout<<"PASS phone metadata overhead bounds and unsupported response/signature refusal "<<ble<<"\n";
    {
      auto f=std::make_unique<Fixture>(ble);f->handshake();unsigned configs=0,modules=0;
      for(const auto &r:f->h.out) {
        configs+=r.which_payload_variant==meshtastic_FromRadio_config_tag;
        modules+=r.which_payload_variant==meshtastic_FromRadio_moduleConfig_tag;
      }
      assert(configs==9&&modules==17); // nine supported Config variants (DeviceUI missing), all ModuleConfig oneofs
      const auto handshake=f->h.out;
      for(const auto &r:handshake) {
        meshtastic_AdminMessage a{};
        if(r.which_payload_variant==meshtastic_FromRadio_config_tag) {
          a.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;
          a.get_config_request=static_cast<meshtastic_AdminMessage_ConfigType>(r.config.which_payload_variant-1);
        } else if(r.which_payload_variant==meshtastic_FromRadio_moduleConfig_tag) {
          a.which_payload_variant=meshtastic_AdminMessage_get_module_config_request_tag;
          a.get_module_config_request=static_cast<meshtastic_AdminMessage_ModuleConfigType>(r.moduleConfig.which_payload_variant-1);
        } else continue;
        f->h.out.clear();f->admin(a);const auto &p=f->h.out.back().packet;
        assert(p.decoded.portnum==meshtastic_PortNum_ADMIN_APP&&p.decoded.request_id==ID);
        meshtastic_AdminMessage got{};auto in=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);
        assert(pb_decode(&in,meshtastic_AdminMessage_fields,&got));
        uint8_t expected[512],actual[512];auto e=pb_ostream_from_buffer(expected,sizeof(expected)),v=pb_ostream_from_buffer(actual,sizeof(actual));
        if(a.which_payload_variant==meshtastic_AdminMessage_get_config_request_tag) {
          assert(got.which_payload_variant==meshtastic_AdminMessage_get_config_response_tag);
          assert(pb_encode(&e,meshtastic_Config_fields,&r.config)&&pb_encode(&v,meshtastic_Config_fields,&got.get_config_response));
        } else {
          assert(got.which_payload_variant==meshtastic_AdminMessage_get_module_config_response_tag);
          assert(pb_encode(&e,meshtastic_ModuleConfig_fields,&r.moduleConfig)&&pb_encode(&v,meshtastic_ModuleConfig_fields,&got.get_module_config_response));
        }
        assert(e.bytes_written==v.bytes_written&&!memcmp(expected,actual,e.bytes_written));
      }
      assert(!f->h.saves&&!f->radio.starts&&!f->radio.sends);
    }
    std::cout<<"PASS phone handshake/get consistency for pinned config and module variants "<<ble<<"\n";
    {
      auto f=std::make_unique<Fixture>(ble);f->provision(true);const auto cfg=f->s.config;
      for(unsigned variant=0;variant<7;++variant) {
        meshtastic_AdminMessage a{};
        if(variant==0){a.which_payload_variant=meshtastic_AdminMessage_shutdown_seconds_tag;a.shutdown_seconds=1;}
        if(variant==1){a.which_payload_variant=meshtastic_AdminMessage_begin_edit_settings_tag;a.begin_edit_settings=true;}
        if(variant==2){a.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;a.get_config_request=meshtastic_AdminMessage_ConfigType_DEVICEUI_CONFIG;}
        if(variant==3){a.which_payload_variant=meshtastic_AdminMessage_get_module_config_request_tag;a.get_module_config_request=static_cast<meshtastic_AdminMessage_ModuleConfigType>(17);}
        if(variant==4){a.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;a.set_owner.public_key.size=32;}
        if(variant==5){a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;a.set_config.which_payload_variant=meshtastic_Config_network_tag;a.set_config.payload_variant.network.wifi_enabled=true;}
        // variant 6 sends a syntactically empty but unsupported admin oneof.
        f->h.out.clear();f->admin(a);assert(f->h.result(32)&&f->h.notice(""));
        assert(!f->h.saves&&f->s.endpointArmed()&&!memcmp(cfg.bytes,f->s.config.bytes,sizeof(cfg.bytes))&&!f->radio.sends);
      }
      f->h.out.clear();auto r=text();r.packet.to=OURS;r.packet.decoded.portnum=meshtastic_PortNum_ADMIN_APP;
      r.packet.decoded.payload.size=1;r.packet.decoded.payload.bytes[0]=0x80;f->input(r);
      assert(f->h.result(32)&&f->h.notice("Malformed")&&!f->h.saves&&f->s.endpointArmed());
    }
    std::cout<<"PASS phone all unsupported/malformed admin failures carry request-associated reason "<<ble<<"\n";
    {
      auto f=std::make_unique<Fixture>(ble);f->provision(true);
      for(uint32_t seconds:{0u,1u,60u,86400u,UINT32_MAX}) {
        meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
        a.set_config.which_payload_variant=meshtastic_Config_display_tag;
        a.set_config.payload_variant.display.screen_on_secs=seconds;a.set_config.payload_variant.display.flip_screen=true;
        a.set_config.payload_variant.display.auto_screen_carousel_secs=seconds==UINT32_MAX?86400:seconds;
        f->h.out.clear();f->admin(a);assert(f->h.result(0));
        assert(f->s.config.displaySeconds()==seconds&&f->s.config.displayFlip()&&
          f->s.config.displayCarouselSeconds()==(seconds==UINT32_MAX?86400:seconds)&&!f->s.endpointArmed()&&f->s.receiving());
        const auto saves=f->h.saves;assert(f->s.armEndpoint(true));f->h.out.clear();f->admin(a);
        assert(f->h.result(0)&&f->h.saves==saves&&f->s.endpointArmed());
        f->h.out.clear();f->handshake();bool found=false;
        for(const auto &r:f->h.out)if(r.which_payload_variant==meshtastic_FromRadio_config_tag&&r.config.which_payload_variant==meshtastic_Config_display_tag) {
          assert(r.config.payload_variant.display.screen_on_secs==seconds&&r.config.payload_variant.display.flip_screen&&
            r.config.payload_variant.display.auto_screen_carousel_secs==(seconds==UINT32_MAX?86400:seconds));found=true;
        }assert(found);
      }
      assert(!f->radio.sends);
    }
    std::cout<<"PASS phone display timeout/flip/carousel get-set agreement and inert exact saves "<<ble<<"\n";
    {
      for(uint8_t model:{uint8_t(81),uint8_t(127)}) {
        auto f=std::make_unique<Fixture>(ble,model);f->provision(true);const auto initial=f->s.config;
        for(unsigned variant=0;variant<4;++variant) {
          meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;a.set_config.which_payload_variant=meshtastic_Config_display_tag;
          auto &d=a.set_config.payload_variant.display;d.screen_on_secs=variant==0?86401:60;
          if(variant==1)d.heading_bold=true;
          if(variant==2)d.wake_on_tap_or_motion=true;
          if(variant==3)d.displaymode=meshtastic_Config_DisplayConfig_DisplayMode_COLOR;
          f->h.out.clear();f->admin(a);assert(f->h.result(32)&&f->h.notice("Display config rejected"));
          assert(f->s.config.same(initial)&&f->s.endpointArmed()&&!f->h.saves);
        }
        if(model==81) {
          meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;a.set_config.which_payload_variant=meshtastic_Config_display_tag;
          a.set_config.payload_variant.display.screen_on_secs=60;f->h.out.clear();f->admin(a);assert(f->h.result(32)&&f->s.config.same(initial));
          a.set_config.payload_variant.display.screen_on_secs=initial.displaySeconds();f->h.out.clear();f->admin(a);assert(f->h.result(0)&&!f->h.saves&&f->s.endpointArmed());
          a.set_config.payload_variant.display.auto_screen_carousel_secs=30;f->h.out.clear();f->admin(a);
          assert(f->h.result(32)&&f->s.config.same(initial)&&!f->h.saves&&f->s.endpointArmed());
        }
      }
    }
    std::cout<<"PASS phone unsupported display fields and display-less writes refuse without mutation "<<ble<<"\n";



  }
}
