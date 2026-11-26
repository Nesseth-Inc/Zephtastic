// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "SatelliteReplay.h"
#include "../RemoteTelemetryQuery.h"
#include <monocypher.h>

namespace mt::companion {
// One private file. Only the owner's atomic rename publishes a complete generation.
// Encode one peer at a time, under the same lock used by live radio/phone admission.
class SatelliteHistory {
public:
    struct Domain {
        uint32_t node=0,channelGeneration=0;
        uint8_t channelDigest[16]{},publicKey[32]{},epoch[16]{};
        void directoryChannels(uint8_t (&out)[16])const{
            struct Binding {uint32_t generation;uint8_t digest[16];} binding{channelGeneration,{}};
            static_assert(sizeof(binding)==20);
            memcpy(binding.digest,channelDigest,sizeof(channelDigest));
            crypto_blake2b(out,sizeof(out),reinterpret_cast<const uint8_t*>(&binding),sizeof(binding));
        }
    };
    static_assert(sizeof(Domain)==72);
    static constexpr uint64_t Interval=2ull*60*60*1000;
private:
    using Kind=SatelliteReplay::Kind;
    struct Header {uint8_t magic[4];uint32_t generation,inverse;Domain domain;};
    struct Row {
        uint32_t node=0,peerEpoch=0,received[4]{};
        uint8_t keySize=0,key[32]{},mask=0,channels[4]{},length[4]{},flags[4]{};int16_t snr[4]{};
        uint8_t body[32+32+233+79]{}; // PositionLite, DeviceMetrics, Environment wire, status text.
    };
    static constexpr size_t Prefix=offsetof(Row,body);
    static_assert(sizeof(Header)==84&&Prefix==78);
    uint32_t generation_=0;
    uint64_t next_=0;
    bool ready_=false;
    uint8_t previous_[16]{};
    static bool usable(const Domain &d){
        uint8_t some=0;for(auto b:d.publicKey)some|=b;
        uint8_t epoch=0;for(auto b:d.epoch)epoch|=b;
        return some&&epoch&&d.node>1&&d.node<UINT32_MAX-1;
    }
    static bool encode(const pb_msgdesc_t *desc,const void *value,uint8_t *p,size_t size,uint8_t &length){
        auto os=pb_ostream_from_buffer(p,size);if(!pb_encode(&os,desc,value)||!os.bytes_written)return false;
        length=uint8_t(os.bytes_written);return true;
    }
    static int makeRow(const NodeCache::Entry &e,SatelliteReplay &samples,uint32_t epoch,uint8_t available,Row &r){
        if(!epoch||e.ignored()||e.placeholder())return 0;
        r.peerEpoch=epoch;
        r.node=e.id();r.keySize=e.bytes[NodeCache::Entry::KeyLength];
        memcpy(r.key,e.bytes+NodeCache::Entry::Key,r.keySize);
        size_t used=0;
        for(unsigned k=0;k<4;k++){
            auto kind=Kind(k);unsigned channel=samples.channel(e.id(),kind);
            if(channel>=8||!(available&(1u<<channel)))continue;
            uint8_t &length=r.length[k];
            if(kind==Kind::Position&&e.replayPosition){
                meshtastic_Position p{};e.position.exportReplay(p);
                if(!encode(meshtastic_Position_fields,&p,r.body+used,32,length))return -EILSEQ;
            }else if(kind==Kind::Device&&e.replayDevice){
                meshtastic_DeviceMetrics d{};e.deviceMetrics.exportTo(d);
                if(!encode(meshtastic_DeviceMetrics_fields,&d,r.body+used,32,length))return -EILSEQ;
            }else if(kind==Kind::Environment&&e.replayEnvironment){
                size_t n=0;auto *p=samples.environment(e.id(),n);
                if(!p||!n||n>233)return -EILSEQ;
                memcpy(r.body+used,p,n);length=uint8_t(n);
            }else if(kind==Kind::Status&&e.status.replayable()&&e.status.text()[0]){
                size_t n=strlen(e.status.text());if(n>79)return -EILSEQ;
                memcpy(r.body+used,e.status.text(),n);length=uint8_t(n);
            }else continue;
            r.mask|=uint8_t(1u<<k);r.channels[k]=channel;auto obs=samples.received(e.id(),kind);
            r.received[k]=obs.utc;r.snr[k]=obs.quarterSnr;r.flags[k]=obs.flags&31;used+=length;
        }
        return used?int(Prefix+used):0;
    }
    static int inspect(const Row &r,unsigned channels,unsigned &bodySize){
        static constexpr uint8_t maxLen[]={32,32,233,79};bodySize=0;
        if(!r.peerEpoch||!r.mask||r.mask>=16||!(r.keySize==0||r.keySize==32))return -EILSEQ;
        if(!r.keySize){for(auto b:r.key)if(b)return -EILSEQ;}
        for(unsigned k=0;k<4;++k){
            if(!(r.mask&(1u<<k))){if(r.length[k]||r.channels[k]||r.received[k]||r.flags[k]||r.snr[k])return -EILSEQ;continue;}
            if((r.flags[k]&~31u)||(!(r.flags[k]&8)&&(r.flags[k]&7))||!r.length[k]||r.length[k]>maxLen[k]||r.channels[k]>=8||!(channels&(1u<<r.channels[k])))return -EILSEQ;
            bodySize+=r.length[k];
        }
        return 0;
    }
    static int fields(const Row &r,NodeCache &nodes,SatelliteReplay &samples,uint8_t missing,uint8_t &added,uint32_t &publicUtc){
        size_t offset=0;
        for(unsigned k=0;k<4;k++){
            if(!(r.mask&(1u<<k)))continue;
            auto kind=Kind(k);const uint8_t *p=r.body+offset;
            size_t n=r.length[k];offset+=n;auto in=pb_istream_from_buffer(p,n);
            PeerPosition pos;PeerDeviceMetrics device;PeerStatus status;
            if(kind==Kind::Position){
                meshtastic_Position v{};if(!pb_decode(&in,meshtastic_Position_fields,&v))return -EILSEQ;
                // Stock PositionLite elides zero axes when rendered to the phone.
                v.has_latitude_i=v.has_longitude_i=true;if(!pos.assign(v))return -EILSEQ;
            }else if(kind==Kind::Device){
                meshtastic_DeviceMetrics v{};
                if(!pb_decode(&in,meshtastic_DeviceMetrics_fields,&v)||!device.assign(v))return -EILSEQ;
            }else if(kind==Kind::Environment){
                meshtastic_Telemetry t{};
                if(!RemoteTelemetryQuery::decodeCanonical(p,n,0,t)||t.which_variant!=meshtastic_Telemetry_environment_metrics_tag)return -EILSEQ;
            }else{
                char msg[80]{};if(memchr(p,0,n))return -EILSEQ;memcpy(msg,p,n);
                if(!status.assign(msg))return -EILSEQ;
            }
            if(!(missing&(1u<<k)))continue;
            SatelliteReplay::Observation obs{r.received[k],r.snr[k],uint8_t(r.flags[k]|32)};
            if(!samples.remember(nodes,r.node,kind,r.channels[k],p,n,obs))return -ENOMEM;
            added|=uint8_t(1u<<k);
            if(kind==Kind::Position)nodes.observePosition(r.node,pos,true);
            if(kind==Kind::Device)nodes.observeDeviceMetrics(r.node,device,true);
            if(kind==Kind::Environment)nodes.environmentReplay(r.node,true);
            if(kind==Kind::Status)nodes.observeStatus(r.node,status,true);
            if(r.received[k]>publicUtc)publicUtc=r.received[k];
        }
        return 0;
    }
    template<class Store> int read(Store &io,NodeCache &nodes,SatelliteReplay &samples,const Domain &domain,
                                   uint8_t channels,bool apply,uint32_t (&heard)[NodeCache::Capacity],uint8_t (&added)[NodeCache::Capacity]){
        Header h{};int rc=io.exact(&h,sizeof(h));if(rc)return rc;
        if(memcmp(h.magic,"ZSH\1",4)||!h.generation||h.inverse!=~h.generation)return -EILSEQ;
        // Each row has a per-enrollment epoch. Other peers may have been
        // edited since this snapshot; channel and local identity still bind the entire file.
        if(memcmp(&h.domain,&domain,sizeof(domain)))return -ESTALE;
        crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,16);
        crypto_blake2b_update(&hash,reinterpret_cast<const uint8_t*>(&h.domain),sizeof(h.domain));
        uint16_t seen[(NodeCache::Capacity+15)/16]{};
        for(unsigned i=0;;i++){
            Row row{};rc=io.exact(&row.node,sizeof(row.node));if(rc)return rc;
            if(!row.node)break;
            if(i>=NodeCache::Capacity)return -EILSEQ;
            rc=io.exact(reinterpret_cast<uint8_t*>(&row)+4,Prefix-4);if(rc)return rc;
            unsigned n=0;rc=inspect(row,channels,n);if(rc)return rc;
            if((rc=io.exact(row.body,n)))return rc;
            crypto_blake2b_update(&hash,reinterpret_cast<const uint8_t*>(&row),Prefix+n);
            const int slot=nodes.find(row.node);const auto *e=slot<0?nullptr:nodes.get(slot);
            if(!e||e->ignored()||e->placeholder()||row.peerEpoch!=nodes.replayEpoch(slot)||
               row.keySize!=e->bytes[NodeCache::Entry::KeyLength]||
               memcmp(row.key,e->bytes+NodeCache::Entry::Key,row.keySize))continue;
            const uint16_t mask=uint16_t(1u<<(slot%16));if(seen[slot/16]&mask)return -EILSEQ;seen[slot/16]|=mask;
            if((rc=fields(row,nodes,samples,apply?uint8_t(~e->sampleMask()):0,added[slot],heard[slot])))return rc;
        }
        uint8_t actual[16],expected[16],extra{};crypto_blake2b_final(&hash,actual);
        if((rc=io.exact(expected,sizeof(expected))))return rc;
        rc=io.read(&extra,1);if(rc)return rc<0?rc:-EILSEQ;
        if(memcmp(actual,expected,16))return -EILSEQ;
        generation_=h.generation;memcpy(previous_,actual,sizeof(previous_));return 0;
    }
public:
    template<class Store> int load(Store &io,NodeCache &nodes,SatelliteReplay &samples,const Domain &domain,uint8_t channels){
        ready_=false;generation_=0;
        if(!nodes.ready()||!usable(domain))return -EAGAIN;
        int rc=io.openRead();if(rc==-ENOENT){ready_=true;return 0;}if(rc)return rc;
        uint32_t heard[NodeCache::Capacity]{};uint8_t added[NodeCache::Capacity]{};bool restoring=false;
        rc=read(io,nodes,samples,domain,channels,false,heard,added);
        if(!rc){rc=io.rewind();if(!rc){
            restoring=true;rc=read(io,nodes,samples,domain,channels,true,heard,added);
        }}
        int close=io.close();if(!rc)rc=close;
        if(restoring){
            if(rc){
                // Admission holds the service mutex during both passes. A late
                // read failure may withdraw only the values installed here.
                nodes.clearReplay(added);
                for(unsigned i=0;i<nodes.count();++i)if(added[i])samples.rollback(nodes.get(i)->id(),added[i]);
            }
            else for(unsigned i=0;i<nodes.count();++i){
                const auto *e=nodes.get(i);if(heard[i]&&!e->lastHeard())nodes.observeLastHeard(e->id(),heard[i]);
            }
        }
        if(rc==-ESTALE)rc=0;
        ready_=!rc;return rc;
    }
    template<class Store> int save(Store &io,const NodeCache &nodes,SatelliteReplay &samples,const Domain &domain,
                                   uint8_t channels){
        if(!ready_||!nodes.ready()||!usable(domain)||generation_==UINT32_MAX)return -EAGAIN;
        Header h{{'Z','S','H',1},generation_+1,~(generation_+1),domain};
        crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,16);
        crypto_blake2b_update(&hash,reinterpret_cast<const uint8_t*>(&domain),sizeof(domain));
        auto rows=[&](bool stage,unsigned &count){
            for(unsigned i=0;i<nodes.count();++i){
                Row row{};int n=makeRow(*nodes.get(i),samples,nodes.replayEpoch(i),channels,row);
                if(n<0)return n;if(!n)continue;
                if(stage){int rc=io.write(&row,n);if(rc)return rc;}
                else{++count;crypto_blake2b_update(&hash,reinterpret_cast<const uint8_t*>(&row),size_t(n));}
            }
            return 0;
        };
        unsigned count=0;int rc=rows(false,count);if(rc)return rc;
        uint8_t digest[16];crypto_blake2b_final(&hash,digest);
        if((!generation_&&!count)||(generation_&&!memcmp(digest,previous_,sizeof(digest))))return 0;
        rc=io.stage();if(rc)return rc;
        rc=io.write(&h,sizeof(h));if(!rc)rc=rows(true,count);
        uint32_t end=0;if(!rc)rc=io.write(&end,sizeof(end));
        if(!rc)rc=io.write(digest,sizeof(digest));
        if(!rc)rc=io.commit();else io.abort();
        if(!rc){++generation_;memcpy(previous_,digest,sizeof(previous_));}
        return rc;
    }
#if defined(CONFIG_MT_PAGED_SATELLITES)
    void pagedReady(bool ready){if(ready_&&!ready)next_=0;ready_=ready;}
#endif
    bool ready()const{return ready_;}
    bool due(uint64_t now){
        if(!ready_)return now>=next_;
        if(!next_)next_=now+Interval;
        return now>=next_;
    }
    void attempted(uint64_t now,bool success){next_=now+(success?Interval:60000);}
};
}
