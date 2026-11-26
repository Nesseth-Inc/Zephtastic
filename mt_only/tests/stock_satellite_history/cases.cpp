// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include "NodeJournal.h"
#include "companion/SatelliteHistory.h"
#include "companion/PeerDirectoryOwner.h"
#include "companion/WarmPeerLedger.h"
#include "companion/PeerDirectoryFiles.h"
#include "companion/SatelliteVolume.h"
#include "lfs.h"
#include <algorithm>
#include <map>
#include <limits>
#include <string>
using Bytes=std::vector<uint8_t>;
using History=mt::companion::SatelliteHistory;
using Replay=mt::companion::SatelliteReplay;
struct Settings:mt::SettingsBackend{
    std::map<std::string,Bytes> rows;
    int read(const char *k,uint8_t *p,size_t n)override{
        if(!rows.count(k))return -ENOENT;const auto &v=rows[k];if(v.size()>n)return -E2BIG;
        memcpy(p,v.data(),v.size());return int(v.size());
    }
    int write(const char *k,const uint8_t *p,size_t n)override{rows[k]={p,p+n};return 0;}
};
struct Volume{
    Bytes &nor;int operations=0,fail=-1;bool torn=false,mounted=false,opened=false,checkSecondRead=false,readOutage=false;int secondRead=0;
    lfs_t lfs{};lfs_file_t handle{};lfs_config cfg{};
    explicit Volume(Bytes &media):nor(media){
        cfg.context=this;cfg.block_size=4096;cfg.block_count=nor.size()/4096;cfg.block_cycles=512;
        cfg.read_size=16;cfg.prog_size=16;cfg.cache_size=64;cfg.lookahead_size=16;
        cfg.read=[](const lfs_config *c,lfs_block_t b,lfs_off_t off,void *p,lfs_size_t n){
            auto &v=*static_cast<Volume*>(c->context);size_t a=b*4096+off;assert(a+n<=v.nor.size());
            if(v.readOutage)return -EIO;
            memcpy(p,v.nor.data()+a,n);return 0;
        };
        cfg.prog=[](const lfs_config *c,lfs_block_t b,lfs_off_t off,const void *p,lfs_size_t n){
            auto &v=*static_cast<Volume*>(c->context);int at=v.operations++;
            bool cut=v.fail>=0&&at>=v.fail;size_t used=cut?(v.torn&&at==v.fail?n/2:0):n;
            for(size_t i=0;i<used;i++){auto &current=v.nor[b*4096+off+i];auto value=static_cast<const uint8_t*>(p)[i];
                assert((current&value)==value);current=value;}
            return cut?-EIO:0;
        };
        cfg.erase=[](const lfs_config *c,lfs_block_t b){
            auto &v=*static_cast<Volume*>(c->context);int at=v.operations++;
            bool cut=v.fail>=0&&at>=v.fail;size_t size=cut?(v.torn&&at==v.fail?2048:0):4096;
            memset(v.nor.data()+4096*b,255,size);return cut?-EIO:0;
        };
        cfg.sync=[](const lfs_config*){return 0;};
    }
    ~Volume(){if(opened)close();if(mounted)lfs_unmount(&lfs);}
    int mount(){int e=lfs_mount(&lfs,&cfg);mounted=!e;return e;}
    int erasedFormat(){return mt::companion::initializeErasedHistory(nor.size(),255,
        [&](size_t off,void *p,size_t n){memcpy(p,nor.data()+off,n);return 0;},[&](){return lfs_format(&lfs,&cfg);});}
    int openRead(){int e=lfs_file_open(&lfs,&handle,"history",LFS_O_RDONLY);opened=!e;return e;}
    int stage(){int e=lfs_file_open(&lfs,&handle,"pending",LFS_O_WRONLY|LFS_O_TRUNC|LFS_O_CREAT);opened=!e;return e;}
    int read(void *p,size_t n){if(checkSecondRead&&++secondRead==6)return -EIO;return lfs_file_read(&lfs,&handle,p,n);}
    int exact(void *p,size_t n){int r=read(p,n);return r==int(n)?0:r<0?r:-EILSEQ;}
    int rewind(){if(secondRead<0){checkSecondRead=true;secondRead=0;}return lfs_file_seek(&lfs,&handle,0,LFS_SEEK_SET);}
    int write(const void *p,size_t n){int r=lfs_file_write(&lfs,&handle,p,n);return r==int(n)?0:r<0?r:-EIO;}
    int close(){opened=false;return lfs_file_close(&lfs,&handle);}
    void abort(){(void)close();}
    int commit(){int rc=lfs_file_sync(&lfs,&handle);int ended=close();return rc?rc:ended?ended:lfs_rename(&lfs,"pending","history");}
};
static void populate(Fixture &f,Settings &settings,mt::NodeJournal &journal){
    assert(!journal.load(settings,f.s.nodes,OURS));journal.enableReplayEpochs(f.s.nodes);
    for(unsigned i=0;i<2;i++){mt::NodeCache::Entry e;e.identity(PEER+i,81,0);e.bytes[e.KeyLength]=32;memset(e.bytes+e.Key,int(i+10),32);
        assert(f.s.nodes.accept(e,OURS,-5,2)>=0);}
    assert(journal.flush(settings,f.s.nodes,0,true,true)==1);
}
static Bytes encoded(const pb_msgdesc_t *type,const void *value){
    Bytes b(233);auto os=pb_ostream_from_buffer(b.data(),b.size());assert(pb_encode(&os,type,value));b.resize(os.bytes_written);return b;
}
static void values(Fixture &f,uint32_t node,unsigned sequence){
    auto &s=f.s.satellites;auto &nodes=f.s.nodes;
    Replay::Observation info{1700000000u+sequence,-29,8+2+16};
    meshtastic_Position p{};p.has_latitude_i=p.has_longitude_i=true;p.latitude_i=0;p.longitude_i=4242+sequence;
    p.precision_bits=23;mt::PeerPosition position;assert(position.assign(p));
    assert(s.remember(nodes,node,Replay::Position,2,nullptr,0,info)&&nodes.observePosition(node,position,true));
    meshtastic_DeviceMetrics d{};d.has_voltage=true;d.voltage=3.3;mt::PeerDeviceMetrics device;assert(device.assign(d));
    assert(s.remember(nodes,node,Replay::Device,1,nullptr,0,info)&&nodes.observeDeviceMetrics(node,device,true));
    meshtastic_Telemetry t{};t.which_variant=meshtastic_Telemetry_environment_metrics_tag;
    t.variant.environment_metrics.has_temperature=true;t.variant.environment_metrics.temperature=17.5;
    Bytes e=encoded(meshtastic_Telemetry_fields,&t);assert(s.remember(nodes,node,Replay::Environment,3,e.data(),e.size(),info));
    nodes.environmentReplay(node,true);
    char text[80]="public";mt::PeerStatus status;assert(status.assign(text));
    assert(s.remember(nodes,node,Replay::Status,0,nullptr,0,info)&&nodes.observeStatus(node,status,true));
}
static History::Domain realm(){History::Domain d{};d.node=OURS;d.channelGeneration=37;
    memset(d.publicKey,17,32);memset(d.channelDigest,42,16);memset(d.epoch,31,16);return d;}
static uint32_t longitude(const mt::NodeCache::Entry &e){meshtastic_Position p{};e.position.exportTo(p);return p.longitude_i;}
static void wire(Fixture &f){
    f.h.out.clear();meshtastic_ToRadio want{};want.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;want.want_config_id=69421;
    input(f.c,want,1000);for(unsigned i=0;i<40;i++)f.c.poll(mt::Companion::WorkspaceProof{true});
    unsigned count=0;
    for(const auto &m:f.h.out)if(m.which_payload_variant==meshtastic_FromRadio_packet_tag){
        const auto &p=m.packet;const uint8_t channels[]={2,1,3,0};
        assert(p.from==PEER&&p.to==UINT32_MAX&&p.channel==channels[count]&&p.has_rx_time&&p.rx_time==1700000001);
        assert(p.rx_snr==-7.25f&&p.hop_start==f.s.config.hopLimit()&&p.hop_limit+2==p.hop_start&&p.via_mqtt);
        if(!count){meshtastic_Position position{};auto in=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);
            assert(pb_decode(&in,meshtastic_Position_fields,&position)&&!position.has_latitude_i&&position.longitude_i==4243&&position.precision_bits==23);}
        ++count;
    }
    assert(count==4);
}
static void maxPopulation(unsigned sectors){
    Bytes media(sectors*4096,255);Volume v(media);assert(!v.erasedFormat()&&!v.mount());
    auto f=std::make_unique<Fixture>();Settings settings;mt::NodeJournal j;
    assert(!j.load(settings,f->s.nodes,OURS));j.enableReplayEpochs(f->s.nodes);
    for(unsigned i=0;i<mt::NodeCache::Capacity;i++){
        mt::NodeCache::Entry e;e.identity(PEER+i,81,0);e.bytes[e.KeyLength]=32;memset(e.bytes+e.Key,int(i+1),32);
        assert(f->s.nodes.accept(e,OURS,-3,0)>=0);
    }
    assert(j.flush(settings,f->s.nodes,0,true,true)==1);
    History h;auto d=realm();assert(!h.load(v,f->s.nodes,f->s.satellites,d,255));
    for(unsigned i=0;i<mt::NodeCache::Capacity;i++){
        const auto node=PEER+i;values(*f,node,i);
        char text[80];memset(text,'a'+i,79);text[79]=0;mt::PeerStatus status;assert(status.assign(text));
        Replay::Observation meta{1700000000u+i,5,8};assert(f->s.satellites.remember(f->s.nodes,node,Replay::Status,7,nullptr,0,meta));
        assert(f->s.nodes.observeStatus(node,status,true));
        meshtastic_Telemetry full{};full.time=UINT32_MAX;full.which_variant=meshtastic_Telemetry_environment_metrics_tag;
        auto &m=full.variant.environment_metrics;
#define ALL(ctx,rule,count,type,name,tag) m.has_##name=true;m.name=static_cast<decltype(m.name)>(std::min<double>(0x10000000,std::numeric_limits<decltype(m.name)>::max()));
        meshtastic_EnvironmentMetrics_FIELDLIST(ALL,_)
#undef ALL
        auto body=encoded(meshtastic_Telemetry_fields,&full);assert(body.size()>210&&body.size()<=233);
        assert(f->s.satellites.remember(f->s.nodes,node,Replay::Environment,5,body.data(),body.size(),meta));f->s.nodes.environmentReplay(node,true);
    }
    assert(!h.save(v,f->s.nodes,f->s.satellites,d,255));lfs_info info{};assert(!lfs_stat(&v.lfs,"history",&info));
    assert(info.size>6000&&info.size<7400);
    assert(!j.load(settings,f->s.nodes,OURS));f->s.satellites.clear();assert(!h.load(v,f->s.nodes,f->s.satellites,d,255));
    for(unsigned i=0;i<mt::NodeCache::Capacity;i++){
        const auto *e=f->s.nodes.get(i);assert(e->replayDevice&&e->replayEnvironment&&e->replayPosition&&e->status.replayable());
        assert(strlen(e->status.text())==79&&f->s.satellites.channel(e->id(),Replay::Status)==7);
    }
    if(sectors==19){
        assert(!lfs_unmount(&v.lfs));v.mounted=false;v.readOutage=true;
        History recovery;assert(v.mount()==-EIO&&recovery.due(100));recovery.attempted(100,false);
        assert(!j.load(settings,f->s.nodes,OURS));f->s.satellites.clear();
        // Live UTC can move backwards. Neither that update, the public device
        // observation nor a newer private status may be replaced by the file.
        constexpr uint32_t received=12345;
        mt::PeerStatus status;char message[80]="private after the outage";assert(status.assign(message));
        assert(f->s.nodes.observeStatus(PEER,status,false)&&f->s.nodes.observeReceivedPacket(PEER,received,true));
        meshtastic_DeviceMetrics metrics{};metrics.has_voltage=true;metrics.voltage=4.2;
        mt::PeerDeviceMetrics device;assert(device.assign(metrics));
        assert(f->s.satellites.remember(f->s.nodes,PEER,Replay::Device,6,nullptr,0,{received,-13,8}));
        assert(f->s.nodes.observeDeviceMetrics(PEER,device,true));
        auto live=[&](){
            const auto &e=*f->s.nodes.get(0);meshtastic_DeviceMetrics exported{};e.deviceMetrics.exportTo(exported);
            assert(e.status.present()&&!e.status.replayable()&&!strcmp(e.status.text(),message));
            assert(e.replayDevice&&exported.voltage==metrics.voltage&&f->s.satellites.channel(PEER,Replay::Device)==6);
            assert(f->s.satellites.received(PEER,Replay::Device).utc==received&&e.lastHeard()==received&&e.viaMqtt);
        };
        assert(!recovery.due(60099)&&recovery.due(60100));recovery.attempted(60100,false);
        v.readOutage=false;assert(!v.mount());v.secondRead=-1;
        assert(recovery.load(v,f->s.nodes,f->s.satellites,d,255)==-EIO&&!recovery.ready());
        live();size_t bytes=0;assert(!f->s.nodes.get(0)->replayPosition&&!f->s.satellites.environment(PEER,bytes));
        assert(!recovery.due(120099)&&recovery.due(120100));
        v.secondRead=0;v.checkSecondRead=false;
        assert(!recovery.load(v,f->s.nodes,f->s.satellites,d,255)&&recovery.ready());live();
        assert(f->s.nodes.get(0)->replayPosition&&f->s.nodes.get(0)->replayEnvironment&&
               f->s.nodes.get(1)->status.replayable()&&f->s.satellites.environment(PEER,bytes));
        recovery.attempted(120100,true);assert(!recovery.due(120100+History::Interval-1));
        puts("PASS delayed mount: rate-limited retry, private and public live values and UTC survive a second-pass read fault; absent kinds restore on retry");
    }
    assert(!j.load(settings,f->s.nodes,OURS));f->s.satellites.clear();v.secondRead=-1;
    assert(h.load(v,f->s.nodes,f->s.satellites,d,255)==-EIO&&!f->s.nodes.get(0)->replayPosition&&!f->s.nodes.get(0)->lastHeard());
    v.checkSecondRead=false;v.secondRead=0;
    // Corrupt the tail after the final row. The validating pass rejects it
    // before publishing even the first peer to a newly started companion.
    assert(!v.openRead());Bytes broken(info.size);assert(!v.exact(broken.data(),broken.size())&&!v.close());
    broken.back()^=1;assert(!v.stage()&&!v.write(broken.data(),broken.size())&&!v.commit());
    assert(!j.load(settings,f->s.nodes,OURS));f->s.satellites.clear();
    assert(h.load(v,f->s.nodes,f->s.satellites,d,255)==-EILSEQ&&!f->s.nodes.get(0)->replayPosition);
    printf("PASS all 16 peers and four kinds on %u sectors, 41-field environments and 79-byte statuses in %u bytes; mid-restore outage and corrupt checksum leave every peer unpublished\n",sectors,unsigned(info.size));
}
static void receiveToCold(){
    Bytes media(32*4096,255);Volume v(media);assert(!v.erasedFormat()&&!v.mount());
    auto f=std::make_unique<Fixture>();Settings settings;mt::NodeJournal j;populate(*f,settings,j);
    History h;auto domain=realm();assert(!h.load(v,f->s.nodes,f->s.satellites,domain,1));
    auto receive=[&](meshtastic_PortNum port,const pb_msgdesc_t *desc,const void *x,uint32_t to,unsigned id){
        auto payload=encoded(desc,x);meshtastic_Data d{};d.has_bitfield=true;d.portnum=port;
        memcpy(d.payload.bytes,payload.data(),payload.size());d.payload.size=payload.size();auto data=encoded(meshtastic_Data_fields,&d);
        uint8_t frame[255];size_t n=0;assert(f->s.codec.encodeZeroHopData(PEER,to,id,data.data(),data.size(),frame,255,n));
        f->s.rx(frame,n,-8,id,id+3);
        f->c.receive(frame,n,-80,-8,id,1700000000+id,true,uint8_t(-29),true,mt::Companion::WorkspaceProof{true});
    };
    meshtastic_StatusMessage st{};strcpy(st.status,"shared");receive(meshtastic_PortNum_NODE_STATUS_APP,meshtastic_StatusMessage_fields,&st,UINT32_MAX,200);
    meshtastic_Position pos{};pos.has_latitude_i=pos.has_longitude_i=true;pos.latitude_i=10;pos.longitude_i=30;
    receive(meshtastic_PortNum_POSITION_APP,meshtastic_Position_fields,&pos,UINT32_MAX,201);
    strcpy(st.status,"direct only");receive(meshtastic_PortNum_NODE_STATUS_APP,meshtastic_StatusMessage_fields,&st,OURS,202);
    assert(!h.save(v,f->s.nodes,f->s.satellites,domain,1));
    assert(!j.load(settings,f->s.nodes,OURS));f->s.satellites.clear();assert(!h.load(v,f->s.nodes,f->s.satellites,domain,1));
    const auto *e=f->s.nodes.get(0);const auto obs=f->s.satellites.received(PEER,Replay::Position);
    assert(e->replayPosition&&!e->status.present()&&e->lastHeard()==1700000201&&obs.quarterSnr==-29&&obs.restored());
    puts("PASS canonical RF receiver to littlefs to cold phone: latest shared observation and quarter-dB signal retained, private status bytes and private receive time excluded");
}

static void lifecycle(){
    mt::NodeCache nodes;mt::NodeJournal journal;Settings settings;
    assert(!journal.load(settings,nodes,OURS));mt::NodeCache::Entry peer;
    peer.identity(PEER,81,1);peer.bytes[peer.KeyLength]=32;memset(peer.bytes+peer.Key,10,32);
    assert(nodes.accept(peer,OURS,1,0)>=0&&journal.flush(settings,nodes,1,true,true)==1);
    assert(!nodes.replayEpoch(0)&&settings.rows["mtbase/nodes0"][3]==3);
    nodes.setRouteHint(PEER,5);assert(journal.flush(settings,nodes,2,true,true)==1);
    assert(settings.rows["mtbase/nodes1"][3]==6&&!journal.load(settings,nodes,OURS));
    journal.enableReplayEpochs(nodes);assert(journal.flush(settings,nodes,3,true,true)==1);
    auto bound=nodes.replayEpoch(0);assert(bound&&settings.rows["mtbase/nodes0"].size()==1572);
    peer.bytes[peer.Name]='A';assert(nodes.accept(peer,OURS,3,1)>=0&&journal.flush(settings,nodes,4,true,true)==1);
    assert(nodes.replayEpoch(0)==bound);
    assert(!journal.edit(settings,nodes,PEER,mt::NodeCache::Edit::Ignore,5,true));
    assert(nodes.replayEpoch(0)!=bound);bound=nodes.replayEpoch(0);
    assert(!journal.edit(settings,nodes,PEER,mt::NodeCache::Edit::Unignore,6,true)&&nodes.replayEpoch(0)!=bound);
    bound=nodes.replayEpoch(0);memset(peer.bytes+peer.Key,11,32);
    assert(!journal.ownerContact(settings,nodes,peer,false,true,7,true)&&nodes.replayEpoch(0)!=bound);
    bound=nodes.replayEpoch(0);assert(!journal.load(settings,nodes,OURS)&&nodes.replayEpoch(0)==bound);
    const auto latest=journal.activeSlot()?"mtbase/nodes1":"mtbase/nodes0";auto row=settings.rows[latest];settings.rows.clear();
    mt::NodeCache::Entry::put32(row.data()+4,UINT32_MAX);uint32_t crc=UINT32_MAX;
    for(size_t i=0;i<row.size()-4;++i){crc^=row[i];for(unsigned j=0;j<8;j++)crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));}
    mt::NodeCache::Entry::put32(row.data()+row.size()-4,~crc);settings.rows[latest]=row;
    assert(!journal.load(settings,nodes,OURS));nodes.setRouteHint(PEER,44);
    assert(journal.flush(settings,nodes,8,true,true)==-EOVERFLOW&&!nodes.ready()&&settings.rows.size()==1);
    puts("PASS v3/v6 to v7 verified migration, stable lifetime across rename, fresh lifetime after ignore, unignore or rekey; exhausted generation cannot wrap");
}
struct ResetVolume:Volume {
    using Volume::Volume;
    bool unavailable=false;
    int mount(bool initialize,bool publicTree){
        assert(!initialize&&!publicTree);
        if(unavailable)return -EIO;
        return mounted?0:Volume::mount();
    }
    int verifyErased(){
        return mt::companion::initializeErasedHistory(nor.size(),255,
            [&](size_t offset,void *p,size_t n){memcpy(p,nor.data()+offset,n);return 0;},[](){return 0;});
    }
    static const char *local(const char *p){
        assert(!strncmp(p,"/zt-history/",12));return p+12;
    }
    int remove(const char *name){assert(mounted);return lfs_remove(&lfs,local(name));}
    int eraseFiles(){return mt::companion::SatelliteFiles::eraseOwned([&](const char *p){return remove(p);});}
    int discardManifest(){return remove("/zt-history/.zt-directory");}
    int nonce(void *p,size_t size){static unsigned seq=0;memset(p,++seq,size);return 0;}
    int putFile(const char *name,const char *temp,const void *data,size_t size){
        lfs_file_t f{};int rc=lfs_file_open(&lfs,&f,local(temp),LFS_O_CREAT|LFS_O_TRUNC|LFS_O_WRONLY);
        if(rc)return rc;
        auto count=lfs_file_write(&lfs,&f,data,size);
        rc=count==int(size)?lfs_file_sync(&lfs,&f):(count<0?int(count):-EIO);
        int closed=lfs_file_close(&lfs,&f);if(!rc)rc=closed;
        return rc?rc:lfs_rename(&lfs,local(temp),local(name));
    }
    int getFile(const char *name,void *data,size_t size){
        lfs_file_t f{};int rc=lfs_file_open(&lfs,&f,local(name),LFS_O_RDONLY);if(rc)return rc;
        auto count=lfs_file_read(&lfs,&f,data,size);uint8_t extra{};
        int rcSize=(count==int(size)&&lfs_file_read(&lfs,&f,&extra,1)==0)?0:-EILSEQ;
        int closed=lfs_file_close(&lfs,&f);
        return rcSize?rcSize:closed;
    }
    int put(int page,unsigned bank,const void *data,size_t size){
        using Files=mt::companion::PeerDirectoryFiles;char name[40];Files::path(name,page,bank);
        return putFile(name,Files::stage,data,size);
    }
    int get(int page,unsigned bank,void *data,size_t size){
        char name[40];mt::companion::PeerDirectoryFiles::path(name,page,bank);return getFile(name,data,size);
    }
    int put(int page,const void *data,size_t size){
        using Files=mt::companion::WarmPeerNames;char name[40];Files::path(name,page,0);
        return putFile(name,Files::stage,data,size);
    }
    int get(int page,void *data,size_t size){
        char name[40];mt::companion::WarmPeerNames::path(name,page,0);return getFile(name,data,size);
    }
    void retire(int page,unsigned bank){
        char name[40];mt::companion::PeerDirectoryFiles::path(name,page,bank);assert(!remove(name));
    }
    int reset(){
        return mt::companion::erasePrivateHistory(*this,[&](){
            int rc=mt::companion::erasePeerFiles<mt::companion::PeerDirectoryFiles>(mt::companion::PeerDirectory<119,32>::ResetPages,2,[&](const char *p){return remove(p);});
            if(rc)return rc;
            rc=mt::companion::erasePeerFiles<mt::companion::WarmPeerNames>(2,1,[&](const char *p){return remove(p);});
            if(rc)return rc;
            return mt::companion::eraseSharedRangeLog([&](){
                lfs_info info{};return lfs_stat(&lfs,"public",&info);
            },[&](){return remove("/zt-history/public/rangetest.csv");});
        });
    }
};
template<unsigned Thin,unsigned Warm> static void fillDirectory(ResetVolume &fs){
    using Directory=mt::companion::PeerDirectory<Thin,32>;
    Directory directory;typename Directory::Domain domain{};domain.node=OURS;domain.epoch[0]=7;domain.key[0]=9;
    assert(!directory.open(fs,domain,1));
    for(unsigned i=0;i<Thin;i++){
        mt::NodeCache::Entry entry{};entry.identity(PEER+i,81,0);
        assert(!directory.save(fs,Directory::Row::copy(entry)));
    }
    using Ledger=mt::companion::WarmPeerLedger<Warm>;
    Ledger ledger;mt::companion::WarmPeerDomain d{};d.self=OURS;d.epoch[0]=5;d.owner[0]=7;d.directory[0]=9;
    assert(!ledger.open(fs,d,0));
    for(unsigned i=0;i<Warm;i++){
        mt::companion::WarmPeerRecord row{},previous{};row.id=PEER+Thin+i;row.packed=42;
        assert(!ledger.begin(Ledger::Action::Demote,row));
        int result;do {result=ledger.step(fs,previous);}while(result==-EINPROGRESS);assert(!result);
    }
    assert(directory.count()==Thin);
}
static void priorGeometry(){
    Bytes media(19*4096,255);ResetVolume fs(media);assert(!fs.erasedFormat()&&!fs.mount(false,false));
    mt::companion::PeerDirectory<119,16> old;decltype(old)::Domain prior{};
    prior.node=OURS;prior.key[0]=2;prior.epoch[0]=3;assert(!old.open(fs,prior,7));
    mt::NodeCache::Entry peer{};peer.identity(PEER,81,0);
    assert(!old.save(fs,decltype(old)::Row::copy(peer)));
    mt::companion::PeerDirectory<119,32> fresh;decltype(fresh)::Domain target{};
    memcpy(&target,&prior,sizeof(target));auto snapshot=media;
    assert(fresh.open(fs,target,7)==-EXDEV&&snapshot==media);
    using Owner=mt::companion::PeerDirectoryOwner<119,1,32>;
    Owner foreign;assert(foreign.open(fs,target,7)==-EXDEV);
    static Owner *owner;static ResetVolume *volume;owner=&foreign;volume=&fs;
    mt::NodeJournal journal;mt::NodeCache hot;Settings settings;
    assert(!journal.load(settings,hot,OURS));
    journal.directoryFence([](uint32_t from,uint32_t to){return owner->fenceJournal(*volume,from,to);});
    assert(hot.accept(peer,OURS,-3,0)>=0);
    assert(journal.flush(settings,hot,0,true,true)==1&&snapshot==media);
    owner=nullptr;volume=nullptr;
    assert(!old.open(fs,prior,7)&&old.eligible(PEER));
}
static void lateWithdrawal(){
    using Directory=mt::companion::PeerDirectory<119,32>;
    Bytes media(19*4096,255);Directory::Domain d{};d.node=OURS;d.key[0]=9;d.epoch[0]=7;
    auto channel=realm();channel.directoryChannels(d.channels);
    {ResetVolume blank(media);assert(!mt::companion::withExistingHistory(blank,[](){assert(false);return -EIO;}));}
    {
        ResetVolume fs(media);assert(!fs.erasedFormat()&&!fs.mount(false,false));
        Directory writer;assert(!writer.open(fs,d,1));
        for(unsigned i=0;i<2;++i){
            mt::NodeCache::Entry row{};row.identity(PEER+i,81,0);
            assert(!writer.save(fs,Directory::Row::copy(row)));
        }
    }
    {
        ResetVolume fs(media);Directory directory;bool epochReadable=false;
        auto request=[&](){
            return mt::companion::withExistingHistory(fs,[&](){
                if(!epochReadable)return -EAGAIN;
                int rc=directory.open(fs,d,1);return rc?rc:directory.forget(fs,PEER);
            });
        };
        fs.unavailable=true;assert(request()==-EAGAIN);
        fs.unavailable=false;assert(request()==-EAGAIN);
        epochReadable=true;assert(!request());
    }
    {
        ResetVolume fs(media);assert(!fs.mount(false,false));
        Directory reopened;assert(!reopened.open(fs,d,1));
        assert(!reopened.eligible(PEER)&&reopened.eligible(PEER+1));
        const auto &retired=reopened.withdrawals();
        assert(retired.serial&&retired.ids[(retired.serial-1)%retired.Count]==PEER);
        auto displayEdit=d;channel.directoryChannels(displayEdit.channels);
        assert(!reopened.open(fs,displayEdit,1)&&reopened.eligible(PEER+1));
        // Skip opening the intermediate channel. Returning to the same key and
        // name must still give both directory tiers a fresh lifetime.
        const auto oldWarm=reopened.warmDomain();channel.channelGeneration+=2;
        auto returned=d;channel.directoryChannels(returned.channels);
        assert(!reopened.open(fs,returned,1)&&!reopened.count());
        const auto newWarm=reopened.warmDomain();assert(memcmp(&oldWarm,&newWarm,sizeof(oldWarm)));
    }
    puts("PASS mount and epoch failures refuse archived withdrawal; retry commits a targeted retirement that survives a cold reopen");
}
static void coldReset(){
    Bytes raw(19*4096,255);
    {ResetVolume first(raw);assert(!first.reset()&&!first.mounted&&!first.operations);}
    {Bytes foreign=raw;foreign.back()=0;ResetVolume unknown(foreign);
     assert(unknown.reset()==-EAGAIN&&!unknown.operations);}
    {
        ResetVolume fs(raw);assert(!fs.erasedFormat()&&!fs.mount(false,false));
        fillDirectory<119,100>(fs);
        auto create=[&](const char *p){
            lfs_file_t f{};assert(!lfs_file_open(&fs.lfs,&f,p,LFS_O_CREAT|LFS_O_WRONLY));
            assert(lfs_file_write(&fs.lfs,&f,"data",4)==4&&!lfs_file_close(&fs.lfs,&f));
        };
        assert(!lfs_mkdir(&fs.lfs,"public"));create("public/rangetest.csv");create("public/owner.txt");
        Bytes snapshot(7368,0xab);
        const auto before=lfs_fs_size(&fs.lfs);
        using Files=mt::companion::SatelliteFiles;
        assert(!fs.putFile(Files::file,Files::temp,snapshot.data(),snapshot.size()));
        snapshot[0]=0xef;
        assert(!fs.putFile(Files::file,Files::temp,snapshot.data(),snapshot.size()));
        create(".zt-sat-v1.tmp");create(".zt-dir-tmp");create(".zt-warm-tmp");
        // Prior 16-row page geometry owns the same private namespace. These
        // final page names fall beyond the 32-row current directory manifest.
        create(".zt-dir-04-0");create(".zt-dir-07-1");
        auto occupied=lfs_fs_size(&fs.lfs);
        printf("PASS richest 19-sector volume: 119 thin + 100 warm peers, two public files, repeated 7368-byte atomic snapshots; %d blocks used beforehand, %d after replacement and three staged names\n",int(before),int(occupied));
    }
    {ResetVolume cold(raw);cold.fail=0;assert(cold.reset()<0);}
    {ResetVolume retry(raw);assert(!retry.reset());}
    {
        ResetVolume readback(raw);assert(!readback.mount(false,false));lfs_dir_t dir{};
        assert(!lfs_dir_open(&readback.lfs,&dir,"/"));lfs_info info{};int rc;
        while((rc=lfs_dir_read(&readback.lfs,&dir,&info))>0)
            assert(!strcmp(info.name,".")||!strcmp(info.name,"..")||!strcmp(info.name,"public"));
        assert(!rc&&!lfs_dir_close(&readback.lfs,&dir));
        assert(lfs_stat(&readback.lfs,"public/rangetest.csv",&info)==LFS_ERR_NOENT);
        assert(!lfs_stat(&readback.lfs,"public/owner.txt",&info));
        assert(!lfs_remove(&readback.lfs,"public/owner.txt")&&!lfs_remove(&readback.lfs,"public"));
    }
    {ResetVolume cold(raw);assert(!cold.reset());}
    puts("PASS unmounted cold reset on blank, unrecognized and populated media; committed thin pages, satellite files and CSV erased after retry");
}
int main(){
    priorGeometry();lateWithdrawal();coldReset();lifecycle();
    for(unsigned sectors:{19u,31u,128u})maxPopulation(sectors);receiveToCold();
    // If first-use initialization lost power, a subsequently recognizable FS
    // may mount. Ambiguous, nonblank media may never be silently reformatted.
    unsigned interrupted=0;
    for(int step=0;step<12;step++){
        Bytes image(19*4096,255);
        {Volume first(image);first.fail=step;first.torn=true;(void)first.erasedFormat();}
        if(std::all_of(image.begin(),image.end(),[](uint8_t b){return b==255;}))continue;
        Volume restarted(image);if(restarted.mount()){auto before=image;
            assert(restarted.erasedFormat()==-ENOTEMPTY&&before==image&&!restarted.operations);++interrupted;}
    }
    assert(interrupted);printf("PASS refused to reformat %u nonblank unmountable first-use interruption images\n",interrupted);
    Bytes media(31*4096,255);Settings settings;auto f=std::make_unique<Fixture>();mt::NodeJournal j;populate(*f,settings,j);
    auto d=realm();History history;auto &nodes=f->s.nodes;auto &s=f->s.satellites;
    {
        Volume fs(media);assert(fs.mount()!=0&&!fs.operations);auto unknown=media;unknown.back()=0;Volume foreign(unknown);
        assert(foreign.erasedFormat()==-ENOTEMPTY&&!foreign.operations);
        bool formatted=false;assert(mt::companion::initializeErasedHistory(512,255,[](size_t,void*,size_t){return -EIO;},
            [&](){formatted=true;return 0;})==-EIO&&!formatted);
        assert(!fs.erasedFormat()&&!fs.mount());assert(!history.load(fs,nodes,s,d,15));
        assert(!history.due(1000)&&!history.due(1000+History::Interval-1)&&history.due(1000+History::Interval));
        values(*f,PEER,1);values(*f,PEER+1,1);
        // Replacing A's status with a direct report removes its eligibility.
        char privateText[80]="private";mt::PeerStatus direct;assert(direct.assign(privateText));
        nodes.observeStatus(PEER+1,direct,false);
        assert(!history.save(fs,nodes,s,d,15));auto noWrites=fs.operations;
        assert(!history.save(fs,nodes,s,d,15)&&fs.operations==noWrites);
    }
    auto cold=std::make_unique<Fixture>();mt::NodeJournal cj;assert(!cj.load(settings,cold->s.nodes,OURS));
    assert(cold->s.nodes.get(0)->replayPosition==false);
    auto check=[&](Bytes &bytes,Fixture &out,const History::Domain &domain){
        Volume fs(bytes);assert(!fs.mount());History reader;return reader.load(fs,out.s.nodes,out.s.satellites,domain,15);
    };
    assert(!check(media,*cold,d));assert(longitude(*cold->s.nodes.get(0))==4243&&cold->s.nodes.get(1)->replayDevice&&!cold->s.nodes.get(1)->status.replayable());
    const unsigned oldEpoch=cold->s.nodes.replayEpoch(1);
    cold->s.nodes.setRouteHint(PEER+1,24);assert(cj.flush(settings,cold->s.nodes,1,true,true)==1);
    assert(cold->s.nodes.replayEpoch(1)==oldEpoch);
    assert(!cj.edit(settings,cold->s.nodes,PEER+1,mt::NodeCache::Edit::Remove,2,true));
    mt::NodeCache::Entry e;e.identity(PEER+1,81,0);e.bytes[e.KeyLength]=32;memset(e.bytes+e.Key,11,32);
    assert(cold->s.nodes.accept(e,OURS,3,1)>=0&&cj.flush(settings,cold->s.nodes,3,true,true)==1);
    assert(cold->s.nodes.replayEpoch(1)!=oldEpoch);
    assert(!cj.load(settings,cold->s.nodes,OURS));cold->s.satellites.clear();
    assert(!check(media,*cold,d)&&cold->s.nodes.get(0)->replayPosition&&!cold->s.nodes.get(1)->replayDevice);
    wire(*cold);
    for(unsigned k=0;k<4;k++){
        auto wrong=d;if(k==0)wrong.epoch[0]++;if(k==1)wrong.publicKey[0]++;if(k==2)wrong.channelDigest[0]++;if(k==3)wrong.channelGeneration++;
        assert(!cj.load(settings,cold->s.nodes,OURS));cold->s.satellites.clear();assert(!check(media,*cold,wrong)&&!cold->s.nodes.get(0)->replayPosition);
    }
    puts("PASS private LittleFS first use, unknown-volume refusal, legacy-safe node epochs, unrelated edits, same-key reenrollment, per-kind source channels and public cold-boot PhoneAPI metadata");
    Bytes baseline=media;int total=0;
    for(int cut=-1;cut<total;cut++)for(unsigned torn=0;torn<(cut<0?1u:2u);++torn){
        Bytes image=baseline;auto owner=std::make_unique<Fixture>();mt::NodeJournal journal;
        assert(!journal.load(settings,owner->s.nodes,OURS));
        {Volume vol(image);assert(!vol.mount());History h;assert(!h.load(vol,owner->s.nodes,owner->s.satellites,d,15));
         values(*owner,PEER,2);vol.operations=0;vol.torn=torn;vol.fail=cut;
         int rc=h.save(vol,owner->s.nodes,owner->s.satellites,d,15);
         if(cut<0){assert(!rc);total=vol.operations;}
        }
        assert(!journal.load(settings,owner->s.nodes,OURS));owner->s.satellites.clear();
        assert(!check(image,*owner,d));const auto value=longitude(*owner->s.nodes.get(0));
        assert(value==4243||value==4244);
    }
    assert(total>4);printf("PASS complete snapshots across %d LittleFS program/erase boundaries, before and torn writes, with durable old-or-new file readback\n",total);
}
