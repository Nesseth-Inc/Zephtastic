// SPDX-License-Identifier: GPL-3.0-only
// Built twice. The V24 reader includes the unchanged historical headers.
#include "SettingsJournal.h"
#include <cassert>
#include <fstream>
#include <map>
#include <string>
#include <vector>
struct Store:mt::SettingsBackend{
  using Bytes=std::vector<uint8_t>;
  std::map<std::string,Bytes> rows;
  unsigned writes=0,cut=0;
  bool after=false;
  int read(const char *key,uint8_t *out,size_t size)override{
    auto v=rows.find(key);if(v==rows.end())return -ENOENT;
    assert(size>=v->second.size());memcpy(out,v->second.data(),v->second.size());return int(v->second.size());
  }
  int write(const char *key,const uint8_t *p,size_t size)override{
    ++writes;if(cut==writes&&!after)return -EIO;
    rows[key]=Bytes(p,p+size);return writes==cut?-EIO:0;
  }
};
static const char *key(unsigned slot){return slot?"mtbase/state1":"mtbase/state0";}
int main(int argc,char **argv){
  assert(argc==3);const std::string path=argv[1];const unsigned last=unsigned(std::stoul(argv[2]));
#if defined(CONFIG_MT_LOCAL_CLOCK)
  Store store;mt::SettingsJournal journal;assert(!journal.load(store));mt::Config c=journal.config();
  assert(c.bytes[2]==24);
  uint32_t ignored[]={0xabcdefff};assert(c.setIgnoredSenders(ignored,1));
  for(unsigned gen=1;gen<=last;++gen){
    assert(c.setNodeInfoSeconds(3600+gen));
    if(gen==3){char zone[65]="NPT-5:45";assert(c.setTimezone(zone));}
    if(gen==5){char empty[65]{};assert(c.setTimezone(empty));}
    if(gen==5){
      // Primary/refusal and error-after-commit at either write. A reboot
      // resumes only the verified legacy fallback; both groups stay ordered.
      for(unsigned write=1;write<=2;++write)for(bool after:{false,true}){
        Store fail=store;auto writer=journal;fail.cut=fail.writes+write;fail.after=after;
        assert(writer.save(fail,c)==-EIO);fail.cut=0;
        mt::SettingsJournal recovered;assert(!recovered.load(fail));
        const bool committed=write>1||after;
        assert(recovered.config().bytes[2]==(committed?24:25));
        assert(recovered.generation()==(committed?gen:gen-1));
        for(unsigned i=0;i<2;++i)assert(fail.rows.at(key(i))[14]==(committed?24:25));
      }
    }
    assert(!journal.save(store,c));
    assert(journal.config().bytes[2]==((gen==3||gen==4)?25:24));
  }
  for(unsigned i=0;i<2;++i){
    const auto &raw=store.rows.at(key(i));std::ofstream file(path+char('0'+i),std::ios::binary);
    assert(file.write(reinterpret_cast<const char*>(raw.data()),raw.size()));
  }
#else
  static_assert(mt::Config::Version==24,"requires the pinned old firmware's Config and SettingsJournal");
  for(unsigned mask:{1u,2u,3u}){
    Store store;
    for(unsigned i=0;i<2;++i)if(mask&(1u<<i)){
      std::ifstream file(path+char('0'+i),std::ios::binary);
      assert(file);store.rows[key(i)]={std::istreambuf_iterator<char>(file),{}};
    }
    mt::SettingsJournal j;assert(!j.load(store)&&j.ready()&&!store.writes);
    const unsigned slot=mask==3?(last-1)%2:mask-1;
    const unsigned expected=(last-1)%2==slot?last:last-1;
    assert(j.generation()==expected&&j.activeSlot()==int(slot)&&j.config().bytes[2]==24);
    assert(j.channelGeneration()==0&&j.config().node().interval()==3600+expected&&j.config().ignoresIncoming(0xabcdefff));
  }
#endif
}
