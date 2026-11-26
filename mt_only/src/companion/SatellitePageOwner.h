// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "SatellitePages.h"
#include "SatelliteReplay.h"
#include "SatelliteWire.h"
#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#else
#include <stdlib.h>
#endif

namespace mt::companion {
template<unsigned Capacity,unsigned Clients> class SatellitePageOwner {
    using Pages=SatellitePages<Capacity>;
    Pages pages_;
    struct Pending:SatelliteSample {
        uint64_t ticket=0;
        uint32_t headerUtc=0;
        int8_t expectedKeySize=-1;
        static void *operator new(size_t,void *storage)noexcept{return storage;}
    };
    Pending *queue_[4]{};
    unsigned queued_=0,nextReader_=0;
    static constexpr unsigned DirtyWords=(NodeCache::Capacity+15)/16;
    uint16_t dirty_[4][DirtyWords]{};
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    bool dirtyPending_=false;
    using DirtyCursor=uint16_t;
#else
    using DirtyCursor=uint8_t;
#endif
    DirtyCursor dirtyCursor_=0;
    struct Read {unsigned ordinal=0;uint64_t ticket=0;uint8_t kind=4;bool done=false;uint32_t id=0;uint8_t *sample=nullptr;};
    Read reads_[Clients]{};
    bool bound_=false,readable_=true;
    static uint8_t *allocate(size_t n){
#if defined(__ZEPHYR__)
        return static_cast<uint8_t*>(k_malloc(n));
#else
        return static_cast<uint8_t*>(malloc(n));
#endif
    }
    static void release(void *p){
#if defined(__ZEPHYR__)
        k_free(p);
#else
        free(p);
#endif
    }
    void remove(unsigned pos){
        release(queue_[pos]);
        --queued_;memmove(queue_+pos,queue_+pos+1,(queued_-pos)*sizeof(queue_[0]));queue_[queued_]={};
        if(!pos)pages_.start();
    }
    static bool alive(uint64_t ticket,const SatelliteOwner &owner){
        return ticket&&uint8_t(ticket)==owner.slot+1&&owner.lifetime<=uint32_t(ticket>>8);
    }
    template<class Directory,class IO> static int check(Directory &dir,IO &io,const SatelliteOwner &expected,uint32_t &heard){
        SatelliteOwner actual{};int rc=dir.binding(io,expected.id,actual,heard);
        return rc?rc:expected.same(actual)?0:-ESTALE;
    }
public:
    static constexpr unsigned Count=Capacity,FileCount=Pages::Pages;
    void disconnect(unsigned owner){
        if(owner>=Clients)return;auto &r=reads_[owner];release(r.sample);r={};
    }
    void withdraw(uint32_t id=0){
        for(unsigned i=queued_;i;i--)if(!id||queue_[i-1]->owner.id==id)remove(i-1);
        for(unsigned i=0;i<Clients;i++)if(!id||(reads_[i].id==id))disconnect(i);
        if(!id)memset(dirty_,0,sizeof(dirty_));
    }
    void bind(const uint8_t (&incarnation)[16]){
        if(!pages_.bind(incarnation)){
            withdraw();
            // Service clears live samples on channel, key and peer replacement.
            // Reconcile current hot data (including private tombstones) after
            // every mount/domain transition, even without another RF packet.
            if(pages_.ready())memset(dirty_,0xff,sizeof(dirty_));
        }
        bound_=pages_.ready();
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
        if(bound_)dirtyPending_=true;
#endif
    }
    template<class Directory,class DirIO,class PageIO,class WarmIO,class History>
    int openDirectory(Directory &dir,DirIO &dio,const typename Directory::Domain &domain,uint32_t generation,
                      PageIO &pio,WarmIO &wio,bool compatible,History &history){
        int result=dir.open(dio,domain,generation);
        if(!result&&compatible){
            result=dir.reclaimInactive(dio);
            if(!result)result=pio.dropStage();
            if(!result)result=wio.dropStage();
            if(result)dir.close();
        }
        if(!result&&compatible)bind(dir.warmDomain().directory);else close();
        history.pagedReady(bound_);return result;
    }
    void close(){withdraw();bound_=false;uint8_t empty[16]{};pages_.bind(empty);}
    void pauseReads(){
        readable_=false;
        for(unsigned i=0;i<Clients;i++)disconnect(i);
    }
    bool bound()const{return bound_;}
    // A newer admitted NodeInfo owns its directory write. A superseded
    // queued report can still write its body but must not rewind that header.
    void supersedeHeader(uint32_t id){
        for(unsigned i=0;i<queued_;i++)if(queue_[i]->owner.id==id)queue_[i]->headerUtc=0;
    }
    void dirty(unsigned slot,unsigned kind){
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
        if(slot<NodeCache::Capacity&&kind<4)dirtyPending_=true;
#endif
        if(slot<NodeCache::Capacity&&kind<4)dirty_[kind][slot/16]|=uint16_t(1u<<(slot%16));
    }
    template<class Capture> void retryHot(Capture capture){
        if(!bound_||queued_==4)return;
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
        if(!dirtyPending_)return;
#endif
        constexpr unsigned slots=4*NodeCache::Capacity;
        for(unsigned scan=0;scan<slots;++scan){
            const unsigned bit=dirtyCursor_,k=bit/NodeCache::Capacity,i=bit%NodeCache::Capacity;
            dirtyCursor_=(bit+1)%slots;
            auto &word=dirty_[k][i/16];const uint16_t mask=uint16_t(1u<<(i%16));
            if(!(word&mask))continue;
            if(capture(i,k)!=-EAGAIN)word&=uint16_t(~mask);
            return;
        }
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
        dirtyPending_=false;
#endif
    }
    template<class Directory> void reconcile(const NodeCache &hot,SatelliteReplay &live,Directory &dir){
        retryHot([&](unsigned slot,unsigned kind){
            const auto *e=hot.get(slot);
            if(!e||e->ignored()||e->placeholder()||!(e->sampleMask()&(1u<<kind)))return -ENOENT;
            if(!hot.replayEpoch(slot))return -EAGAIN;
            uint8_t bytes[233];size_t size=0;
            const bool publicSample=SatelliteWire::hot(*e,live,kind,bytes,size);
            const auto k=static_cast<SatelliteReplay::Kind>(kind);
            // A private replacement removes a prior public sample, without
            // using its payload, channel or private observation timestamp.
            return offer(dir.ticket(e->id()),e->id(),kind,publicSample?live.channel(e->id(),k):0,
                publicSample?live.received(e->id(),k):SatelliteReplay::Observation{},
                publicSample?bytes:nullptr,size,e->bytes+NodeCache::Entry::Key,e->bytes[NodeCache::Entry::KeyLength],
                live.publicHeader(e->id()));
        });
    }
    int offer(uint64_t ticket,uint32_t id,unsigned kind,unsigned channel,SatelliteReplay::Observation obs,
              const uint8_t *body,size_t n,const uint8_t *key=nullptr,int keySize=-1){
        return offer(ticket,id,kind,channel,obs,body,n,key,keySize,obs.utc);
    }
    int offer(uint64_t ticket,uint32_t id,unsigned kind,unsigned channel,SatelliteReplay::Observation obs,
              const uint8_t *body,size_t n,const uint8_t *key,int keySize,uint32_t headerUtc){
        if(!bound_||!ticket)return -EAGAIN;
        if((keySize!=-1&&keySize!=0&&keySize!=32)||(keySize==32&&!key)||id<=1||id>=UINT32_MAX-1||kind>=4||channel>=8||n>SatelliteSample::limit(kind)||
           (n&&!body))return -EINVAL;
        unsigned pos=0;
        // A different directory ticket may be an unrelated page edit or a
        // different enrollment with the same key. Let the worker validate
        // each lifetime independently, retaining a prior trusted timestamp.
        while(pos<queued_){
            const auto &old=*queue_[pos];
            if(old.owner.id==id&&old.kind==kind&&old.ticket==ticket&&old.expectedKeySize==keySize&&
               (keySize<=0||!memcmp(old.owner.key,key,32)))break;
            ++pos;
        }
        if(pos==4)return -EAGAIN;
        auto *item=queue_[pos];
        const bool fresh=!item||bool(item->size)!=bool(n);
        if(!item){
            void *storage=allocate(sizeof(Pending));if(!storage)return -EAGAIN;
            item=queue_[pos]=new(storage) Pending;
            ++queued_;
        }
        uint8_t projected[32];
        if(!fresh&&kind==SatelliteSample::Position&&n&&item->channel==channel){
            memcpy(projected,body,n);
            int rc=SatelliteWire::retainFixTime(item->bytes,item->size,projected,n);if(rc)return rc;
            body=projected;
        }
        if(headerUtc){supersedeHeader(id);item->headerUtc=headerUtc;}
        if(!pos&&fresh)pages_.start();
        if(n)memcpy(item->bytes,body,n);
        if(keySize>0)memcpy(item->owner.key,key,32);
        item->ticket=ticket;item->owner.id=id;item->kind=kind;item->size=n;item->channel=channel;
        item->utc=obs.utc;item->quarterSnr=obs.quarterSnr;item->flags=obs.flags&31;item->expectedKeySize=keySize;
        return 0;
    }
    // Return 1 to emit, 2 to skip, 0 at end, or -EAGAIN to wait. The same
    // payload stays owned until the caller advances or disconnects.
    template<class Directory> int request(Directory &dir,unsigned owner,unsigned kind,unsigned ordinal,SatelliteSample &out){
        if(!readable_||!bound_||!dir.ready()||owner>=Clients||ordinal>=Capacity||kind>=4)return 0;
        auto &r=reads_[owner];
        if(r.kind!=kind||r.ordinal!=ordinal){disconnect(owner);r.kind=kind;r.ordinal=ordinal;}
        if(!r.done)return -EAGAIN;
        if(!r.sample)return 2;
        memcpy(&out,r.sample,sizeof(out));
        if(!alive(dir.ticket(r.id),out.owner))return 2;
        // A changed page may be an unrelated peer, or a same-key reenrollment.
        // Re-read the exact current owner before emitting any prefetched bytes.
        if(dir.ticket(r.id)!=r.ticket){disconnect(owner);return -EAGAIN;}
        return 1;
    }
    template<class Directory,class DirIO,class PageIO> int poll(Directory &dir,DirIO &dio,PageIO &pio){
        if(!bound_||!dir.ready())return -EAGAIN;
        readable_=true;
        // Read and write owners alternate so a long handshake never blocks a
        // newly received report indefinitely.
        bool preferRead=nextReader_++%2||!queued_;
        if(preferRead)for(unsigned i=0;i<Clients;i++){
            auto &r=reads_[(i+nextReader_/2)%Clients];
            if(r.done||r.kind>=4)continue;
            SatelliteSample found{};int rc=pages_.read(pio,r.kind,r.ordinal,found);
            uint32_t age=0;if(!rc)rc=check(dir,dio,found.owner,age);
            if(rc==-EAGAIN)return rc;
            if(rc&&rc!=-ENOENT&&rc!=-ESTALE)return rc;
            if(!rc){
                found.utc=age;
                r.sample=allocate(sizeof(SatelliteSample));
                if(!r.sample)return -ENOMEM;
                memcpy(r.sample,&found,sizeof(found));r.id=found.owner.id;r.ticket=dir.ticket(found.owner.id);
            }
            r.done=true;return 0;
        }
        if(!queued_)return 0;
        auto &sample=*queue_[0];SatelliteOwner current{};uint32_t age=0;
        int rc=dir.binding(dio,sample.owner.id,current,age);
        if(rc==-ENOENT||rc==-ESTALE||(!rc&&!alive(sample.ticket,current))){remove(0);return 0;}
        if(rc)return rc;
        if(sample.expectedKeySize>=0&&(sample.expectedKeySize!=current.keySize||
           (current.keySize&&memcmp(sample.owner.key,current.key,current.keySize)))){
            remove(0);return 0;
        }
        // Publish the ordered owner timestamp first. A reset between the two
        // commits leaves the previous admitted body with the newer header,
        // matching NodeDB updating last_heard before its satellite payload.
        if(sample.headerUtc&&sample.headerUtc!=age){
            rc=dir.publicHeard(dio,current,sample.headerUtc);if(rc)return rc;
        }
        sample.headerUtc=0;
        sample.owner=current;
        rc=pages_.step(pio,sample,[&](const SatelliteOwner &o,uint32_t &heard){return check(dir,dio,o,heard);},
            [](const uint8_t *old,unsigned oldSize,SatelliteSample &next){
                uint8_t fix[32];size_t size=next.size;memcpy(fix,next.bytes,size);
                const int result=SatelliteWire::retainFixTime(old,oldSize,fix,size);
                if(!result){memcpy(next.bytes,fix,size);next.size=size;}
                return result;
            });
        if(rc==-EINPROGRESS||rc==-EAGAIN)return 0;
        if(!rc||rc==-ESTALE||rc==-ENOENT){remove(0);return 0;}
        pages_.start();return rc;
    }
};
}
