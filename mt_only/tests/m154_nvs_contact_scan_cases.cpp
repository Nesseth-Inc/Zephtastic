// SPDX-License-Identifier: GPL-3.0-only
// Read-only M154 contact-key scan tests using actual pinned Zephyr NVS/settings
// code over the pki_storage_nvs synthetic NOR bridge.
#define main m154_unused_existing_nvs_cases_main
#include "cases.cpp"
#undef main

#include "pki/PkiContactSnapshot.h"
#include "bridge.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace mt::pki;
using Bytes=std::vector<uint8_t>;
struct Expected {std::string name;Bytes bytes;unsigned slot;};
static const Expected &expected(const std::vector<Expected> &rows,const char *name){
  const auto it=std::find_if(rows.begin(),rows.end(),[name](const Expected &r){return r.name==name;});
  assert(it!=rows.end());return *it;
}

static Bytes pattern(size_t size,uint8_t seed){Bytes out(size);for(size_t i=0;i<size;++i)out[i]=uint8_t(seed+29u*i);return out;}
static std::vector<Expected> buildRows(Fixture &f,int badContactSlot=-1,size_t badSize=0,
                                      bool omitContacts=false){
  std::vector<Expected> rows;
  const struct {const char *name;size_t size;uint8_t seed;unsigned slot;} old[]={
    {"mtidentity/state0",IdentityJournal::RecordSize,0x10,1},
    {"mtidentity/state1",IdentityJournal::RecordSize,0x20,2},
    {"mtpki/ledger0",PkiLedger::RecordSize,0x30,3},
    {"mtpki/ledger1",PkiLedger::RecordSize,0x40,5},
    {"mtpki/nonces0",PkiNonceCounter::RecordBytes,0x50,6},
    {"mtpki/nonces1",PkiNonceCounter::RecordBytes,0x60,7},
  };
  for(const auto &row:old)rows.push_back({row.name,pattern(row.size,row.seed),row.slot});

  PkiPeerPin peer{};peer.node=f.peer.binding.node;memcpy(peer.publicKey,f.peer.binding.publicKey,32);
  Bytes contact0(PkiContactSnapshot::RecordBytes),contact1(PkiContactSnapshot::RecordBytes);
  assert(PkiContactSnapshot::encode(f.local.binding,1,1,0x1234abcd,&peer,1,
                                   contact0.data(),contact0.size()));
  assert(PkiContactSnapshot::encode(f.local.binding,2,1,0x1234abcd,&peer,1,
                                   contact1.data(),contact1.size()));
  if(!omitContacts){
    if(badContactSlot==0)contact0.resize(badSize,0xa5);
    rows.push_back({"mtpki/contacts0",std::move(contact0),8});
    rows.push_back({"fixture/unknown",pattern(3,0x70),10});
    if(badContactSlot==1)contact1.resize(badSize,0xa6);
    rows.push_back({"mtpki/contacts1",std::move(contact1),12});
  }else rows.push_back({"fixture/unknown",pattern(3,0x70),8});
  return rows;
}
static void populate(const std::vector<Expected> &rows){
  model_reset(4);
  for(const auto &row:rows)rawrow(row.slot,row.name.data(),row.name.size(),row.bytes.data(),row.bytes.size());
  model_remount();
}
static int nameIdFor(const char *key){
  for(unsigned slot=1;slot<=NvsBootSettings::NameLimit;++slot){
    char name[NvsBootSettings::NameBytes+1]{};
    const int n=int(model_get(uint16_t(0x8000+slot),name,sizeof(name)-1));
    if(n<=0)continue;name[n]=0;if(!strcmp(name,key))return int(0x8000+slot);
  }
  return -1;
}
static void assertScanAndReads(Fixture &f){
  auto rows=buildRows(f);populate(rows);
  std::array<uint8_t,32768> before{},after{};model_copy_flash(before.data());
  const unsigned writes=model_writes(),reads=model_reads();
  NvsBootSettings boot;assert(boot.begin()==0);
  const auto d=boot.diagnostic();
  assert(d.stage==9&&d.lastNameId==0x800c&&d.names==9);
  constexpr uint32_t valueBytes=2*IdentityJournal::RecordSize+2*PkiLedger::RecordSize+
      2*PkiNonceCounter::RecordBytes+2*PkiContactSnapshot::RecordBytes+3;
  assert(d.valueBytes==valueBytes);
  assert(model_reads()-reads==1+2*NvsBootSettings::NameLimit);

  // These six key lookups preserve their pre-M154 index/order and exact sizes.
  for(unsigned i=0;i<6;++i){
    const auto &row=rows[i];Bytes out(row.bytes.size(),0xa5);
    assert(boot.read(row.name.c_str(),out.data(),out.size())==int(out.size()));
    assert(out==row.bytes);
  }
  for(const char *key:{"mtpki/contacts0","mtpki/contacts1"}){
    const auto &row=expected(rows,key);Bytes out(PkiContactSnapshot::RecordBytes,0x5a);
    assert(boot.read(row.name.c_str(),out.data(),out.size())==int(out.size()));
    assert(out==row.bytes);
    PkiContactSnapshot::Value decoded{};
    assert(PkiContactSnapshot::decode(out.data(),out.size(),f.local.binding,1,0x1234abcd,decoded));
    assert(decoded.count==1&&decoded.contacts[0].node==f.peer.binding.node);
  }
  std::array<uint8_t,PkiContactSnapshot::RecordBytes> contactOut{};
  assert(boot.read("mtpki/contacts0",contactOut.data(),contactOut.size()-1)==-EACCES);
  std::array<uint8_t,PkiContactSnapshot::RecordBytes+1> oversized{};
  assert(boot.read("mtpki/contacts1",oversized.data(),oversized.size())==-EACCES);
  assert(boot.read("fixture/unknown",contactOut.data(),contactOut.size())==-EACCES);
  boot.end();model_copy_flash(after.data());
  assert(before==after&&writes==model_writes());
  puts("PASS eight known names, old-six order, valid contact records, holes, unknown-name read denial, exact read-only scan");
}
static void assertBadContactLengths(Fixture &f){
  for(int slot=0;slot<2;++slot)for(size_t size:{size_t(639),size_t(641)}){
    populate(buildRows(f,slot,size));readonlyRejected(-EILSEQ);
  }
  puts("PASS contact slot lengths 639/641 refused without repair or writes");
}
static void assertLegacyLedgerSizesRemainSupported(Fixture &f){
  for(const char *key:{"mtpki/ledger0","mtpki/ledger1"}){
    auto rows=buildRows(f);
    auto it=std::find_if(rows.begin(),rows.end(),[key](const Expected &r){return r.name==key;});
    assert(it!=rows.end());it->bytes=pattern(PkiLedger::LegacyRecordSize,0x83);
    populate(rows);const unsigned writes=model_writes();NvsBootSettings boot;assert(boot.begin()==0);
    std::array<uint8_t,PkiLedger::RecordSize> out;out.fill(0xa5);
    assert(boot.read(key,out.data(),out.size())==int(PkiLedger::LegacyRecordSize));
    assert(std::equal(it->bytes.begin(),it->bytes.end(),out.begin()));
    const uint32_t expectedBytes=2*IdentityJournal::RecordSize+PkiLedger::RecordSize+
        PkiLedger::LegacyRecordSize+2*PkiNonceCounter::RecordBytes+
        2*PkiContactSnapshot::RecordBytes+3;
    assert(boot.diagnostic().valueBytes==expectedBytes);
    boot.end();assert(model_writes()==writes);
  }
  puts("PASS historical legacy-ledger record size remains supported in either original ledger slot");
}
static void assertPartialLedgerRowsRefused(Fixture &f){
  const auto rows=buildRows(f);populate(rows);const int ledgerName=nameIdFor("mtpki/ledger0");assert(ledgerName>0);
  for(int removeName=0;removeName<2;++removeName){
    populate(rows);const int deleted=model_delete(uint16_t(ledgerName+(removeName?0:0x4000)));
    assert(deleted==0);model_remount();readonlyRejected(-EILSEQ);
  }
  puts("PASS partial legacy ledger name/value tombstones remain fail-closed");
}
static void assertAbsentContactsRemainAbsent(Fixture &f){
  auto rows=buildRows(f,-1,0,true);populate(rows);
  const unsigned writes=model_writes();NvsBootSettings boot;assert(boot.begin()==0);
  std::array<uint8_t,PkiContactSnapshot::RecordBytes> out;out.fill(0xa5);
  assert(boot.read("mtpki/contacts0",out.data(),out.size())==-ENOENT);
  assert(boot.read("mtpki/contacts1",out.data(),out.size())==-ENOENT);
  for(uint8_t b:out)assert(b==0);
  boot.end();assert(model_writes()==writes);
  puts("PASS both supported contact keys may be absent and read as ENOENT");
}
}

int main(){
  Fixture f;
  assertScanAndReads(f);
  assertBadContactLengths(f);
  assertLegacyLedgerSizesRemainSupported(f);
  assertPartialLedgerRowsRefused(f);
  assertAbsentContactsRemainAbsent(f);
  return 0;
}
