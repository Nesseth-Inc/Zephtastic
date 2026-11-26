// SPDX-License-Identifier: GPL-3.0-only
#include "companion/SatellitePages.h"
#include "companion/PeerDirectory.h"
#include <cassert>
#include <map>
#include <vector>
#include <cstdio>
#include "../stock_warm_peers/Map.h"
#include "companion/WarmPeerVolume.h"
#include "companion/SatellitePageVolume.h"
using namespace mt::companion;
lfs_t *warmFs;
struct Device{
    std::vector<uint8_t> &bytes; lfs_config c{};lfs_t fs{};int mutations=0,failAt=-1;bool torn=false;
    Device(std::vector<uint8_t> &media,bool format=false,unsigned cache=64):bytes(media){
        c.context=this;c.block_size=4096;c.block_count=media.size()/4096;c.block_cycles=512;
        c.read_size=16;c.prog_size=16;c.cache_size=cache;c.lookahead_size=16;
        c.read=[](const lfs_config *c,lfs_block_t b,lfs_off_t off,void *out,lfs_size_t n){
            auto &d=*static_cast<Device*>(c->context);memcpy(out,d.bytes.data()+b*4096+off,n);return 0;};
        c.prog=[](const lfs_config *c,lfs_block_t b,lfs_off_t off,const void *in,lfs_size_t n){
            auto &d=*static_cast<Device*>(c->context);unsigned wrote=d.available(n);auto p=static_cast<const uint8_t*>(in);
            for(unsigned i=0;i<wrote;i++){auto &v=d.bytes[b*4096+off+i];assert((v&p[i])==p[i]);v=p[i];}return wrote==n?0:-EIO;};
        c.erase=[](const lfs_config *c,lfs_block_t b){auto &d=*static_cast<Device*>(c->context);unsigned n=d.available(4096);
            memset(d.bytes.data()+b*4096,255,n);return n==4096?0:-EIO;};
        c.sync=[](const lfs_config*){return 0;};
        if(format)assert(!lfs_format(&fs,&c));
        assert(!lfs_mount(&fs,&c));warmFs=&fs;zt_lfs_health=ZT_LFS_OK;
    }
    ~Device(){if(zt_lfs_health!=ZT_LFS_DETACHED)assert(!lfs_unmount(&fs));}
    unsigned available(unsigned n){const int at=mutations++;return failAt<0||at<failAt?n:torn&&at==failAt?n/2:0;}
};
using Reports=SatellitePageVolume;
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
#include "companion/LargeNodeVolume.h"
struct Mounted {
    bool mounted()const{return true;}
    int mount(bool format,bool preparePublic){assert(!format&&!preparePublic);return 0;}
};
struct LargeFixture {
    Mounted mount;LargeNodeVolume<Mounted> files{mount};
    mt::LargeNodeFiles::Token tokens[2]{};
    std::vector<uint8_t> row=std::vector<uint8_t>(mt::NodeJournal::LargeRecordSize,53);
    LargeFixture(){for(unsigned i=0;i<2;++i){auto *p=tokens[i].bytes;memcpy(p,"ZNX1",4);p[4]=81;p[8]=i;p[12]=i+1;}}
    int replace(unsigned i,uint8_t value,bool enrolled=true){row[100]=value;return files.replace(i,tokens[i],row.data(),row.size(),enrolled);}
    void verify(unsigned slot,uint8_t a,uint8_t b){
        assert(files.load(slot,tokens[slot],row.data(),row.size())==int(row.size()));
        assert(row[100]==a||row[100]==b);for(unsigned i=0;i<row.size();++i)if(i!=100)assert(row[i]==53);
    }
};
#endif
template<unsigned N,unsigned Thin,unsigned Warm,unsigned Rows,unsigned Blocks,unsigned Cache=64> static void storage(){
    using D=PeerDirectory<Thin,Rows>;using L=WarmPeerLedger<Warm>;using P=SatellitePages<N>;
    std::vector<uint8_t> media(Blocks*4096,255);SatelliteSample first{};
    typename D::Domain d{};d.node=77;d.key[0]=7;d.epoch[0]=2;
    {
        Device dev(media,true,Cache);PeerDirectoryVolume thin;WarmPeerVolume warm;Reports reports;D directory;
        assert(!directory.open(thin,d,42));auto domain=directory.warmDomain();
        for(unsigned i=0;i<Thin;i++){mt::NodeCache::Entry e;e.identity(1000+i,1,0);assert(!directory.save(thin,D::Row::copy(e)));}
        Map generated;L ledger;assert(!ledger.open(generated,domain,0));
        for(unsigned i=0;i<Warm;i++){WarmPeerRecord r{},previous{};r.id=10000+i;
            assert(!ledger.begin(L::Action::Demote,r));int rc;do{rc=ledger.step(generated,previous);}while(rc==-EINPROGRESS);assert(!rc);}
        for(const auto &v:generated.files)assert(!warm.put(v.first,v.second.data(),v.second.size()));
        assert(!lfs_mkdir(&dev.fs,"public"));for(const char *name:{"public/rangetest.csv","public/owner.txt"}){
            lfs_file_t f{};assert(!lfs_file_open(&dev.fs,&f,name,LFS_O_WRONLY|LFS_O_CREAT));assert(lfs_file_write(&dev.fs,&f,"data",4)==4&&!lfs_file_close(&dev.fs,&f));}
        P pages;pages.bind(domain.directory);Map staged;
        auto check=[](const SatelliteOwner &,uint32_t &t){t=1700000000;return 0;};
        for(unsigned k=0;k<4;k++)for(unsigned i=0;i<N;i++){
            SatelliteSample r{};uint32_t time=0;assert(!directory.binding(thin,1000+i,r.owner,time));
            r.kind=k;r.channel=i%8;r.flags=29;r.quarterSnr=-29;r.utc=1700000000+i;r.size=SatelliteSample::limit(k);
            memset(r.bytes,i+k+1,r.size);if(!i&&!k)first=r;
            pages.start();int e;do{e=pages.step(staged,r,check);}while(e==-EINPROGRESS);assert(!e);
        }
        for(const auto &row:staged.files)assert(!reports.put(row.first,row.second.data(),row.second.size()));
        for(unsigned k=0;k<4;k++)for(unsigned i=0;i<N;i++){SatelliteSample r{};assert(!pages.read(reports,k,i,r));assert(r.size==SatelliteSample::limit(k));}
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
        LargeFixture full;assert(!full.replace(0,1,false)&&!full.replace(1,2,false));
        full.verify(0,1,1);full.verify(1,2,2);
#endif
        printf("PASS actual %u-sector filesystem %u thin, %u warm, %u full-size reports per kind, two public files; steady blocks %d\n",Blocks,Thin,Warm,N,int(lfs_fs_size(&dev.fs)));
    }
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    {
        auto image=media;Device dev(image,false,Cache);LargeFixture full;
        full.tokens[1].bytes[12]^=0x55;
        assert(full.replace(1,37)==-EILSEQ);
        full.tokens[1].bytes[12]^=0x55;full.verify(1,2,2);
        lfs_file_t unexpected{};assert(!lfs_file_open(&dev.fs,&unexpected,"nodedb.stage",LFS_O_CREAT|LFS_O_WRONLY));
        assert(lfs_file_write(&dev.fs,&unexpected,"foreign",7)==7&&!lfs_file_close(&dev.fs,&unexpected));
        assert(full.replace(1,37)==-EILSEQ);
        char old[7];assert(!lfs_file_open(&dev.fs,&unexpected,"nodedb.stage",LFS_O_RDONLY));
        assert(lfs_file_read(&dev.fs,&unexpected,old,7)==7&&!memcmp(old,"foreign",7)&&!lfs_file_close(&dev.fs,&unexpected));
        full.verify(1,2,2);
    }
    int mutations=0;
    {auto image=media;Device dev(image,false,Cache);LargeFixture full;dev.mutations=0;assert(!full.replace(1,3));mutations=dev.mutations;}
    for(int cut=0;cut<=mutations;++cut)for(bool torn:{false,true}){
        auto image=media;
        {Device dev(image,false,Cache);LargeFixture full;dev.mutations=0;dev.failAt=cut;dev.torn=torn;(void)full.replace(1,3);}
        {Device dev(image,false,Cache);LargeFixture full;full.verify(0,1,1);full.verify(1,2,3);}
    }
    printf("PASS extended NodeDB native-NOR replacement %d mutation points, clean/torn cuts, two old/new complete selectors\n",mutations);
#endif
    // Hold one fully synced report stage, as after a cut before its rename.
    for(bool stage:{false,true})for(bool warmFirst:{false,true}){
        auto copy=media;Device dev(copy,false,Cache);PeerDirectoryVolume thin;WarmPeerVolume warm;D directory;
        assert(!directory.open(thin,d,42));L ledger;assert(!ledger.open(warm,directory.warmDomain(),0));
        if(stage){
            lfs_file_t f{};char full[P::MaxPage]{};assert(!lfs_file_open(&dev.fs,&f,".zt-rpt-stage",LFS_O_CREAT|LFS_O_WRONLY));
            assert(lfs_file_write(&dev.fs,&f,full,sizeof(full))==sizeof(full));assert(!lfs_file_sync(&dev.fs,&f));
            assert(!lfs_file_close(&dev.fs,&f));
        }
        mt::NodeCache::Entry e;e.identity(1000+Thin-1,7,0);
        int thinRc=0;if(!warmFirst)thinRc=directory.save(thin,D::Row::copy(e));
        WarmPeerRecord r{},before{};r.id=10000+Warm-1;r.packed=WarmPeerRecord::pack(1700000101,7,2,true);
        assert(!ledger.begin(L::Action::Demote,r));int warmRc;
        do{warmRc=ledger.step(warm,before);}while(warmRc==-EINPROGRESS);
        if(warmFirst)thinRc=directory.save(thin,D::Row::copy(e));
        printf("CHECK %u sectors, report stage %s, warm-first %u, thin replacement %d, warm replacement %d, blocks %d\n",
               Blocks,stage?"held":"absent",warmFirst,thinRc,warmRc,int(lfs_fs_size(&dev.fs)));fflush(stdout);
        if(Blocks==18&&Cache==64&&stage)assert(thinRc==-ENOSPC);else assert(!thinRc&&!warmRc);
    }
    auto change=[&](Device &dev,bool fail,int cut,bool torn){
        Reports io;PeerDirectoryVolume thin;D directory;assert(!directory.open(thin,d,42));P pages;pages.bind(directory.warmDomain().directory);
        auto sample=first;sample.bytes[0]^=0x35;dev.mutations=0;if(fail){dev.failAt=cut;dev.torn=torn;}
        auto check=[&](const SatelliteOwner &o,uint32_t &t){SatelliteOwner actual{};int rc=directory.binding(thin,o.id,actual,t);return rc?rc:o.same(actual)?0:-ESTALE;};
        int rc;do{rc=pages.step(io,sample,check);}while(rc==-EINPROGRESS);return rc;
    };
    // The production worker serializes thin, warm and report transactions.
    // XModem's stage stays open between separate calls on that same worker.
    for(unsigned size:{4000u,4096u,4097u,8000u}){
        auto copy=media;Device dev(copy,false,Cache);PeerDirectoryVolume thin;WarmPeerVolume warm;D directory;
        L ledger;assert(!directory.open(thin,d,42)&&!ledger.open(warm,directory.warmDomain(),0));
        const uint32_t revoked=1000+N-2;assert(!directory.forget(thin,revoked));
        lfs_file_t upload{};assert(!lfs_file_open(&dev.fs,&upload,"public/.zt-upload",LFS_O_CREAT|LFS_O_WRONLY));
        char data[128]{};int writeRc=0;
        for(unsigned n=0;n<size&&!writeRc;){unsigned chunk=size-n>128?128:size-n;
            int rc=lfs_file_write(&dev.fs,&upload,data,chunk);if(rc!=int(chunk))writeRc=rc<0?rc:-EIO;n+=chunk;}
        if(!writeRc)writeRc=lfs_file_sync(&dev.fs,&upload);
        mt::NodeCache::Entry e;e.identity(1000+Thin-1,7,0);
        int thinRc=directory.save(thin,D::Row::copy(e));WarmPeerRecord record{},previous{};
        record.id=10000+Warm-1;record.packed=5123;assert(!ledger.begin(L::Action::Demote,record));
        int warmRc;do{warmRc=ledger.step(warm,previous);}while(warmRc==-EINPROGRESS);
        int reportRc=thinRc?thinRc:change(dev,false,0,false);
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
        LargeFixture full;assert(!full.replace(1,4));full.verify(0,1,1);full.verify(1,4,4);
#endif
        printf("CHECK %u sectors, %u cache, %u-byte public upload held, write=%d thin=%d warm=%d report=%d\n",
            Blocks,Cache,size,writeRc,thinRc,warmRc,reportRc);
        if((Blocks==18&&Cache==128&&size<=4096)||Blocks==128)assert(!writeRc&&!thinRc&&!warmRc&&!reportRc);
        assert(!lfs_file_close(&dev.fs,&upload));
        if(!thinRc){
            if(!writeRc)assert(!lfs_rename(&dev.fs,"public/.zt-upload","public/owner.txt"));
        }else{
            assert(thinRc==-ENOSPC);
            // The authorized public upload is canceled. Reclaim only unselected
            // private banks and stages, then retry all actual private writers.
            assert(!lfs_remove(&dev.fs,"public/.zt-upload"));
            assert(!directory.open(thin,d,42));
            SatelliteOwner withdrawn{};uint32_t heard=0;assert(directory.binding(thin,revoked,withdrawn,heard)==-ENOENT);
            typename D::Row retained{};assert(!directory.read(thin,1000+Thin-1,retained));
            assert(mt::NodeCache::Entry::get32(retained.user+4)==1);
            assert(!directory.reclaimInactive(thin));Reports reports;
            assert(!reports.dropStage()&&!warm.dropStage());
            assert(!directory.save(thin,D::Row::copy(e)));
            assert(!ledger.open(warm,directory.warmDomain(),0));
            assert(!ledger.begin(L::Action::Demote,record));
            do{warmRc=ledger.step(warm,previous);}while(warmRc==-EINPROGRESS);
            assert(!warmRc&&!change(dev,false,0,false));
            assert(directory.binding(thin,revoked,withdrawn,heard)==-ENOENT);
            lfs_info file{};assert(!lfs_stat(&dev.fs,"public/owner.txt",&file)&&file.size==4);
        }
    }
    int count=0;{auto final=media;Device dev(final,false,Cache);assert(!change(dev,false,0,false));count=dev.mutations;}
    for(int cut=0;cut<=count;cut++)for(bool torn:{false,true}){
        auto bytes=media;{Device dev(bytes,false,Cache);(void)change(dev,true,cut,torn);}
        {Device dev(bytes,false,Cache);PeerDirectoryVolume thin;D directory;assert(!directory.open(thin,d,42));Reports io;P pages;
         pages.bind(directory.warmDomain().directory);SatelliteSample r{};assert(!pages.read(io,0,0,r));
         assert(r.bytes[0]==first.bytes[0]||r.bytes[0]==uint8_t(first.bytes[0]^0x35));assert(!memcmp(r.bytes+1,first.bytes+1,r.size-1));
         assert(!pages.read(io,1,0,r)&&r.bytes[0]==2);}
    }
    for(int cut=0;cut<count;cut++)for(bool torn:{false,true}){
        auto copy=media;Device dev(copy,false,Cache);lfs_file_t transfer{};
        assert(!lfs_file_open(&dev.fs,&transfer,"public/owner.txt",LFS_O_WRONLY));
        assert(lfs_file_write(&dev.fs,&transfer,"lost",4)==4);
        assert(change(dev,true,cut,torn)<0);dev.failAt=-1;
        assert(zt_lfs_health==ZT_LFS_UNCERTAIN);
        const int before=dev.mutations;
        assert(lfs_file_sync(&dev.fs,&transfer)==LFS_ERR_IO);
        assert(lfs_file_write(&dev.fs,&transfer,"oops",4)==LFS_ERR_IO);
        assert(lfs_mkdir(&dev.fs,"unsafe")==LFS_ERR_IO);
        assert(lfs_file_close(&dev.fs,&transfer)==LFS_ERR_IO);
        assert(before==dev.mutations);
        lfs_file_t next{};assert(!lfs_file_open(&dev.fs,&next,"public/owner.txt",LFS_O_RDONLY));
        char content[4]{};assert(lfs_file_read(&dev.fs,&next,content,4)==4&&!memcmp(content,"data",4));
        assert(!lfs_file_close(&dev.fs,&next)&&zt_lfs_health==ZT_LFS_OK);
        assert(!lfs_file_open(&dev.fs,&next,"public/next",LFS_O_WRONLY|LFS_O_CREAT));
        assert(lfs_file_write(&dev.fs,&next,"good",4)==4&&!lfs_file_sync(&dev.fs,&next)&&!lfs_file_close(&dev.fs,&next));
        Reports io;P pages;D directory;PeerDirectoryVolume thin;assert(!directory.open(thin,d,42));pages.bind(directory.warmDomain().directory);
        SatelliteSample r{};assert(!pages.read(io,1,0,r)&&r.bytes[0]==2);assert(!pages.read(io,0,0,r));
        assert(r.bytes[0]==first.bytes[0]||r.bytes[0]==uint8_t(first.bytes[0]^0x35));
    }
    printf("PASS %u-slot same-boot recovery after %u transient I/O failures with a preexisting dirty public upload\n",N,count*2);
    printf("PASS %u-slot production page atomic replace across %u hardware-free interrupted/torn writes\n",N,(count+1)*2);
}
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
int main(){storage<199,199,2000,16,128>();}
#else
int main(){storage<40,119,100,40,18>();storage<40,119,100,40,18,128>();storage<40,119,100,40,19>();storage<199,199,2000,16,128>();}

#endif
