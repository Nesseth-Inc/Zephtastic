// SPDX-License-Identifier: GPL-3.0-only
#include "NodeJournal.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using Cache=mt::NodeCache;
using Entry=Cache::Entry;
using Op=Cache::Edit;
using Journal=mt::NodeJournal;
using Bytes=std::vector<uint8_t>;
constexpr unsigned Self=81,First=5000;
struct Native:mt::SettingsBackend {
    Bytes slots[2];int failAfter=-1;bool failCommitted=false;
    static unsigned slot(const char *name){
        if(!strcmp(name,"mtbase/nodes0"))return 0;
        assert(!strcmp(name,"mtbase/nodes1"));return 1;
    }
    int read(const char *key,uint8_t *p,size_t n)override{
        auto &r=slots[slot(key)];if(r.empty())return -ENOENT;
        if(r.size()>n)return -E2BIG;memcpy(p,r.data(),r.size());return int(r.size());
    }
    int write(const char *key,const uint8_t *p,size_t n)override{
        const bool fail=failAfter==0;if(failAfter>0)--failAfter;
        if(fail&&!failCommitted)return -ENOSPC;
        slots[slot(key)].assign(p,p+n);return fail?-EIO:0;
    }
    void save(const char *dir,const char *phase){
        for(int i=0;i<2;++i){std::ofstream out(std::string(dir)+"/"+phase+char('0'+i),std::ios::binary);
            out.write(reinterpret_cast<const char*>(slots[i].data()),slots[i].size());assert(out);}
    }
    void load(const char *dir,const char *phase){
        for(int i=0;i<2;++i){std::ifstream in(std::string(dir)+"/"+phase+char('0'+i),std::ios::binary);
            assert(in);slots[i]=Bytes(std::istreambuf_iterator<char>(in),{});}
    }
};
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
#include "Files.h"
struct Application {
    Cache hot{};Journal journal{};
    int boot(Native &n,Files &f){journal.files(f);return journal.load(n,hot,Self);}
    int edit(Native &n,unsigned id,Op op){return journal.edit(n,hot,id,op,99,true);}
};
static Entry contact(unsigned id){Entry e;e.identity(id,81,2);e.bytes[Entry::KeyLength]=32;
    memset(e.bytes+Entry::Key,0x24,32);assert(Entry::text(e.bytes+Entry::Name,40,"Owner test",11));return e;}
static void fillLegacy(Application &a,Native &n,Files &f){
    assert(!a.boot(n,f));
    a.journal.enableReplayEpochs(a.hot);
    for(unsigned i=0;i<14;++i)assert(!a.edit(n,First+i,Op::Ignore));
    assert(!a.hot.expanded()&&a.hot.protectedCount()==14&&!f.writes);
}
int main(int argc,char **argv){
    assert(argc==2);Native native;Files fs;Application a;fillLegacy(a,native,fs);
    native.save(argv[1],"legacy");
    // Repeated opportunistic RF discoveries never trigger migration.
    for(unsigned i=0;i<70;++i)assert(a.hot.accept(contact(7000+i),Self,-3,1)>=0);
    assert(a.hot.count()==16&&!fs.writes);
    fs.enabled=false;assert(a.edit(native,First+14,Op::Ignore)==-ENOSPC&&!a.hot.expanded());
    fs.enabled=true;assert(!a.edit(native,First+14,Op::Ignore));
    assert(a.hot.expanded()&&a.hot.protectedCount()==15&&fs.writes==1);
    native.save(argv[1],"active");
    assert(!a.edit(native,First+15,Op::Ignore));assert(fs.nonces==2);
    for(unsigned i=16;i<197;++i)assert(!a.edit(native,First+i,Op::Ignore));
    auto manual=contact(First+197);Entry checked;
    assert(!a.journal.previewOwnerContact(a.hot,manual,false,true,checked));
    assert(checked.favorite()&&checked.manuallyVerified());
    assert(!a.journal.ownerContact(native,a.hot,manual,false,true,99,true));
    assert(a.hot.protectedCount()==198&&a.hot.count()==199);
    assert(a.edit(native,First+198,Op::Ignore)==-ENOSPC);
    assert(a.hot.accept(contact(9000),Self,0,0)>=0);
    assert(a.hot.accept(contact(9001),Self,0,0)>=0&&a.hot.find(9000)<0);
    assert(!a.edit(native,First+196,Op::Favorite));
    Application recovered;assert(!recovered.boot(native,fs));assert(recovered.hot.protectedCount()==198);
    for(unsigned i=0;i<197;++i)assert(recovered.hot.ignored(First+i));
    auto high=recovered.hot.find(First+197);assert(high>=0);
    auto p=recovered.hot.get(high);assert(p->favorite()&&p->manuallyVerified()&&p->bytes[Entry::KeyLength]==32);
    assert(recovered.hot.replayEpoch(high));manual.bytes[Entry::Key]^=1;
    assert(recovered.hot.permits(manual,Self,1)==-EACCES);
    auto missing=fs;missing.enabled=false;Application withheld;assert(withheld.boot(native,missing)<0&&!withheld.hot.ready());
    auto other=fs;other.slots[0][12]^=1;assert(withheld.boot(native,other)<0&&!withheld.hot.ready());
    // A failed atomic stage leaves both policy and the current readback authoritative.
    fs.error=-ENOSPC;assert(recovered.edit(native,First+100,Op::Remove)==-ENOSPC&&recovered.hot.ignored(First+100)&&recovered.hot.ready());
    assert(recovered.journal.verifiedCapacityRefusal(-ENOSPC,recovered.hot));
    assert(!recovered.journal.verifiedCapacityRefusal(-EIO,recovered.hot));
    fs.error=0;
    auto uncertainFull=fs;uncertainFull.error=-ENOSPC;uncertainFull.afterRename=true;
    Application afterFull;assert(!afterFull.boot(native,uncertainFull));
    assert(afterFull.edit(native,First+100,Op::Remove)==-ENOSPC&&!afterFull.hot.ready());
    assert(!afterFull.journal.verifiedCapacityRefusal(-ENOSPC,afterFull.hot));
    uncertainFull.error=0;assert(!afterFull.boot(native,uncertainFull)&&!afterFull.hot.ignored(First+100));
    auto after=fs;after.error=-EIO;after.afterRename=true;Application uncertain;
    assert(!uncertain.boot(native,after));assert(uncertain.edit(native,First+100,Op::Remove)==-EIO&&!uncertain.hot.ready());
    after.error=0;assert(!uncertain.boot(native,after));assert(!uncertain.hot.ignored(First+100)&&uncertain.hot.ignored(First+101));
    for(unsigned i=197;i>=15;--i)assert(!recovered.edit(native,First+i,Op::Remove));
    assert(recovered.hot.protectedCount()==15&&recovered.hot.count()==16);
    auto cuts=native;auto cutFs=fs;Application interrupted;assert(!interrupted.boot(cuts,cutFs));
    cuts.failAfter=1;assert(interrupted.edit(cuts,First+14,Op::Remove)<0&&!interrupted.hot.ready());
    cuts.failAfter=-1;assert(!interrupted.boot(cuts,cutFs));assert(!interrupted.hot.expanded()&&interrupted.hot.dirty());
    assert(interrupted.journal.flush(cuts,interrupted.hot,100,true,true)==1);
    assert(cuts.slots[0].size()==Journal::MaxRecordSize&&cuts.slots[1].size()==Journal::MaxRecordSize);
    assert(!recovered.edit(native,First+14,Op::Remove));
    assert(!recovered.hot.expanded()&&recovered.hot.protectedCount()==14&&fs.slots[0].empty()&&fs.slots[1].empty());
    native.save(argv[1],"restored");
    // A reported post-commit error freezes the cache. Reboot selects the
    // complete new generation; the next admin edit does not reuse obsolete policy.
    Native side;Files sideFs;Application sideApp;fillLegacy(sideApp,side,sideFs);
    side.failAfter=0;side.failCommitted=true;assert(sideApp.edit(side,First+14,Op::Ignore)<0&&!sideApp.hot.ready());
    side.failAfter=-1;assert(!sideApp.boot(side,sideFs)&&sideApp.hot.ignored(First+14));
    puts("PASS 198 distinct protected peers, one evictable, high-slot key/epoch, failed stage and post-commit recovery, two-selector downgrade");
}
#else
int main(int argc,char **argv){
    assert(argc==2);Native n;Cache hot;Journal j;n.load(argv[1],"legacy");
    assert(!j.load(n,hot,Self)&&hot.protectedCount()==14);
    n.load(argv[1],"active");assert(j.load(n,hot,Self)<0&&!hot.ready());
    n.load(argv[1],"restored");assert(!j.load(n,hot,Self)&&hot.protectedCount()==14);
    for(unsigned i=0;i<14;++i)assert(hot.ignored(First+i));
    puts("PASS actual pre-migration firmware journal declines active pointers and opens the verified downgrade");
}
#endif
