// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../NodeCache.h"
#include <string.h>
#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#else
#include <stdlib.h>
#endif

namespace mt::companion {
// The NodeCache owns identity and replay eligibility. The shared service keeps
// the last channel of each observation and only allocates environment bytes
// for known peers that actually sent a report. All callers hold serviceLock.
class SatelliteReplay {
public:
    struct Observation {
        uint32_t utc;int16_t quarterSnr;uint8_t flags;
        bool heard()const{return flags&8;}
        bool viaMqtt()const{return flags&16;}
        unsigned hops()const{return flags&7;}
        bool restored()const{return flags&32;}
    };
    enum Kind:uint8_t {Position,Device,Environment,Status,Done};
private:
    struct Source {
        uint32_t node=0;
        uint8_t *environment=nullptr;
        uint8_t length=0,channels[4]{};
#if defined(CONFIG_MT_SATELLITE_HISTORY)
        Observation received[4]{};
#if defined(CONFIG_MT_PAGED_SATELLITES)
        uint32_t publicUtc=0;
#endif
#endif
    } sources_[NodeCache::Capacity]{};
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    // The containing service is in PSRAM. Keep up to 199 maximum-size public
    // environment reports away from the internal radio/network allocator.
    uint8_t environmentBytes_[NodeCache::Capacity][233];
    uint8_t *allocate(Source &s,size_t){return environmentBytes_[&s-sources_];}
    static void discard(uint8_t *){}
#else
    static uint8_t *allocate(Source &,size_t n){
#if defined(__ZEPHYR__)
        return static_cast<uint8_t*>(k_malloc(n));
#else
        return static_cast<uint8_t*>(malloc(n));
#endif
    }
    static void discard(uint8_t *ptr){
        if(!ptr)return;
#if defined(__ZEPHYR__)
        k_free(ptr);
#else
        free(ptr);
#endif
    }
#endif
    static void release(Source &s){discard(s.environment);s={};}
    Source *find(uint32_t node){for(auto &s:sources_)if(s.node==node)return &s;return nullptr;}
    Source *source(const NodeCache &nodes,uint32_t node){
        if(auto *s=find(node))return s;
        for(auto &candidate:sources_)if(!candidate.node||nodes.find(candidate.node)<0){
            release(candidate);candidate.node=node;return &candidate;
        }
        return nullptr;
    }
public:
    SatelliteReplay()=default;
    SatelliteReplay(const SatelliteReplay&)=delete;
    SatelliteReplay &operator=(const SatelliteReplay&)=delete;
    ~SatelliteReplay(){clear();}
    void clear(){for(auto &s:sources_)release(s);}
    bool remember(const NodeCache &nodes,uint32_t node,Kind kind,unsigned channel,
                  const uint8_t *wire=nullptr,size_t size=0,Observation obs={}){
        const int slot=nodes.find(node);const auto *peer=slot<0?nullptr:nodes.get(unsigned(slot));
        if(!nodes.ready()||!peer||peer->ignored()||peer->placeholder()||channel>=8||kind>=Done)return false;
        if(kind==Environment&&(!wire||!size||size>233))return false;
        Source *s=source(nodes,node);if(!s)return false;
        if(kind==Environment){
            if(size!=s->length){
                uint8_t *fresh=allocate(*s,size);if(!fresh)return false;
                discard(s->environment);
                s->environment=fresh;s->length=uint8_t(size);
            }
            memcpy(s->environment,wire,size);
        }
#if defined(CONFIG_MT_SATELLITE_HISTORY)
        s->received[kind]=obs;
#if defined(CONFIG_MT_PAGED_SATELLITES)
        if(obs.utc)s->publicUtc=obs.utc;
#endif
#else
        (void)obs;
#endif
        s->channels[kind]=uint8_t(channel);return true;
    }
#if defined(CONFIG_MT_SATELLITE_HISTORY)
#if defined(CONFIG_MT_PAGED_SATELLITES)
    // Reconciliation walks kinds in slot order, which says nothing about the
    // receive order after a clock correction. Keep the trusted public header
    // separate from each report's own observation and private packet times.
    void publicHeader(const NodeCache &nodes,uint32_t id,uint32_t utc){
        if(utc)if(auto *s=source(nodes,id))s->publicUtc=utc;
    }
    uint32_t publicHeader(uint32_t id){auto *s=find(id);return s?s->publicUtc:0;}
#endif
    void rollback(uint32_t node,uint8_t mask){
        auto *s=find(node);if(!s)return;
        for(unsigned k=0;k<Done;++k)if(mask&(1u<<k)){s->channels[k]=0;s->received[k]={};}
        if(mask&(1u<<Environment)){discard(s->environment);s->environment=nullptr;s->length=0;}
    }
    Observation received(uint32_t node,Kind kind){auto *s=find(node);return s?s->received[kind]:Observation{};}
#endif
    unsigned channel(uint32_t node,Kind kind){auto *s=find(node);return s?s->channels[kind]:0;}
    const uint8_t *environment(uint32_t node,size_t &n){
        auto *s=find(node);n=s?s->length:0;return s?s->environment:nullptr;
    }
};
}
