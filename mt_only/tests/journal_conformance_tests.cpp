// SPDX-License-Identifier: GPL-3.0-only
// Independent persistence contract tests; entirely memory-backed.
#include "SettingsJournal.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
struct Backend:mt::SettingsBackend {
  std::map<std::string,std::vector<uint8_t>> files;
  bool errorAfterCommit=false;unsigned writes=0;
  int read(const char *key,uint8_t *p,size_t capacity)override {
    auto i=files.find(key);if(i==files.end())return -ENOENT;
    if(i->second.size()>capacity)return -E2BIG;
    memcpy(p,i->second.data(),i->second.size());return i->second.size();
  }
  int write(const char *key,const uint8_t *p,size_t n)override {
    ++writes;files[key]={p,p+n};return errorAfterCommit?-EIO:0;
  }
};
static mt::Config named(const char *name) {
  mt::Config c;auto node=c.node();assert(node.setName(name,false));c.node(node);return c;
}
static bool committed_error_cannot_ack_false_rollback() {
  Backend b;mt::SettingsJournal j;const auto a=named("Before"),z=named("Uncertain");
  assert(!j.load(b)&&!j.save(b,a));b.errorAfterCommit=true;
  assert(j.save(b,z)==-EIO);b.errorAfterCommit=false;
  // Refusal is safe; acknowledged restoration must survive a reboot.
  const int restored=j.save(b,a);
  mt::SettingsJournal reboot;assert(!reboot.load(b));
  return restored!=0||reboot.config().same(a);
}
static bool failed_write_retains_verified_ram() {
  Backend b;mt::SettingsJournal j;const auto a=named("Before"),z=named("Uncertain");
  assert(!j.load(b)&&!j.save(b,a));b.errorAfterCommit=true;
  assert(j.save(b,z)==-EIO);
  return j.config().same(a);
}
static bool reload_resolves_uncertain_commit() {
  Backend b;mt::SettingsJournal j;const auto a=named("Before"),z=named("Uncertain");
  assert(!j.load(b)&&!j.save(b,a));b.errorAfterCommit=true;
  assert(j.save(b,z)==-EIO);b.errorAfterCommit=false;
  assert(!j.load(b));if(!j.config().same(z))return false;
  if(j.save(b,a))return false;
  mt::SettingsJournal reboot;
  return !reboot.load(b)&&reboot.config().same(a);
}
static bool invalid_save_has_no_side_effect() {
  Backend b;mt::SettingsJournal j;assert(!j.load(b));
  auto invalid=j.config();invalid.bytes[4]=2;invalid.seal();
  const auto saved=j.config();const auto writes=b.writes;
  return j.save(b,invalid)==-EINVAL&&j.config().same(saved)&&b.writes==writes;
}
int main() {
  struct Test {const char *name;bool (*run)();};
  const Test tests[]={
    {"committed_error_cannot_ack_false_rollback",committed_error_cannot_ack_false_rollback},
    {"failed_write_retains_verified_ram",failed_write_retains_verified_ram},
    {"reload_resolves_uncertain_commit",reload_resolves_uncertain_commit},
    {"invalid_save_has_no_side_effect",invalid_save_has_no_side_effect}
  };
  unsigned failed=0;
  for(const auto &test:tests){const bool ok=test.run();failed+=!ok;printf("%s %s\n",ok?"PASS":"FAIL",test.name);}
  return failed?1:0;
}
