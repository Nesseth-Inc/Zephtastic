// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, WaypointModule::handleReceived,
// WaypointStore::addFromPacket/mergeNotificationPreferences/addStoredWaypoint/purgeExpired, meshUtils.h.
#pragma once
#include <pb_decode.h>
#include "GeofenceModule.h"
#include "meshtastic/mesh.pb.h"
#include <string.h>
#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#else
#include <stdlib.h>
#endif

namespace mt::stock::waypoint {
inline bool active(uint32_t expire, uint32_t now)
{
    if (expire <= 1)
        return expire == 0;
    return now == 0 || expire > now;
}

struct Scope {
    uint32_t privatePeer; // Zero for channel traffic. PKI and shared-channel histories are distinct.
    uint8_t channel;
};

struct Entry {
    uint32_t id, expire, lockedTo, creator, privatePeer;
    Geometry geometry;
    uint8_t channel, notificationPreferences;
    char name[sizeof(meshtastic_Waypoint::name)];
};

enum class Result { Ignored, Deleted, Stored };

class WaypointModule {
    friend class WaypointHistory;
    // This display sink projects the name and admission fields it consumes.
    // Full coordinates, geofences and descriptions remain in the companion payload.
    Entry entries_[10]{};
    uint8_t order_[10]{};
    unsigned count_=0;
    uint32_t local_=0,generation_=0,lastFrom_=0,lastId_=0,lastPeer_=0;
    uint8_t lastChannel_=0;
    // Stock caps geofence state at 256 (node,waypoint) pairs, including a
    // single subscribed fence observed by 256 different nodes.
    static constexpr unsigned MaxTracked=256;
    struct Crossings {
        uint32_t nodes[MaxTracked];
        uint8_t slots[MaxTracked]; // Low bits select one of ten slots, bit 7 records inside.
    };
    Crossings *tracking_=nullptr;
    unsigned tracked_=0;
    static Crossings *allocateCrossings(){
#if defined(__ZEPHYR__)
        // Use the board's existing system heap; the T096 libc arena may be smaller.
        return static_cast<Crossings*>(k_malloc(sizeof(Crossings)));
#else
        return static_cast<Crossings*>(malloc(sizeof(Crossings)));
#endif
    }
    void freeCrossings(){
        if(tracking_){
#if defined(__ZEPHYR__)
            k_free(tracking_);
#else
            free(tracking_);
#endif
        }
        tracking_=nullptr;tracked_=0;
    }
    void forgetCrossingAt(unsigned i){
        --tracked_;tracking_->nodes[i]=tracking_->nodes[tracked_];tracking_->slots[i]=tracking_->slots[tracked_];
    }
    bool subscriptions_=false;
    bool dirty_=false;
    void changed() {
        dirty_=true;
        subscriptions_=false;
        for(unsigned i=0;i<count_;++i)if((entries_[order_[i]].notificationPreferences&3)&&entries_[order_[i]].geometry.eligible()){
            subscriptions_=true;break;
        }
    }

    void clearCrossings(unsigned slot){
        for(unsigned i=0;i<tracked_;){
            if((tracking_->slots[i]&0x7f)==slot)forgetCrossingAt(i);else ++i;
        }
        if(!tracked_)freeCrossings();
    }
    void remove(unsigned index){
        const unsigned slot=order_[index];
        clearCrossings(slot);entries_[slot]={};
        --count_;memmove(order_+index,order_+index+1,count_-index);
        order_[count_]=uint8_t(slot);
    }

public:
    WaypointModule()=default;
    WaypointModule(const WaypointModule &)=delete;
    WaypointModule &operator=(const WaypointModule &)=delete;
    ~WaypointModule(){freeCrossings();}
    bool subscribed() const {return subscriptions_;}
    unsigned count() const {return count_;}
    const Entry *newest() const {return count_ ? &entries_[order_[0]] : nullptr;}
    bool domain(uint32_t local, uint32_t generation)
    {
        if(local_==local && generation_==generation)return false;
        memset(entries_,0,sizeof(entries_));
        count_=0;freeCrossings();for(unsigned i=0;i<10;++i)order_[i]=uint8_t(i);
        changed();lastId_=0;local_=local;generation_=generation;
        return true;
    }
    void forgetPeer(uint32_t node){
        if(!node)return;
        const unsigned before=count_;
        for(unsigned i=0;i<count_;){
            if(entries_[order_[i]].privatePeer==node)remove(i);else ++i;
        }
        for(unsigned i=0;i<tracked_;){
            if(tracking_->nodes[i]==node)forgetCrossingAt(i);else ++i;
        }
        if(!tracked_)freeCrossings();
        if(lastFrom_==node||lastPeer_==node)lastId_=0;
        if(before!=count_)changed();
    }
    void purgeExpired(uint32_t utc)
    {
        const unsigned before=count_;
        for(unsigned i=0;i<count_;){
            if(!active(entries_[order_[i]].expire,utc))remove(i);else ++i;
        }
        if(before!=count_)changed();
    }
    Result handleReceived(uint32_t from,uint32_t packetId,Scope scope,bool locallyAuthored,
                          const uint8_t *payload,size_t length,uint32_t utc)
    {
        if(!local_ || !payload || !length || !from || !packetId || scope.channel>=8)return Result::Ignored;
        // A single accepted frame fans out to up to four Companion objects synchronously.
        if(lastId_==packetId && lastFrom_==from && lastPeer_==scope.privatePeer && lastChannel_==scope.channel)
            return Result::Ignored;
        meshtastic_Waypoint wp{};
        auto in=pb_istream_from_buffer(payload,length);
        if(!pb_decode(&in,meshtastic_Waypoint_fields,&wp) || in.bytes_left)return Result::Ignored;
        lastId_=packetId;lastFrom_=from;lastPeer_=scope.privatePeer;lastChannel_=scope.channel;
        purgeExpired(utc);
        unsigned i=0;
        for(;i<count_;++i){
            const auto &e=entries_[order_[i]];
            if(e.id==wp.id&&e.channel==scope.channel&&e.privatePeer==scope.privatePeer)break;
        }
        const bool exists=i<count_;
        const Geometry geometry{wp.latitude_i,wp.longitude_i,wp.geofence_radius,wp.bounding_box,
                                wp.has_bounding_box,wp.has_latitude_i&&wp.has_longitude_i};
        const Entry *old=exists?&entries_[order_[i]]:nullptr;
        const bool sameGeometry=exists&&old->geometry.same(geometry);
        if(exists&&old->lockedTo&&old->lockedTo!=from)return Result::Ignored;
        if(exists&&!locallyAuthored&&(old->notificationPreferences&3)&&
           (!sameGeometry||old->lockedTo!=wp.locked_to||old->expire!=wp.expire||strcmp(old->name,wp.name)))
            return Result::Ignored;
        if(!active(wp.expire,utc)){
            if(!exists)return Result::Ignored;
            remove(i);changed();return Result::Deleted;
        }
        const uint8_t flags=locallyAuthored ? uint8_t(wp.notify_on_enter|(wp.notify_on_exit<<1)|
                                                     (wp.notify_favorites_only<<2)) :
                                            (exists ? old->notificationPreferences : 0);
        if(!exists&&count_==10){
            // RF churn cannot evict locally subscribed geofences.
            i=count_;
            do {--i;} while(i && !locallyAuthored && (entries_[order_[i]].notificationPreferences&3));
            if(!locallyAuthored&&(entries_[order_[i]].notificationPreferences&3))return Result::Ignored;
            remove(i);i=count_;
        }
        const unsigned slot=order_[i];
        if(exists&&(!sameGeometry||old->notificationPreferences!=flags))clearCrossings(slot);
        if(!exists)++count_;
        memmove(order_+1,order_,i);order_[0]=uint8_t(slot);
        auto &entry=entries_[slot];
        entry.id=wp.id;entry.expire=wp.expire;entry.lockedTo=wp.locked_to;entry.creator=from;
        entry.privatePeer=scope.privatePeer;entry.channel=scope.channel;entry.notificationPreferences=flags;
        memcpy(entry.name,wp.name,sizeof(entry.name));
        entry.geometry=geometry;
        changed();
        return Result::Stored;
    }
    template<class Notify> void evaluatePosition(uint32_t from,Scope scope,const uint8_t *bytes,size_t size,
                                                uint32_t utc,bool favorite,Notify notify){
        if(!subscriptions_ || !from || from==local_ || !bytes || !size)return;
        meshtastic_Position position{};auto stream=pb_istream_from_buffer(bytes,size);
        if(!pb_decode(&stream,meshtastic_Position_fields,&position)||stream.bytes_left||
           !position.has_latitude_i||!position.has_longitude_i||(!position.latitude_i&&!position.longitude_i))return;
        for(unsigned i=0;i<count_;++i){
            const unsigned slot=order_[i];const auto &e=entries_[slot];const auto flags=e.notificationPreferences;
            if(!(flags&3)||!e.geometry.eligible()||e.channel!=scope.channel ||
               e.privatePeer!=scope.privatePeer||!active(e.expire,utc))continue;
            unsigned j=0;while(j<tracked_&&(tracking_->nodes[j]!=from||(tracking_->slots[j]&0x7f)!=slot))++j;
            const bool inside=e.geometry.inside(position.latitude_i,position.longitude_i);
            if(j==tracked_){
                if(tracked_==MaxTracked)continue;
                if(!tracking_){
                    tracking_=allocateCrossings();
                    if(!tracking_)return;
                }
                tracking_->nodes[tracked_]=from;tracking_->slots[tracked_++]=uint8_t(slot|(inside?0x80:0));
                continue;
            }
            const auto crossing=classify(false,tracking_->slots[j]&0x80,inside,flags&1,flags&2);
            tracking_->slots[j]=uint8_t(slot|(inside?0x80:0));
            if(crossing!=Crossing::None&&(!(flags&4)||favorite))notify(e,crossing);
        }
    }
};
}
