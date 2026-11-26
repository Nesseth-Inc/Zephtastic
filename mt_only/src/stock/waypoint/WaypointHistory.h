// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "WaypointModule.h"
#include "../../ChannelRecord.h"
#include <errno.h>
#include <stddef.h>

namespace mt::stock::waypoint {
// Both board ABIs are little-endian. Persist fixed-width fields, never C++
// booleans, padding from live objects, channel keys or private peer keys.
class WaypointHistory {
public:
    struct Domain {
        uint32_t node, channelGeneration;
        uint8_t channelHash[16], publicKey[32];
    };
    struct Contacts { uint32_t generation; bool known; };
    static constexpr const char *Key="mtway/history";
    static constexpr uint64_t AutosaveMs=2ull*60*60*1000;
private:
    struct Row {
        uint32_t id, expire, lockedTo, creator, privatePeer;
        int32_t lat, lon, west, east, north, south;
        uint32_t radius;
        char name[sizeof(Entry::name)];
        uint8_t channel, notifications, geometry;
    };
    struct Record {
        Domain domain;
        uint32_t contacts;
        uint16_t crc;
        uint8_t version, count;
        Row rows[10];
    };
    static_assert(sizeof(Record)<1024 && sizeof(Domain)==56);
    static constexpr size_t Header=offsetof(Record,rows);
    static size_t size(unsigned count){return Header+sizeof(Row)*count;}
    static bool valid(const Row &r){
        return r.channel<8&&r.creator&&(r.notifications&~7u)==0&&(r.geometry&~3u)==0&&
               memchr(r.name,0,sizeof(r.name));
    }
    Contacts lastContacts_{};
    bool enabled_=false,hadData_=false;
    uint32_t observedChannel_=0;
    uint64_t next_=0;
public:
    void syncContacts(WaypointModule &w,Contacts contacts){
        if(lastContacts_.known==contacts.known&&lastContacts_.generation==contacts.generation)return;
        lastContacts_=contacts;
        const unsigned before=w.count_;
        for(unsigned i=0;i<w.count_;){
            if(w.entries_[w.order_[i]].privatePeer)w.remove(i);else ++i;
        }
        if(before!=w.count_)w.changed();
    }
    template<class Store> int load(Store &store,WaypointModule &w,const Domain &domain,Contacts contacts,uint32_t utc){
        // Startup only, before dispatch. A storage I/O failure leaves the
        // volatile owner usable but cannot authorize replacing an unread record.
        Record rec{};enabled_=false;lastContacts_=contacts;
        if(w.count_||!domain.node||domain.node!=w.local_)return -EINVAL;
        const int n=store.read(Key,reinterpret_cast<uint8_t*>(&rec),sizeof(rec));
        if(n<0&&n!=-ENOENT)return n;
        enabled_=true;hadData_=n>=0;observedChannel_=domain.channelGeneration;w.dirty_=false;
        if(n==-ENOENT)return 0;
        if(n<int(Header)||rec.version!=1||rec.count>10||size(rec.count)!=size_t(n))return -EILSEQ;
        const uint16_t crc=rec.crc;rec.crc=0;
        if(crc!=ChannelRecord::checksum(reinterpret_cast<const uint8_t*>(&rec),n))return -EILSEQ;
        if(memcmp(&rec.domain,&domain,sizeof(domain)))return 0;
        for(unsigned i=0;i<rec.count;++i){
            const auto &r=rec.rows[i];if(!valid(r))return -EILSEQ;
            for(unsigned j=0;j<i;++j){const auto &s=rec.rows[j];
                if(s.id==r.id&&s.channel==r.channel&&s.privatePeer==r.privatePeer)return -EILSEQ;}
        }
        for(unsigned i=0;i<rec.count;++i){
            const auto &r=rec.rows[i];
            if(!active(r.expire,utc)||(r.privatePeer&&(!contacts.known||contacts.generation!=rec.contacts)))continue;
            auto &e=w.entries_[w.order_[w.count_++]];
            e.id=r.id;e.expire=r.expire;e.lockedTo=r.lockedTo;e.creator=r.creator;e.privatePeer=r.privatePeer;
            e.channel=r.channel;e.notificationPreferences=r.notifications;memcpy(e.name,r.name,sizeof(e.name));
            auto &g=e.geometry;g.latitude=r.lat;g.longitude=r.lon;g.radius=r.radius;
            g.box.longitude_west_i=r.west;g.box.longitude_east_i=r.east;
            g.box.latitude_north_i=r.north;g.box.latitude_south_i=r.south;
            g.hasCenter=r.geometry&1;g.hasBox=r.geometry&2;
        }
        w.changed();w.dirty_=w.count_!=rec.count;
        return 0;
    }
    bool due(WaypointModule &w,uint32_t generation,uint64_t now){
        if(!enabled_)return false;
        if(!w.count_&&!hadData_){w.dirty_=false;next_=0;return false;}
        if(generation!=observedChannel_){observedChannel_=generation;w.dirty_=true;next_=now+1000;}
        if(!w.dirty_)return false;
        if(!next_)next_=now+AutosaveMs;
        return now>=next_;
    }
    void retry(uint64_t now){next_=now+60000;}
    template<class Store> int save(Store &store,WaypointModule &w,const Domain &domain,Contacts contacts,uint64_t now){
        if(!enabled_||!domain.node||domain.node!=w.local_)return -EINVAL;
        syncContacts(w,contacts);retry(now);
        Record rec{};rec.domain=domain;rec.version=1;rec.contacts=contacts.generation;
        for(unsigned i=0;i<w.count_;++i){
            const auto &e=w.entries_[w.order_[i]];
            if(e.privatePeer&&!contacts.known)continue;
            auto &r=rec.rows[rec.count++];const auto &g=e.geometry;
            r.id=e.id;r.expire=e.expire;r.lockedTo=e.lockedTo;r.creator=e.creator;r.privatePeer=e.privatePeer;
            r.channel=e.channel;r.notifications=e.notificationPreferences;memcpy(r.name,e.name,sizeof(r.name));
            r.lat=g.latitude;r.lon=g.longitude;r.radius=g.radius;r.west=g.box.longitude_west_i;r.east=g.box.longitude_east_i;
            r.north=g.box.latitude_north_i;r.south=g.box.latitude_south_i;r.geometry=uint8_t(g.hasCenter|(g.hasBox<<1));
        }
        auto *bytes=reinterpret_cast<uint8_t*>(&rec);const size_t length=size(rec.count);
        rec.crc=ChannelRecord::checksum(bytes,length);
        const int result=store.write(Key,bytes,length);
        if(!result){observedChannel_=domain.channelGeneration;hadData_=rec.count!=0;w.dirty_=false;next_=0;}
        return result;
    }
};
}
