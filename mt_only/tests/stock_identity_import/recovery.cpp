// SPDX-License-Identifier: GPL-3.0-only
#include "fixture.h"
#include "device_config_fixture.h"
#include "pki/recovery/PkiIdentityRecovery.h"
#include "stock/admin/DeviceReset.h"

namespace dr = mt::stock::admin::device_reset;

static Memory imported(const Pair &owner, unsigned seed) {
  Memory result;
  Provision setup(result,seed);
  setup.arm();
  assert(!setup.stage(owner) && setup.confirm(owner)>0);
  return result;
}

static IdentityBinding binding(const Pair &p) {
  IdentityBinding b;
  assert(ImportedDomain::binding(p.scalar.data(),p.pub.data(),b));
  return b;
}

struct RecoveryGate final : IdentityRecoveryGate {
  bool identityRecoveryQuiescent()const override{return true;}
};

static int repair(Memory &memory,const Pair &owner) {
  Clock clock;
  Random entropy(9);
  Volume mount(memory);
  PkiStorage live(mount,memory,clock,entropy);
  assert(live.prepare()!=0);
  IdentityRecoveryBootOverlay overlay(mount);
  PkiStorage proof(overlay,memory,clock,entropy);
  RecoveryGate gate;
  PkiIdentityRecovery recovery(memory,clock,gate,live,overlay,proof);
  return recovery.restore(owner.scalar.data());
}

static uint32_t issue(Memory &memory,const IdentityBinding &owner,const Pair &peer,bool pin) {
  Clock clock;
  Volume mount(memory);
  Random entropy(42);
  PkiStorage authority(mount,memory,clock,entropy);
  IdentityBinding current;
  KeyRef key;
  assert(!authority.prepare() && authority.importedIdentity() && !authority.activate(owner.node));
  assert(authority.runtimeIdentity()->binding(current,key));
  PkiPeerPin contact{};
  contact.node=bench::MtSignature::nodeId(peer.pub.data());
  memcpy(contact.publicKey,peer.pub.data(),32);
  if(pin)assert(authority.addTrustedPeer(current,contact)==0);
  PkiPeerSet peers;
  assert(authority.preparedPeers(peers) && peers.count()==1 && peers.find(contact.node));
  uint32_t nonce=0;
  assert(authority.reserve(key,owner.publicKey,owner.node,52,nonce));
  return nonce;
}

struct FloorFlash {
  std::array<uint8_t,dr::Floor::PageBytes> bytes;
  unsigned remaining=UINT32_MAX;
  FloorFlash(){bytes.fill(0xff);}
  int read(size_t at,uint8_t *dst,size_t n){
    assert(at+n<=bytes.size());memcpy(dst,bytes.data()+at,n);return 0;
  }
  int program(size_t at,const uint8_t *src,size_t n){
    assert(at+n<=bytes.size());
    for(size_t i=0;i<n;++i){
      if(!remaining)return -EIO;
      --remaining;
      assert((bytes[at+i]&src[i])==src[i]);
      bytes[at+i]&=src[i];
    }
    return 0;
  }
  int firstEnrollment(bool=false){return 0;}
};

static void checkResetAndRepair(const Pair &p,const Pair &other) {
  Memory original=imported(p,19);
  const IdentityBinding orig=binding(p);
  const uint32_t prior=issue(original,orig,other,true);
  const Memory independent=imported(p,28);
  Memory absent=original;
  absent.rows.erase("mtidentity/state0");
  assert(!absent.rows.count("mtidentity/state1"));
  for(const char *k:{"mtpki/fresh0","mtpki/fresh-pending","mtpki/nonces0","mtpki/nonces1",
                     "mtpki/contacts0","mtpki/contacts1"}){
    for(bool remove:{false,true}){
      Memory mismatch=absent;
      if(remove)mismatch.rows.erase(k);
      else mismatch.rows[k]=independent.rows.at(k);
      const auto before=mismatch.rows;
      assert(repair(mismatch,p)<0 && mismatch.rows==before);
    }
  }
  for(const char *k:{"mtidentity/state0","mtidentity/state1"}){
    Memory corrupt=absent;corrupt.rows[k]={1,2,3};
    const auto before=corrupt.rows;
    assert(repair(corrupt,p)<0 && corrupt.rows==before);
  }
  assert(repair(absent,other)<0);
  const auto persisted=absent.writes;
  assert(!repair(absent,p) && absent.writes==persisted+1 && absent.rows==original.rows);
  assert(issue(absent,orig,other,false)!=prior);
  assert(dr::freshBinding(original,orig,dr::RootUse::RetireCurrent)==0);
  assert(dr::freshBinding(original,orig,dr::RootUse::VerifyReplacement)<0);
  for(const char *k:{"mtpki/fresh0","mtpki/fresh-pending"}){
    Memory bad=original;bad.rows[k]=independent.rows.at(k);
    assert(dr::freshBinding(bad,orig,dr::RootUse::RetireCurrent)<0);
  }
  for(unsigned cut:{1u,47u,48u,69u,95u,96u}){
    Memory active=original;
    FloorFlash floor;floor.remaining=cut;
    Random entropy(73);
    bool attempted=false;
    assert((dr::begin(floor,active,orig,entropy,attempted)==0)==(cut==96));
    assert(attempted);
    floor.remaining=UINT32_MAX;
    dr::Floor::Head head;
    if(cut<96){
      const auto saved=floor.bytes;
      Memory mixed=active;
      mixed.rows["mtpki/fresh-pending"]=independent.rows.at("mtpki/fresh-pending");
      assert(dr::inspect(floor,mixed,head)<0 && floor.bytes==saved);
    }
    assert(!dr::inspect(floor,active,head) && head.state==dr::Floor::State::Selected);
    assert(!dr::rebuild(active,head,[]{return 0;},[]{return 0;},[&](const IdentityBinding &expected){
      Provision next(active,215);
      IdentityBinding candidate;
      if(next.core.stage(candidate) || memcmp(candidate.publicKey,expected.publicKey,32))return -EXDEV;
      return next.core.confirm(expected);
    }));
    Volume mount(active);Clock clock;PkiStorage replacement(mount,active,clock,entropy);
    IdentityBinding next;
    assert(!replacement.prepare() && !replacement.importedIdentity() && replacement.preparedBinding(next));
    assert(!dr::freshBinding(active,next) && !dr::ready(active,head));
    assert(!dr::Floor::activate(floor,head,next.publicKey) && !dr::inspect(floor,active,head));
    assert(head.state==dr::Floor::State::Active && memcmp(head.newKey,orig.publicKey,32));
    assert(dr::ready(original,head)<0 && dr::freshBinding(original,orig)<0);
  }
}

struct HeldHost final: Host {
  Provision &session;
  unsigned calls=0;
  bool floorUnused=true;
  HeldHost(Fixture &fixture,Provision &s):Host(fixture.edit,fixture.journal),session(s){readyFlag=false;}
  bool consoleAllowed()const override{return false;}
  int freshProvision(const uint8_t *data,size_t n,uint32_t epoch,char *out,size_t size)override{
    ++calls;return session.control.command(data,n,epoch,out,size,floorUnused,false,true);
  }
};

struct HeldWire {
  Fixture f{81};Provision session;HeldHost host;
  mt::companion::CompanionWorkspace scratch;mt::Companion companion;unsigned id=950;
  explicit HeldWire(Memory &m,unsigned owner=0):session(m,977),host(f,session),companion(f.service,host,81,scratch,owner){
    meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=618;
    input(hello,owner==0);
  }
  void input(const meshtastic_ToRadio &value,bool serial){
    uint8_t raw[512];auto wire=pb_ostream_from_buffer(raw,sizeof(raw));assert(pb_encode(&wire,meshtastic_ToRadio_fields,&value));
    host.out.clear();
    if(serial){
      auto byte=[&](uint8_t v){companion.byte(v,100,session.clock.epoch,mt::Companion::WorkspaceProof{true});};
      byte(0x94);byte(0xc3);byte(uint8_t(wire.bytes_written>>8));byte(uint8_t(wire.bytes_written));
      for(size_t i=0;i<wire.bytes_written;++i)byte(raw[i]);
    }else companion.packet(raw,wire.bytes_written,100,mt::Companion::WorkspaceProof{true});
    eraseIdentityBytes(raw,sizeof(raw));
  }
  std::string command(const std::string &verb,bool serial=true){
    meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=request.packet;p.id=++id;p.to=Node;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
    const std::string text=std::string(StockImportConsent<Clock>::Prefix)+verb;
    assert(text.size()<sizeof(p.decoded.payload.bytes));p.decoded.payload.size=text.size();
    memcpy(p.decoded.payload.bytes,text.data(),text.size());input(request,serial);
    for(const auto &event:host.out){
      const auto &m=event.packet.decoded;
      if(event.which_payload_variant==meshtastic_FromRadio_packet_tag && m.request_id==p.id &&
         m.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP)
        return {reinterpret_cast<const char*>(m.payload.bytes),m.payload.size};
    }
    return {};
  }
  std::string arm(){
    const auto reply=command("arm");
    const std::string prefix="pki_import_v1 rc=0 armed=1 challenge=";
    return reply.substr(0,prefix.size())==prefix?reply.substr(prefix.size()):"";
  }
  std::string finish(const char *verb,const Pair &p,const std::string &challenge){
    char node[9]{};snprintf(node,sizeof(node),"%08x",bench::MtSignature::nodeId(p.pub.data()));
    return command(std::string(verb)+" "+node+" "+hex(p.pub.data(),32)+" "+challenge);
  }
};

static void checkHeld(const Pair &p,const Pair &foreign){
  Memory expected=imported(p,88);
  for(unsigned step=1;step<=7;++step)for(bool after:{false,true}){
    Memory trial;
    trial.failAt=step;trial.failAfter=after;
    Provision original(trial,88);original.arm();assert(!original.stage(p) && original.confirm(p)<0);
    trial.failAt=0;
    auto wire=std::make_unique<HeldWire>(trial);
    assert(wire->host.out.size()==2 && wire->host.out.front().which_payload_variant==meshtastic_FromRadio_my_info_tag);
    assert(wire->command("capabilities")=="pki_import_v1 profile=mt-zephyr-xiao held=1");
    const auto before=trial.rows;
    assert(wire->command("arm",false).empty());
    wire->host.floorUnused=false;assert(wire->arm().empty());
    wire->host.floorUnused=true;wire->session.clock.entropyAllowed=false;assert(wire->arm().empty());
    assert(trial.rows==before);
    wire->session.clock.entropyAllowed=true;
    const auto challenge=wire->arm();
    if((step==1 && !after)||(step==7 && after)){
      assert(challenge.empty() && trial.rows==before);
      if(step==7)assert(trial.rows==expected.rows);
      continue;
    }
    assert(challenge.size()==32);
    assert(wire->session.stage(foreign)<0 && wire->finish("confirm",p,challenge).empty());
    assert(wire->finish("resume",foreign,challenge).empty());
    const auto next=wire->arm();assert(next.size()==32 && next!=challenge);
    assert(wire->finish("resume",p,challenge).empty());
    const auto timed=wire->arm();wire->session.clock.now+=90000;assert(wire->finish("resume",p,timed).empty());
    assert(trial.rows==before);
    const auto fresh=wire->arm();assert(fresh.size()==32);
    assert(wire->finish("resume",p,fresh)=="pki_import_v1 rc=0 committed=1 reboot=1");
    assert(trial.rows==expected.rows && wire->f.radio.sends==0);
  }
  // A witness removed after the new USB challenge must not be recreated.
  for(const char *k:{"mtpki/fresh-pending","mtpki/fresh0","mtpki/contacts1"}){
    Memory changed=expected;
    changed.rows.erase("mtidentity/state0");
    auto wire=std::make_unique<HeldWire>(changed);
    const auto pending=wire->arm();
    assert(pending.size()==32);
    changed.rows.erase(k);
    const auto before=changed.rows;
    assert(wire->finish("resume",p,pending).empty() && changed.rows==before);
  }
  for(const char *k:{"mtpki/fresh0","mtpki/fresh-pending","mtpki/contacts0","mtpki/nonces0"}){
    Memory bad=expected;bad.rows.erase("mtidentity/state0");bad.rows[k][0]^=1;
    const auto before=bad.rows;auto wire=std::make_unique<HeldWire>(bad);
    assert(wire->arm().empty() && before==bad.rows);
  }
  for(const char *k:{"mtpki/nonces0","mtpki/nonces1","mtpki/contacts0","mtpki/contacts1"}){
    Memory hole=expected;hole.rows.erase("mtidentity/state0");hole.rows.erase(k);
    const auto before=hole.rows;auto wire=std::make_unique<HeldWire>(hole);
    assert(wire->arm().empty() && hole.rows==before);
  }
  Memory advanced=expected;
  (void)issue(advanced,binding(p),foreign,true);
  advanced.rows.erase("mtidentity/state0");const auto prior=advanced.rows;
  auto hold=std::make_unique<HeldWire>(advanced);assert(hold->arm().empty() && advanced.rows==prior);
  for(unsigned owner=1;owner<=3;++owner){
    auto remote=std::make_unique<HeldWire>(advanced,owner);
    assert(remote->command("capabilities",false).empty() && remote->host.calls==0);
  }
}

int main(){
  Pair owner(27),other(47);
  checkResetAndRepair(owner,other);
  puts("PASS imported identity retirement across six floor cuts, native replacement, NVS rollback refusal and existing USB same-scalar repair with contacts and nonce continuity");
  checkHeld(owner,other);
  puts("PASS held serial companion, all fourteen import power cuts, rejected wrong identity, expired and replayed challenges, damaged or out-of-order genesis, and non-serial ingress");
}
