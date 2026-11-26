// SPDX-License-Identifier: GPL-3.0-only
// Exact in-place ledger lengths accepted by the NVS boot adapter. The adapter
// admits known sizes only; PkiStorage performs all record/state validation.
#define main m154_unused_contact_scan_cases_main
#include "m154_nvs_contact_scan_cases.cpp"
#undef main

#include "pki/PkiContactMigrationMarker.h"
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace {
using namespace mt::pki;

static void setLedgerLength(std::vector<Expected> &rows,const char *key,size_t size,uint8_t seed){
  const auto it=std::find_if(rows.begin(),rows.end(),[key](const Expected &row){return row.name==key;});
  assert(it!=rows.end());it->bytes=pattern(size,seed);
}

static void acceptedInPlaceLengths(Fixture &fixture){
  for(const char *key:{"mtpki/ledger0","mtpki/ledger1"}){
    for(const size_t size:{size_t(PkiContactMigrationMarker::RecordBytes),
                           size_t(PkiContactSnapshot::RecordBytes)}){
      auto rows=buildRows(fixture);
      const auto it=std::find_if(rows.begin(),rows.end(),[key](const Expected &row){return row.name==key;});
      assert(it!=rows.end());
      const size_t seed=size==PkiContactMigrationMarker::RecordBytes?0x91:0xa2;
      setLedgerLength(rows,key,size,uint8_t(seed));populate(rows);

      NvsBootSettings boot;assert(boot.begin()==0);
      // The original key's caller capacity remains the old 1944-byte maximum;
      // the adapter returns the actual shorter value length without decoding it.
      std::array<uint8_t,PkiLedger::RecordSize> output;output.fill(0x5a);
      assert(boot.read(key,output.data(),output.size())==int(size));
      const auto &source=expected(rows,key);
      assert(std::equal(source.bytes.begin(),source.bytes.end(),output.begin()));
      assert(boot.read(key,output.data(),output.size()-1)==-EACCES);
      boot.end();
    }
  }
  puts("PASS original ledger names accept exact 64-byte marker and 640-byte contact values with 1944-byte read capacity");
}

static void rejectNearbyInPlaceLengths(Fixture &fixture){
  for(const char *key:{"mtpki/ledger0","mtpki/ledger1"}){
    for(const size_t size:{size_t(63),size_t(65),size_t(639),size_t(641)}){
      auto rows=buildRows(fixture);setLedgerLength(rows,key,size,0xb3);populate(rows);
      readonlyRejected(-EILSEQ);
    }
  }
  puts("PASS ledger marker/contact adjacent lengths 63/65/639/641 fail closed without writes");
}
}

int main(){
  Fixture fixture;
  acceptedInPlaceLengths(fixture);
  rejectNearbyInPlaceLengths(fixture);
  return 0;
}
