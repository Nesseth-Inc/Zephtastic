// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <monocypher.h>
#include "../NodeCache.h"

namespace mt::companion {
// ZT's private wire format combines the pinned per-kind cap with the
// independent durable enrollment of each admitted thin-directory peer.
struct SatelliteOwner {
    uint32_t id=0,lifetime=0;
    uint8_t key[32]{},slot=0,keySize=0;
    bool valid()const{
        if(id<=1||id>=UINT32_MAX-1||!lifetime||(keySize!=0&&keySize!=32))return false;
        if(!keySize)for(auto b:key)if(b)return false;
        return true;
    }
    bool same(const SatelliteOwner &b)const{
        return id==b.id&&lifetime==b.lifetime&&slot==b.slot&&keySize==b.keySize&&!memcmp(key,b.key,sizeof(key));
    }
};
struct SatelliteSample {
    enum Kind:uint8_t{Position,Device,Environment,Status,Kinds};
    SatelliteOwner owner{};
    uint32_t utc=0;
    int16_t quarterSnr=0;
    uint8_t kind=0,channel=0,flags=0,size=0,bytes[233]{};
    static constexpr unsigned limit(unsigned k){return k<2?32:k==Environment?233:k==Status?79:0;}
    bool valid()const{return owner.valid()&&kind<Kinds&&channel<8&&!(flags&~31u)&&
        ((flags&8)||!(flags&7))&&size<=limit(kind);}
};

template<unsigned Count> class SatellitePages {
    static_assert(Count==40||Count==199);
public:
    static constexpr unsigned Capacity=Count,Pages=Count==40?6:30,MaxPage=4000;
private:
    static constexpr unsigned Header=24,Digest=16,Prefix=50;
    struct Layout {uint8_t slots[Pages][4]{};uint16_t used[Pages]{};};
    static constexpr Layout layout(){
        Layout result{};
        constexpr unsigned types[]={2,3,0,1};
        for(auto k:types)for(unsigned i=0;i<Count;i++){
            unsigned p=0;
            for(unsigned other=1;other<Pages;other++)if(result.used[other]<result.used[p])p=other;
            ++result.slots[p][k];result.used[p]+=Prefix+SatelliteSample::limit(k);
        }
        return result;
    }
    static constexpr auto map_=layout();
    static constexpr unsigned maxBytes(){unsigned n=0;for(auto size:map_.used)if(size>n)n=size;return n+Header+Digest;}
    static_assert(maxBytes()<=MaxPage);
    using E=NodeCache::Entry;
    struct Page {uint8_t data[maxBytes()];};
    uint8_t domain_[16]{};
    bool ready_=false;
    unsigned next_=0;int target_=-1,empty_=-1,oldest_=-1;
    uint32_t oldestUtc_=UINT32_MAX,oldestId_=UINT32_MAX;
    static unsigned countBefore(unsigned p,unsigned k){
        unsigned base=0;for(unsigned i=0;i<p;i++)base+=map_.slots[i][k];return base;
    }
    static uint8_t *at(Page &page,unsigned p,unsigned k,unsigned slot){
        unsigned offset=Header;
        for(unsigned i=0;i<k;i++)offset+=map_.slots[p][i]*(Prefix+SatelliteSample::limit(i));
        return page.data+offset+slot*(Prefix+SatelliteSample::limit(k));
    }
    template<class IO> int load(IO &io,unsigned p,Page &page){
        const unsigned n=Header+map_.used[p]+Digest;
        int rc=io.get(int(p),page.data,n);
        if(rc&&rc!=-ENOENT)return rc;
        if(!rc){
            if(memcmp(page.data,"ZSP1",4)||E::get32(page.data+20)!=(Count<<8|p))return -EXDEV;
            uint8_t hash[Digest];
            crypto_blake2b(hash,Digest,page.data,n-Digest);
            if(memcmp(hash,page.data+n-Digest,Digest))return -EILSEQ;
        }
        if(rc==-ENOENT||memcmp(page.data+4,domain_,16)){
            memset(page.data,0,n);memcpy(page.data,"ZSP1",4);memcpy(page.data+4,domain_,16);
            E::put32(page.data+20,Count<<8|p);
        }
        return 0;
    }
    static void owner(const uint8_t *src,SatelliteOwner &out){
        static_assert(offsetof(SatelliteOwner,id)==0&&offsetof(SatelliteOwner,lifetime)==4&&offsetof(SatelliteOwner,key)==8);
        static_assert(__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__);
        memcpy(static_cast<void*>(&out),src,40);
        out.keySize=src[47];out.slot=src[49];
    }
    static int decode(const uint8_t *src,unsigned kind,SatelliteSample &out){
        if(!E::get32(src))return -ENOENT;
        owner(src,out.owner);out.kind=kind;
        static_assert(offsetof(SatelliteSample,quarterSnr)==offsetof(SatelliteSample,utc)+4);
        memcpy(&out.utc,src+40,4);memcpy(&out.quarterSnr,src+44,2);
        out.flags=src[46]&31;out.channel=src[46]>>5;out.size=src[48];
        if(!out.valid()||!out.size)return -EILSEQ;
        memcpy(out.bytes,src+Prefix,out.size);return 0;
    }
    template<class IO> int replace(IO &io,Page &page,unsigned p,unsigned local,const SatelliteSample &sample){
        uint8_t *dst=at(page,p,sample.kind,local);
        const unsigned n=Header+map_.used[p]+Digest,width=Prefix+SatelliteSample::limit(sample.kind);
        if(!sample.size&&!E::get32(dst))return 0;
        if(sample.size){
            uint8_t header[Prefix];memcpy(header,&sample.owner,40);
            memcpy(header+40,&sample.utc,4);memcpy(header+44,&sample.quarterSnr,2);
            header[46]=uint8_t(sample.channel<<5|sample.flags);header[47]=sample.owner.keySize;
            header[48]=sample.size;header[49]=sample.owner.slot;
            if(!memcmp(dst,header,Prefix)&&!memcmp(dst+Prefix,sample.bytes,sample.size))return 0;
            memcpy(dst,header,Prefix);
            memcpy(dst+Prefix,sample.bytes,sample.size);memset(dst+Prefix+sample.size,0,width-Prefix-sample.size);
        }else memset(dst,0,width);
        crypto_blake2b(page.data+n-Digest,Digest,page.data,n-Digest);
        return io.put(int(p),page.data,n);
    }
public:
    bool bind(const uint8_t (&domain)[16]){
        bool old=ready_&&!memcmp(domain,domain_,16);memcpy(domain_,domain,16);
        ready_=false;for(auto b:domain_)ready_|=b!=0;if(!old)start();return old;
    }
    bool ready()const{return ready_;}
    void start(){next_=0;target_=empty_=oldest_=-1;oldestUtc_=oldestId_=UINT32_MAX;}
    // Only an already validated owner may seed a page. check() compares its
    // full current key, enrollment and domain and supplies the owner's current
    // last-heard, as NodeDB::evictStalestSatellite does.
    template<class IO,class Sample,class Check,class Merge> int step(IO &io,Sample &sample,Check check,Merge merge){
        if(!ready_||!sample.valid())return -EINVAL;
        uint32_t current=0;int admit=check(sample.owner,current);if(admit)return admit;
        unsigned p=next_,slot=0;
        if(target_>=0&&!locate(sample.kind,unsigned(target_),p,slot))return -EILSEQ;
        Page page;int rc=load(io,p,page);if(rc)return rc;
        if(target_>=0)return replace(io,page,p,slot,sample);
        const auto first=countBefore(p,sample.kind);
        for(unsigned i=0;i<map_.slots[p][sample.kind];i++){
            const uint8_t *rec=at(page,p,sample.kind,i);const auto id=E::get32(rec);
            if(id==sample.owner.id){
                SatelliteOwner previous{};owner(rec,previous);
                if(sample.kind==SatelliteSample::Position&&sample.size&&previous.same(sample.owner)&&
                   (rec[46]>>5)==sample.channel){
                    if(!rec[48]||rec[48]>32)return -EILSEQ;
                    const int result=merge(rec+Prefix,rec[48],sample);if(result)return result;
                }
                return replace(io,page,p,i,sample);
            }
            if(!sample.size)continue;
            const int ordinal=int(first+i);
            if(!id){if(empty_<0)empty_=ordinal;continue;}
            if(empty_>=0)continue;
            SatelliteOwner binding{};owner(rec,binding);if(!binding.valid())return -EILSEQ;
            uint32_t heard=0;
            const int eligible=check(binding,heard);
            if(eligible==-ENOENT||eligible==-ESTALE){empty_=ordinal;continue;}
            if(eligible)return eligible;
            if(oldest_<0||heard<oldestUtc_||(heard==oldestUtc_&&id<oldestId_)){
                oldest_=ordinal;oldestUtc_=heard;oldestId_=id;
            }
        }
        if(++next_<Pages)return -EINPROGRESS;
        if(!sample.size)return 0;
        target_=empty_>=0?empty_:oldest_;if(target_<0)return -ENOSPC;
        return -EINPROGRESS;
    }
    template<class IO,class Check> int step(IO &io,const SatelliteSample &sample,Check check){
        return step(io,sample,check,[](const uint8_t*,unsigned,const SatelliteSample &){return 0;});
    }
    static bool locate(unsigned kind,unsigned ordinal,unsigned &page,unsigned &local){
        if(kind>=4||ordinal>=Count)return false;
        for(page=0;page<Pages;page++){if(ordinal<map_.slots[page][kind]){local=ordinal;return true;}ordinal-=map_.slots[page][kind];}
        return false;
    }
    template<class IO> int read(IO &io,unsigned kind,unsigned ordinal,SatelliteSample &out){
        unsigned p,slot;if(!ready_||!locate(kind,ordinal,p,slot))return -ENOENT;
        Page page;const int rc=load(io,p,page);return rc?rc:decode(at(page,p,kind,slot),kind,out);
    }
};
}
