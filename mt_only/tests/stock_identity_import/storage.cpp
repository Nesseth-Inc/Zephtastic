// SPDX-License-Identifier: GPL-3.0-only
#include "fixture.h"
static bool live(Memory &memory,bool imported){
  Volume boot(memory);Clock c;Random rng(47);PkiStorage s(boot,memory,c,rng);
  if(s.prepare())return false;return s.importedIdentity()==imported;
}
static void reserve(Memory &m,const Pair &p,unsigned n,std::set<uint32_t>&out){
  Volume boot(m);Clock c;Random rng(94);PkiStorage s(boot,m,c,rng);assert(s.prepare()==0&&s.importedIdentity());
  IdentityBinding b;KeyRef r;assert(s.preparedBinding(b)&&!memcmp(b.publicKey,p.pub.data(),32));
  PkiPeerSet pins;assert(s.preparedPeers(pins)&&pins.count()==0);
  assert(s.activate(b.node)==0&&s.runtimeIdentity()->binding(b,r));
  for(unsigned i=0;i<n;i++){uint32_t extra=0;assert(s.reserve(r,b.publicKey,b.node,7,extra)&&out.insert(extra).second);}
  assert(s.close());
}
static void checkUsbClientStatusContract(){
  Memory memory;
  Provision session(memory, 89);
  Pair candidate(34);
  session.arm();
  std::string status;
  assert(session.command("status",status,false)>0);
  assert(status=="pki_import_v1 rc=0 armed=1 staged=0 node=00000000 pub= ble_eligible=0");
  assert(session.command("status",status)>0);
  assert(status=="pki_import_v1 rc=0 armed=1 staged=0 node=00000000 pub= ble_eligible=1");
  assert(session.stage(candidate)==0);
  char node[9]{};
  snprintf(node,sizeof(node),"%08x",bench::MtSignature::nodeId(candidate.pub.data()));
  assert(session.command("status",status)>0);
  assert(status==std::string("pki_import_v1 rc=0 armed=1 staged=1 node=")+node+
    " pub="+hex(candidate.pub.data(),candidate.pub.size())+" ble_eligible=1");
}

struct ContactReadFault : Memory {
  enum class Failure { Io, BadSize, CorruptRow };
  const char *key = nullptr;
  unsigned reads = 0, failOn = 0;
  Failure failure = Failure::Io;
  bool injected = false;
  int read(const char *name,uint8_t *out,size_t capacity) override {
    if(key && !strcmp(key,name) && ++reads==failOn){
      injected=true;
      assert(capacity==PkiContactStore::RecordBytes);
      if(failure==Failure::CorruptRow){
        rows.at(name)[0]^=1;
      }else{
        // NvsBootSettings zeroes the destination and returns -EILSEQ for
        // wrong-size NVS records; genuine backend failures retain their errno.
        memset(out,0,capacity);
        if(failure==Failure::Io)return -EIO;
        rows.at(name).pop_back();
        return -EILSEQ;
      }
    }
    const int count=Memory::read(name,out,capacity);
    if(count>=0 && !strncmp(name,"mtpki/contacts",14) && count!=int(capacity)){
      memset(out,0,capacity);
      return -EILSEQ;
    }
    return count;
  }
};

static void checkContactReadFailures(){
  const Pair pair(78);
  Memory complete;
  Provision first(complete,17);
  first.arm();
  assert(!first.stage(pair) && first.confirm(pair)>0);
  for(unsigned slot=0;slot<2;++slot){
    const char *key=PkiContactStore::key(slot);
    for(auto kind:{ContactReadFault::Failure::Io,ContactReadFault::Failure::BadSize,
                   ContactReadFault::Failure::CorruptRow}){
      for(unsigned pass:{3u,4u}){
        ContactReadFault trial;
        trial.rows=complete.rows;
        trial.rows.erase("mtidentity/state0");
        trial.rows.erase(PkiFreshProvisioning::RootKey);
        // Cover an I/O error while checking a truly missing row and a final
        // error in the first row after the second has just been persisted.
        if((pass==3 && slot==1 && kind==ContactReadFault::Failure::Io) || (pass==4 && slot==0))
          trial.rows.erase(PkiContactStore::key(1));
        Provision resume(trial,43);
        resume.arm();assert(!resume.stage(pair));
        trial.key=key;trial.failOn=pass;trial.failure=kind;
        const int rc=resume.confirm(pair);
        assert(trial.injected && rc==(kind==ContactReadFault::Failure::Io?-EIO:-EILSEQ));
        assert(!trial.rows.count(PkiFreshProvisioning::RootKey) && !trial.rows.count("mtidentity/state0"));
        trial.key=nullptr;
        Provision retry(trial,71);
        std::string response;
        if(kind!=ContactReadFault::Failure::Io){
          const auto before=trial.rows;
          assert(retry.command("arm",response,false,true)<0 && before==trial.rows);
          continue;
        }
        retry.arm(true);
        char node[9];snprintf(node,sizeof(node),"%08x",bench::MtSignature::nodeId(pair.pub.data()));
        assert(retry.command(std::string("resume ")+node+" "+hex(pair.pub.data(),32)+" "+retry.challenge,
                             response,false,true)>0);
        assert(trial.rows==complete.rows && live(trial,true));
      }
    }
  }
  puts("PASS contact completion and final reads reject I/O, wrong size and corrupt bytes in both slots; clean USB retry retains the installation");
}

int main(){
  checkContactReadFailures();
  checkUsbClientStatusContract();
  Pair p(17),other(35);
  Memory complete;
  {
    Provision tx(complete,13);std::string reply;
    assert(tx.stage(p)<0&&complete.writes==0);
    tx.arm();assert(tx.command("status",reply)>0&&reply.find(tx.challenge)==std::string::npos);
    assert(tx.stage(p)==0&&tx.stage(other)<0&&complete.writes==0);
    assert(tx.confirm(other)<0&&tx.stage(p)<0&&complete.writes==0);
    tx.arm();tx.clock.now+=90000;assert(tx.stage(p)<0&&complete.writes==0);
    tx.arm();assert(tx.stage(p,false)==0);++tx.clock.ble;assert(tx.confirm(p)<0&&complete.writes==0);
    tx.arm();assert(tx.stage(p)==0);++tx.clock.epoch;assert(tx.confirm(p)<0&&complete.writes==0);
    tx.arm();assert(tx.stage(p)==0);tx.challenge[0]=tx.challenge[0]=='0'?'1':'0';assert(tx.confirm(p)<0&&complete.writes==0);
    tx.arm();uint8_t zero[32]{};assert(tx.control.stage(zero,p.pub.data(),true,tx.clock.epoch)<0&&complete.writes==0);
    tx.arm();assert(tx.control.stage(p.scalar.data(),other.pub.data(),true,tx.clock.epoch)<0&&complete.writes==0);
    tx.arm();assert(tx.control.stage(p.scalar.data(),zero,true,tx.clock.epoch)<0&&complete.writes==0);
    tx.arm();auto raw=p.scalar;raw[0]^=3;raw[31]^=128;
    assert(tx.control.stage(raw.data(),p.pub.data(),false,tx.clock.ble)==0&&complete.writes==0);
    assert(tx.confirm(p)>0&&complete.writes==7);
  }
  assert(live(complete,true));
  {
    PkiFreshRoot ordinary;auto root=complete.rows.at("mtpki/fresh0");
    assert(!PkiFreshRoot::decode(root.data(),root.size(),ordinary));
    IdentityBinding b;uint8_t scratch[32]{};MonocypherIdentityDeriver d;const auto &pending=complete.rows.at("mtpki/fresh-pending");
    assert(!PkiFreshIntent::decode(pending.data(),pending.size(),b,scratch,d));
    auto before=complete.rows;Provision again(complete,13);again.arm();assert(again.stage(p)<0&&again.stage(other)<0&&complete.rows==before);
  }
  std::set<uint32_t> issued;reserve(complete,p,80,issued);reserve(complete,p,320,issued);assert(issued.size()==400);
  Memory independent;Provision next(independent,371);next.arm();assert(next.stage(p)==0&&next.confirm(p)>0&&live(independent,true));
  assert(complete.rows.at("mtpki/fresh0")!=independent.rows.at("mtpki/fresh0"));
  {
    // Independent Bouncy Castle 1.83 RFC 7748, keyed BLAKE2b and FF1 inverse
    // for the p/seed-371 fixture. Advance synthetic checkpoints to its block.
    constexpr uint32_t zeroPreimage=0xcc6cd43f;
    constexpr uint32_t through=(zeroPreimage-1)&~255u;
    Memory closeToZero=independent;
    for(const char *key:{"mtpki/nonces0","mtpki/nonces1"}){
      auto &record=closeToZero.rows.at(key);
      PkiFreshRoot::put(record.data()+52,through);PkiFreshRoot::put(record.data()+56,through/256+1);
      PkiFreshRoot::put(record.data()+60,PkiFreshRoot::checksum(record.data(),60));
    }
    std::set<uint32_t> values;
    reserve(closeToZero,p,zeroPreimage-through,values);assert(values.count(0)==1);
  }
  for(const char*key:{"mtpki/fresh0","mtpki/fresh-pending","mtpki/nonces0","mtpki/nonces1","mtpki/contacts0","mtpki/contacts1"}){
    Memory m=independent;m.rows.at(key)=complete.rows.at(key);assert(!live(m,true));
    m=independent;m.rows.erase(key);assert(!live(m,true));
    m=independent;m.rows.at(key)[0]^=1;assert(!live(m,true));
  }
  for(unsigned cut=1;cut<=7;++cut)for(bool after:{false,true}){
    Memory m;m.failAt=cut;m.failAfter=after;Provision first(m,77);first.arm();assert(first.stage(p)==0&&first.confirm(p)<0);
    if(after&&cut==7){assert(live(m,true));continue;}
    assert(!live(m,true));const auto witness=m.rows;const auto writes=m.writes;
    m.failAt=0;
    if(!witness.empty()){
      Provision foreign(m,93);foreign.arm();assert(foreign.stage(other)<0&&m.writes==writes);
      Volume v(m);Clock c;Random r(555);PkiFreshProvisioning ordinary(v,c,r);IdentityBinding x;assert(ordinary.stage(x)<0&&m.writes==writes);
    }
    Provision retry(m,91);retry.arm();assert(retry.stage(p)==0&&retry.confirm(p)>0&&live(m,true));
    if(!witness.empty())assert(witness.at("mtpki/fresh-pending")==m.rows.at("mtpki/fresh-pending"));
  }
  {
    Memory m;Provision old(m,3);IdentityBinding b;assert(old.core.stage(b)==0&&old.core.confirm(b)==0&&live(m,false));
    const auto oldRows=m.rows;Provision attempt(m,7);attempt.arm();assert(attempt.stage(p)<0&&m.rows==oldRows);
  }
  puts("PASS consent, protected identity, X25519 binding, occupied/corrupt/mixed stores, 14 interrupted writes, cold reopen, 400 distinct nonce extras at one caller ID");
}
