// SPDX-License-Identifier: GPL-3.0-only
// Exercise the real legacy ledger's one-way selector, with synthetic RAM data.
#define main original_ledger_main
#include "pki_ledger_tests.cpp"
#undef main

int main(){
  Fixture local(11),remote(97);Memory initial=initialized(local);
  {PkiLedger l;CHECK(!l.load(initial));
    CHECK(!l.addTrustedPeer(initial,remote.binding.node,remote.binding.publicKey,true));
    CHECK(!l.reserve(initial,817,true));
    CHECK(!l.commitAuthenticated(initial,remote.binding.node,remote.binding.publicKey,918,true));}
  PkiLedger base;CHECK(!base.load(initial));PkiLedger::Status status;CHECK(base.status(status));
  uint8_t crcBytes[4]{};CHECK(base.snapshotRead(PkiLedger::RecordSize-4,crcBytes,4));
  AppendAnchor anchor{status.generation,get32(crcBytes),0,172032};
  {Memory m=initial;PkiLedger l;CHECK(!l.load(m));const auto before=m.values;const auto writes=m.writes;
    CHECK(l.installAppendAnchor(m,anchor,false)==-EAGAIN);
    for(unsigned kind=0;kind<3;++kind){auto wrong=anchor;
      if(kind==0)++wrong.generation;
      if(kind==1)wrong.snapshotCrc^=1;
      if(kind==2)wrong.capacity-=4096;
      CHECK(l.installAppendAnchor(m,wrong,true)==-EINVAL);}
    CHECK(m.values==before&&m.writes==writes&&l.ready());}
  Memory enabled=initial;PkiLedger l;CHECK(!l.load(enabled));
  CHECK(!l.installAppendAnchor(enabled,anchor,true));const auto stable=enabled.values;const auto writes=enabled.writes;
  AppendAnchor got;CHECK(l.appendAnchor(got)&&got.rootCrc==0&&got.snapshotCrc==anchor.snapshotCrc);
  CHECK(l.reserve(enabled,819,true)==-EACCES);
  CHECK(l.addTrustedPeer(enabled,remote.binding.node,remote.binding.publicKey,true)==-EACCES);
  CHECK(l.revokeTrustedPeer(enabled,remote.binding.node,remote.binding.publicKey,true)==-EACCES);
  CHECK(l.commitAuthenticated(enabled,remote.binding.node,remote.binding.publicKey,920,true)==-EACCES);
  CHECK(l.installAppendAnchor(enabled,anchor,true)==-EACCES);
  CHECK(enabled.values==stable&&enabled.writes==writes&&l.sent(817));
  CHECK(l.seen(remote.binding.node,remote.binding.publicKey,918)==ReplayCheck::Seen);
  {PkiLedger reload;CHECK(!reload.load(enabled)&&reload.appendAnchor(got));CHECK(reload.sent(817));
    uint8_t sentinel[8];memset(sentinel,0x57,sizeof(sentinel));
    CHECK(!reload.snapshotRead(PkiLedger::RecordSize-1,sentinel,2)&&sentinel[0]==0x57);
    CHECK(!reload.snapshotRead(SIZE_MAX,sentinel,1)&&sentinel[0]==0x57);}
  std::string selector,baseline;
  for(const auto &entry:enabled.values)if(entry.first.rfind("mtpki/ledger",0)==0){
    if(!memcmp(entry.second.data(),"ZTPA",4))selector=entry.first;else baseline=entry.first;}
  CHECK(!selector.empty()&&!baseline.empty()&&initial.values.at(baseline)==enabled.values.at(baseline));
  for(size_t cut=0;cut<PkiLedger::RecordSize;++cut){
    Memory m=initial;PkiLedger current;CHECK(!current.load(m));m.cut=int(cut);
    CHECK(current.installAppendAnchor(m,anchor,true)<0&&!current.ready());
    m.cut=-1;PkiLedger reboot;CHECK(reboot.load(m)<0&&!reboot.ready());++cuts;
  }
  for(size_t byte=0;byte<PkiLedger::RecordSize;++byte)for(unsigned bit=0;bit<8;++bit){
    Memory m=enabled;m.values[selector][byte]^=uint8_t(1u<<bit);PkiLedger reboot;
    CHECK(reboot.load(m)<0&&!reboot.ready());++bits;
  }
  {Memory m=enabled;m.values.erase(baseline);PkiLedger reboot;CHECK(reboot.load(m)<0&&!reboot.ready());}
  {Memory m=enabled;m.values[baseline]=m.values[selector];PkiLedger reboot;CHECK(reboot.load(m)<0&&!reboot.ready());}
  {Memory m=initial;PkiLedger current;CHECK(!current.load(m));m.alterReadback=true;
    CHECK(current.installAppendAnchor(m,anchor,true)<0&&!current.ready());
    m.alterReadback=false;PkiLedger reboot;CHECK(!reboot.load(m)&&reboot.appendAnchor(got));}
  std::printf("{\"PASS\":true,\"assertions\":%u,\"selector_cut_points\":%u,\"corrupt_selector_bits\":%u,\"hardware\":false}\n",checks,cuts,bits);
}
