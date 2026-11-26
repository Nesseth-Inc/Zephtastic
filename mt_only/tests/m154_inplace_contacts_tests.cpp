// SPDX-License-Identifier: GPL-3.0-only
// Synthetic settings only; actual NVS and production owner are separate checks.
#define main m154_preserved_boot_component_main
#include "m154_preserved_bounded_boot_tests.cpp"
#undef main
#include "pki/PkiInplaceContacts.h"

namespace {
struct FaultSettings final:mt::SettingsBackend {
  Values rows;unsigned writes=0;int failAt=-1,phase=0;
  int read(const char *name,uint8_t *out,size_t cap)override{
    const auto it=rows.find(name);if(it==rows.end())return -ENOENT;
    if(it->second.size()>cap)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());return int(it->second.size());
  }
  int write(const char *name,const uint8_t *data,size_t size)override{
    ++writes;const bool fault=int(writes)==failAt;
    if(fault&&phase==0)return -EIO;
    rows[name]=Bytes(data,data+(fault&&phase==1?size/2:size));
    return fault?-EIO:0;
  }
};
void fixedUnchanged(const Values &before,const Values &after){
  for(const auto &row:before)if(row.first!="mtpki/ledger0"&&row.first!="mtpki/ledger1")
    assert(after.at(row.first)==row.second);
  assert(before.size()==after.size());
}
}
int main(){
  Durable durable;const Bytes flash=durable.nor.bytes;
  PkiPeerPin expected;expected.node=durable.later.binding.node;
  memcpy(expected.publicKey,durable.later.binding.publicKey,32);
  for(unsigned swap=0;swap<2;++swap){
    Values original=durable.settings.rows;
    if(swap)std::swap(original["mtpki/ledger0"],original["mtpki/ledger1"]);
    {
      FaultSettings backend;backend.rows=original;
      PkiPeerPin caller=expected;
      PkiInplaceContacts contacts(backend,durable.ledger);
      assert(contacts.load(durable.local.binding,durable.anchor,&caller,1)==0);
      assert(contacts.state()==PkiInplaceContacts::State::Legacy&&backend.writes==0);
      // Migration uses the validated list captured by load, never a later
      // caller-provided replacement or an alias into a mutable array.
      caller.node=0;memset(caller.publicKey,0,32);
      assert(contacts.migrate()==0&&contacts.ready()&&backend.writes==3);
      PkiContactSnapshot::Value value;assert(contacts.snapshot(value));
      assert(value.generation==2&&value.count==1&&value.contacts[0].node==expected.node);
      fixedUnchanged(original,backend.rows);
      PkiInplaceContacts reopened(backend,durable.ledger);
      assert(reopened.load(durable.local.binding,durable.anchor,&expected,1)==0&&reopened.ready());
      assert(reopened.replace(nullptr,0)==0&&reopened.snapshot(value)&&value.generation==3&&value.count==0);
      PkiInplaceContacts revoked(backend,durable.ledger);
      assert(revoked.load(durable.local.binding,durable.anchor,&expected,1)==0&&revoked.ready());
      assert(revoked.snapshot(value)&&value.count==0&&value.generation==3);
      assert(revoked.replace(&expected,1)==0&&revoked.snapshot(value)&&value.generation==4&&value.count==1);
      fixedUnchanged(original,backend.rows);
    }
    for(int write=1;write<=3;++write)for(int phase=0;phase<3;++phase){
      FaultSettings backend;backend.rows=original;backend.failAt=write;backend.phase=phase;
      PkiInplaceContacts attempt(backend,durable.ledger);
      assert(attempt.load(durable.local.binding,durable.anchor,&expected,1)==0);
      assert(attempt.migrate()==-EIO&&attempt.state()==PkiInplaceContacts::State::Frozen);
      fixedUnchanged(original,backend.rows);backend.failAt=-1;
      const unsigned before=backend.writes;
      PkiInplaceContacts reboot(backend,durable.ledger);
      const int rc=reboot.load(durable.local.binding,durable.anchor,&expected,1);
      assert(backend.writes==before); // Startup never resumes a migration.
      if(phase==1){assert(rc<0&&reboot.state()==PkiInplaceContacts::State::Frozen);}
      else {
        assert(rc==0);
        if(!reboot.ready())assert(reboot.migrate()==0&&reboot.ready());
        PkiContactSnapshot::Value value;assert(reboot.snapshot(value)&&value.generation==2&&value.count==1);
      }
      fixedUnchanged(original,backend.rows);assert(durable.nor.bytes==flash);
    }
    {
      FaultSettings backend;backend.rows=original;backend.rows.erase("mtpki/ledger0");
      PkiInplaceContacts missing(backend,durable.ledger);
      assert(missing.load(durable.local.binding,durable.anchor,&expected,1)<0&&backend.writes==0);
    }
  }
  puts("PASS_M154_INPLACE_CONTACTS_PRESERVED_HISTORY_AND_CUT_STATES");return 0;
}
