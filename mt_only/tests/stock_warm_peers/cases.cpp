// SPDX-License-Identifier: GPL-3.0-only
#include "companion/WarmPeerOwner.h"
#include "companion/PeerDirectoryOwner.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#define WARM_NODE_COUNT 100
#define LOG_MIGRATION(...) ((void)0)
#define MESHTASTIC_NODEDB_MIGRATION_VERBOSE 0
#define NODEINFO_BITFIELD_IS_FAVORITE_MASK 1
#define NODEINFO_BITFIELD_IS_IGNORED_MASK 2
#define NODEINFO_BITFIELD_IS_KEY_MANUALLY_VERIFIED_MASK 16
#define IS_ONE_OF(x,a,b,c) ((x)==(a)||(x)==(b)||(x)==(c))
namespace oracle {
using NodeNum=uint32_t;
struct meshtastic_NodeInfoLite{uint8_t role,bitfield;bool verified;};
enum{meshtastic_Config_DeviceConfig_Role_TRACKER=5,meshtastic_Config_DeviceConfig_Role_SENSOR=6,meshtastic_Config_DeviceConfig_Role_TAK_TRACKER=10};
static bool nodeInfoLiteHasXeddsaSigned(const meshtastic_NodeInfoLite *n){return n->verified;}
namespace memaudit{void set(const char*,size_t){}}
#include "oracle.inc"
}
using namespace mt::companion;
using E=mt::NodeCache::Entry;
#include "Map.h"
static WarmPeerDomain domain(){WarmPeerDomain d{};d.self=198;d.owner[0]=1;d.epoch[0]=2;d.channels[0]=3;d.directory[0]=4;return d;}
static WarmPeerRecord peer(uint32_t n,uint32_t utc=999,uint8_t key=1){
    WarmPeerRecord r{};r.id=n;r.packed=WarmPeerRecord::pack(utc,n%13,n%4,n&1);memset(r.key,key,32);return r;
}
template<class Ledger,class IO> static int apply(Ledger &w,IO &io,typename Ledger::Action a,WarmPeerRecord r,WarmPeerRecord &out){
    int rc=w.begin(a,r);assert(!rc);do{const auto count=io.reads;rc=w.step(io,out);assert(io.reads==count+1);}while(rc==-EINPROGRESS);
    return rc;
}
static void stock(){
    using L=WarmPeerLedger<100>;L w;Map m;WarmPeerRecord out{};auto d=domain();assert(!w.open(m,d,0));
    oracle::WarmNodeStore o;uint32_t state=1234567;
    for(unsigned i=0;i<2500;i++){
        state=state*1664525u+1013904223u;const uint32_t id=1000+(state%185);
        const uint32_t utc=(state>>24)*128+(i%128);auto r=peer(id,utc,uint8_t((state>>13)%3));
        const bool take=(state>>27)%3==0;
        if(take){
            oracle::WarmNodeEntry original{};bool found=o.take(id,original);
            assert((!apply(w,m,L::Action::Take,r,out))==found);
            if(found)assert(!memcmp(&original,&out,40));
        }else{
            bool accepted=o.absorb(id,utc,r.key,r.role(),r.category(),r.signedBefore());
            assert((!apply(w,m,L::Action::Demote,r,out))==accepted);
        }
    }
    for(unsigned id=1000;id<1185;id++){
        oracle::WarmNodeEntry r{};const bool have=o.take(id,r);
        assert((!apply(w,m,L::Action::Take,peer(id),out))==have);
        if(have)assert(!memcmp(&r,&out,40));
    }
    for(unsigned role=0;role<=12;role++)for(unsigned f=0;f<64;f++)for(unsigned t:{0u,126u,127u,128u,UINT32_MAX}){
        E e;e.identity(550,2,role);e.bytes[E::LocalFlags]=f;auto r=warmDemotion(e.bytes,t);
        oracle::meshtastic_NodeInfoLite lite{uint8_t(role),uint8_t(f),bool(f&E::VerifiedSigner)};
        const auto cat=oracle::warmProtectedCategory(lite);assert(r.packed==oracle::warmPackLastHeard(t,role,cat,lite.verified));
    }
    puts("PASS exact pinned absorb/place/take, quantized LRU ties and NodeDB role/protection/signer packing");
}
template<unsigned Cap> static void full(){
    using L=WarmPeerLedger<Cap>;L w;Map m;auto d=domain();assert(!w.open(m,d,0));WarmPeerRecord out;
    for(unsigned i=0;i<Cap;i++)assert(!apply(w,m,L::Action::Demote,peer(200+i,1000),out));
    assert(m.files.size()==L::Pages+1);
    assert(apply(w,m,L::Action::Demote,peer(3000,1500,0),out)==-ENOSPC);
    assert(!apply(w,m,L::Action::Demote,peer(3000,1500,2),out));
    assert(apply(w,m,L::Action::Take,peer(200),out)==-ENOENT);
    assert(!apply(w,m,L::Action::Take,peer(201),out)&&out.id==201);
    printf("PASS %u rows in %u pages plus one manifest; keyed-only full, exact lowest-slot tie victim\n",Cap,L::Pages);
}
static void resetAndRecovery(){
    using L=WarmPeerLedger<100>;L w;Map m;auto d=domain();WarmPeerRecord out{};
    assert(!w.open(m,d,0));assert(!apply(w,m,L::Action::Demote,peer(501,1023),out));
    auto snapshot=m;
    for(bool after:{false,true}){
        Map cut=snapshot;L old;assert(!old.open(cut,d,0));cut.fail=true;cut.after=after;
        out={};assert(apply(old,cut,L::Action::Take,peer(501),out)==-EIO&&!out.id);
        cut.fail=false;L next;assert(!next.open(cut,d,0));
        assert((!apply(next,cut,L::Action::Take,peer(501),out))!=after);
    }
    for(unsigned field=0;field<5;field++){
        Map cut=snapshot;L next;auto changed=d;
        if(field==0)changed.self++;if(field==1)changed.epoch[0]++;if(field==2)changed.owner[0]++;
        if(field==3)changed.channels[0]++;if(field==4)changed.directory[0]++;
        assert(!next.open(cut,changed,0)&&apply(next,cut,L::Action::Take,peer(501),out)==-ENOENT);
        assert(!next.open(cut,d,0)&&apply(next,cut,L::Action::Take,peer(501),out)==-ENOENT);
    }
    m.files[0].back()^=1;assert(apply(w,m,L::Action::Take,peer(501),out)==-EILSEQ&&!w.ready());
    puts("PASS failed/uncertain consumption has no output; reboot selects complete state, corruption and all domain changes fail closed");
}
#include "companion/WarmPeerVolume.h"
#include <lfs.h>
lfs_t *warmFs;
struct Device{
    std::vector<uint8_t> &bytes; lfs_config c{};lfs_t fs{};int mutations=0,failAt=-1;bool torn=false;
    Device(std::vector<uint8_t> &media,bool format=false):bytes(media){
        c.context=this;c.block_size=4096;c.block_count=media.size()/4096;c.block_cycles=512;
        c.read_size=16;c.prog_size=16;c.cache_size=64;c.lookahead_size=16;
        c.read=[](const lfs_config *c,lfs_block_t b,lfs_off_t off,void *out,lfs_size_t n){
            auto &d=*static_cast<Device*>(c->context);memcpy(out,d.bytes.data()+b*4096+off,n);return 0;};
        c.prog=[](const lfs_config *c,lfs_block_t b,lfs_off_t off,const void *in,lfs_size_t n){
            auto &d=*static_cast<Device*>(c->context);unsigned wrote=d.available(n);auto p=static_cast<const uint8_t*>(in);
            for(unsigned i=0;i<wrote;i++){auto &v=d.bytes[b*4096+off+i];assert((v&p[i])==p[i]);v=p[i];}return wrote==n?0:-EIO;};
        c.erase=[](const lfs_config *c,lfs_block_t b){auto &d=*static_cast<Device*>(c->context);unsigned n=d.available(4096);
            memset(d.bytes.data()+b*4096,255,n);return n==4096?0:-EIO;};
        c.sync=[](const lfs_config*){return 0;};
        if(format)assert(!lfs_format(&fs,&c));
        assert(!lfs_mount(&fs,&c));warmFs=&fs;
    }
    ~Device(){assert(!lfs_unmount(&fs));}
    unsigned available(unsigned n){const int at=mutations++;return failAt<0||at<failAt?n:torn&&at==failAt?n/2:0;}
};
struct CountIO:WarmPeerVolume{
    unsigned reads=0;int get(int p,void *out,size_t n){++reads;return WarmPeerVolume::get(p,out,n);}
};
static void power(){
    using L=WarmPeerLedger<100>;WarmPeerRecord out{};
    auto d=domain();std::vector<uint8_t> old(19*4096,255);
    {Device dev(old,true);CountIO io;L w;assert(!w.open(io,d,0));
     assert(!apply(w,io,L::Action::Demote,peer(501,1023),out));
     assert(!apply(w,io,L::Action::Demote,peer(502,2047),out));}
    int operations=0;
    {auto full=old;Device dev(full);CountIO io;L w;assert(!w.open(io,d,0));dev.mutations=0;
     assert(!apply(w,io,L::Action::Forget,peer(501),out));assert(!w.acknowledge(io,1));operations=dev.mutations;}
    for(int cut=0;cut<=operations;cut++)for(bool torn:{false,true}){
        auto media=old;
        {Device dev(media);CountIO io;L w;assert(!w.open(io,d,0));dev.mutations=0;dev.failAt=cut;dev.torn=torn;
         if(!apply(w,io,L::Action::Forget,peer(501),out))(void)w.acknowledge(io,1);}
        {Device dev(media);CountIO io;L w;assert(!w.open(io,d,1));
         if(w.serial()==0){int rc=apply(w,io,L::Action::Forget,peer(501),out);assert(!rc||rc==-ENOENT);assert(!w.acknowledge(io,1));}
         assert(apply(w,io,L::Action::Inspect,peer(501),out)==-ENOENT);
         assert(!apply(w,io,L::Action::Inspect,peer(502),out)&&out.lastHeard()==1920);}
    }
    printf("PASS all %d actual program/erase cuts including torn NOR writes, targeted revoke preserves unrelated peer\n",2*(operations+1));
}
static void lifecycle(){
    using L=WarmPeerLedger<100>;using D=PeerDirectory<2>;D directory;Map thin,warm;
    D::Domain thinDomain{};thinDomain.node=198;thinDomain.key[0]=1;thinDomain.channels[0]=2;thinDomain.epoch[0]=3;
    assert(!directory.open(thin,thinDomain,4));
    PeerDirectoryOwner<2,2> owner;Map staged;
    assert(owner.refusesUnsignedIdentity(800));
    assert(!owner.open(staged,thinDomain,4));
    E signedEntry{};signedEntry.identity(800,1,0);signedEntry.bytes[E::LocalFlags]=E::VerifiedSigner;
    owner.observe(signedEntry);
    assert(owner.refusesUnsignedIdentity(800)&&!owner.refusesUnsignedIdentity(801));
    assert(!owner.poll(staged,4)&&owner.refusesUnsignedIdentity(800));
    auto entry=[](uint32_t id,uint32_t role,uint8_t key){
        E e;e.identity(id,1,role);e.bytes[E::KeyLength]=32;memset(e.bytes+E::Key,key,32);
        e.bytes[E::LocalFlags]=E::VerifiedSigner;E::put32(e.lastHeardBytes,751);
        return D::Row::copy(e);
    };
    D::Row evicted{};assert(!directory.save(thin,entry(501,5,2),&evicted)&&!evicted.id());
    assert(!directory.save(thin,entry(502,6,4),&evicted)&&!evicted.id());
    thin.fail=true;assert(directory.save(thin,entry(503,0,1),&evicted)==-EIO&&!evicted.id());thin.fail=false;
    assert(!directory.open(thin,thinDomain,4));
    assert(!directory.pendingDemotion().id());
    assert(!directory.save(thin,entry(503,0,1),&evicted)&&evicted.id()==501);
    D restart;assert(!restart.open(thin,thinDomain,4)&&restart.pendingDemotion().id()==501);
    WarmPeerOwner<100> o;o.bind(restart.warmDomain(),restart.withdrawals());
    auto pending=restart.pendingDemotion();o.demote(pending.user,E::get32(pending.utc));
    for(unsigned i=0;i<8&&!o.absorbed();i++)assert(!o.poll(warm));assert(o.absorbed());assert(!restart.finishDemotion(thin));o.acknowledged();
    assert(restart.refusesUnsignedIdentity(503)&&!restart.refusesUnsignedIdentity(990));
    uint8_t role=55;const auto reads=warm.reads;assert(!o.positionRole(501,role)&&role==55&&warm.reads==reads);
    for(int i=0;i<6;i++)assert(!o.poll(warm));assert(o.positionRole(501,role)&&role==5);
    assert(o.refusesUnsignedIdentity(501));assert(o.refusesUnsignedIdentity(990));
    for(int i=0;i<6;i++)assert(!o.poll(warm));assert(!o.refusesUnsignedIdentity(990));
    L ledger;auto d=restart.warmDomain();assert(!ledger.open(warm,d,0));WarmPeerRecord out{};
    assert(!apply(ledger,warm,L::Action::Inspect,peer(501),out)&&out.signedBefore()&&out.category()==3);
    assert(!apply(ledger,warm,L::Action::Promote,peer(501),out)&&out.signedBefore());
    L cold;assert(!cold.open(warm,d,0)&&!apply(cold,warm,L::Action::Inspect,peer(501),out)&&out.signedBefore());
    assert(!apply(ledger,warm,L::Action::Demote,peer(515),out));
    assert(!restart.forget(thin,501));o.bind(d,restart.withdrawals());
    assert(!o.positionRole(501,role));for(int i=0;i<12;i++)assert(!o.poll(warm));assert(!o.positionRole(501,role));
    assert(!ledger.open(warm,d,1)&&ledger.serial()==1);
    assert(apply(ledger,warm,L::Action::Inspect,peer(501),out)==-ENOENT);
    assert(!apply(ledger,warm,L::Action::Inspect,peer(515),out));
    for(unsigned i=1;i<=5;i++)assert(!restart.forget(thin,600+i));
    o.bind(d,restart.withdrawals());for(int i=0;i<10;i++)assert(!o.poll(warm));
    assert(!ledger.open(warm,d,6)&&apply(ledger,warm,L::Action::Inspect,peer(515),out)==-ENOENT);
    puts("PASS real thin eviction durable handoff, off-lock retained descriptive role, targeted remove and ring overrun reset");
}
static void migration(){
    using D=PeerDirectory<2>;Map io;
    D dir;D::Domain d{};d.node=55;d.epoch[0]=1;d.key[0]=2;
    assert(!dir.open(io,d,2));E one;one.identity(552,1,5);assert(!dir.save(io,D::Row::copy(one)));
    // V2 was identical up to the incarnation offset, followed by a CRC32.
    // Independently lay its stable on-disk header out rather than importing
    // mutable source from an unpublished Git branch.
    struct Old{uint32_t magic,version,before,after,count;D::Domain domain;uint32_t ids[2],pages[1];uint8_t order[4],withdrawn[4];uint32_t crc;};
    Old old{};old.magic=0x3144505a;old.version=2;old.before=old.after=2;old.count=1;old.domain=d;old.ids[0]=552;old.pages[0]=1;
    const auto bytes=reinterpret_cast<const uint8_t*>(&old);uint32_t crc=UINT32_MAX;
    for(size_t i=0;i<offsetof(Old,crc);i++){crc^=bytes[i];for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);}
    old.crc=~crc;assert(!io.put(-1,&old,sizeof(old)));
    D migrated;assert(!migrated.open(io,d,2)&&migrated.eligible(552));D::Row row{};assert(!migrated.read(io,552,row)&&row.id()==552);
    assert(migrated.warmDomain().valid());
    puts("PASS original V2 phone directory upgrades without losing existing records");
}
template<unsigned Peers,unsigned Warm,unsigned Blocks> static void usable(const char *board){
    using D=PeerDirectory<Peers,Blocks==19?32:16>;using L=WarmPeerLedger<Warm>;
    std::vector<uint8_t> media(Blocks*4096,255);Device dev(media,true);
    PeerDirectoryVolume thin;CountIO warm;D index;typename D::Domain td{};td.node=198;td.key[0]=2;td.epoch[0]=3;
    assert(!index.open(thin,td,0));L ledger;assert(!ledger.open(warm,index.warmDomain(),0));
    for(unsigned i=0;i<Peers;i++){E e;e.identity(200+i,1,0);assert(!index.save(thin,D::Row::copy(e)));}
    // Generate each real indexed page through the firmware ledger, then install
    // this same image on native LittleFS before exercising its COW paths.
    WarmPeerRecord out{};Map generated;L writer;assert(!writer.open(generated,index.warmDomain(),0));
    for(unsigned i=0;i<Warm;i++)assert(!apply(writer,generated,L::Action::Demote,peer(5000+i),out));
    for(const auto &item:generated.files)assert(!warm.put(item.first,item.second.data(),item.second.size()));
    assert(!ledger.open(warm,index.warmDomain(),0));
    assert(!apply(ledger,warm,L::Action::Inspect,peer(4999+Warm),out)&&out.id==4999+Warm);
    std::vector<uint8_t> snapshot(7368,1);
    auto fill=[&](const char *name){fs_file_t file{};assert(!fs_open(&file,name,FS_O_CREATE|FS_O_TRUNC|FS_O_WRITE));
        assert(fs_write(&file,snapshot.data(),snapshot.size())==ssize_t(snapshot.size()));assert(!fs_sync(&file)&&!fs_close(&file));};
    fill("/zt-history/.zt-sat-v1");fill("/zt-history/.zt-sat-v1.tmp");assert(!fs_rename("/zt-history/.zt-sat-v1.tmp","/zt-history/.zt-sat-v1"));
    // Rotate one COW hot page, one warm page and their manifests under peak load.
    E fresh;fresh.identity(200,7,0);assert(!index.save(thin,D::Row::copy(fresh)));
    if constexpr(Warm==100)assert(!apply(ledger,warm,L::Action::Demote,peer(5000,12345),out));
    else assert(!apply(ledger,warm,L::Action::Take,peer(5000),out));
    printf("PASS %s simultaneous thin and warm page files, two 7368-byte history generations, %d of %u blocks allocated after COW\n",board,int(lfs_fs_size(&dev.fs)),Blocks);
}

int main(){migration();usable<119,100,19>("T096");usable<199,2000,128>("XIAO");stock();full<100>();full<2000>();resetAndRecovery();lifecycle();power();}
