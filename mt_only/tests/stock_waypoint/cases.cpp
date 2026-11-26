// SPDX-License-Identifier: GPL-3.0-only
#include "stock/waypoint/WaypointHistory.h"
#include <vector>
#include "stock/peripherals/StatusLEDModule.h"
#include <pb_encode.h>
#include <cassert>
#include <cstdio>
#include <deque>

namespace donor {
uint32_t clock;
enum class RTCQuality{RTCQualityDevice};
uint32_t getTime(){return clock;}
uint32_t getValidTime(RTCQuality){return clock;}
auto getFrom(const meshtastic_MeshPacket *p){return p->from;}
bool decodeWaypointPayload(const uint8_t *p,size_t size,meshtastic_Waypoint &w){auto s=pb_istream_from_buffer(p,size);return pb_decode(&s,meshtastic_Waypoint_fields,&w);}
struct StoredWaypoint {meshtastic_Waypoint waypoint{};uint32_t receivedTime{},creatorNodeNum{};uint8_t notificationPreferences{};};
enum {WAYPOINT_NOTIFY_ENTER=1,WAYPOINT_NOTIFY_EXIT=2,WAYPOINT_NOTIFY_FAVORITES_ONLY=4,WAYPOINT_HISTORY_LIMIT=10};
struct WaypointStore {
    std::deque<StoredWaypoint> waypoints;
    static bool isExpired(const meshtastic_Waypoint &,uint32_t=0);
    static bool isExpired(const StoredWaypoint &,uint32_t=0);
    static uint8_t notificationPreferencesFromWaypoint(const meshtastic_Waypoint&);
    static uint8_t mergeNotificationPreferences(bool,bool,uint8_t,const meshtastic_Waypoint&);
    static void clearWireNotificationPreferences(meshtastic_Waypoint&);
    const StoredWaypoint *findWaypoint(uint32_t)const;
    bool removeWaypointById(uint32_t);
    bool removeWaypoint(uint32_t);
    void addStoredWaypoint(const StoredWaypoint&);
    bool addFromPacket(const meshtastic_MeshPacket&,bool,StoredWaypoint*);
    bool purgeExpired(uint32_t);
    void notifyChanged(){}
};
constexpr double PI=3.14159265358979323846,DEG_CONVERT=180/PI;
struct GeoCoord {static float latLongToMeter(double,double,double,double);};
struct GeofenceModule {
    enum class Crossing{None,Enter,Exit};
    static bool insideRadius(int32_t,int32_t,int32_t,int32_t,uint32_t);
    static bool insideBox(int32_t,int32_t,const meshtastic_BoundingBox&);
    static bool insideAny(int32_t,int32_t,int32_t,int32_t,uint32_t,bool,const meshtastic_BoundingBox&);
    static bool inside(const meshtastic_Waypoint&,int32_t,int32_t);
    static bool hasGeofence(const meshtastic_Waypoint&);
    static Crossing classify(bool,bool,bool,bool,bool);
    static bool shouldTrack(const meshtastic_Waypoint&,uint8_t,uint32_t);
};
enum {LED_LORA=1,LED_STATE_ON=1,LED_STATE_OFF=0,LORA_RX_LED_FLASH_MS=100};
bool physical;
void digitalWrite(int,bool level){physical=level;}
uint32_t millis(){return clock;}
struct StatusLEDModule{
    bool LORA_LED_state=false;uint32_t LORA_LED_starttime=0;
    int handleLoRaRx(uint32_t);
    void setIntervalFromNow(int){}
    void poll();
};
#include "stock.inc"
}
void endpointCases();
using namespace mt::stock::waypoint;
int main(){
    donor::WaypointStore stock;
    WaypointModule module;
    module.domain(27,1);
    meshtastic_Waypoint wp{};strcpy(wp.name,"North field");wp.id=76;
    wp.notify_on_enter=wp.notify_on_exit=true;wp.notify_favorites_only=true;
    wp.has_bounding_box=true;
    wp.bounding_box.latitude_north_i=wp.bounding_box.longitude_east_i=210000000;
    wp.bounding_box.latitude_south_i=wp.bounding_box.longitude_west_i=190000000;
    meshtastic_MeshPacket packet{};
    auto send=[&](bool local,uint32_t from,uint32_t id,Scope scope={0,0}){
        packet.from=from;packet.id=id;
        auto &body=packet.decoded.payload;auto o=pb_ostream_from_buffer(body.bytes,sizeof(body.bytes));
        assert(pb_encode(&o,meshtastic_Waypoint_fields,&wp));body.size=o.bytes_written;
        return module.handleReceived(from,id,scope,local,body.bytes,body.size,donor::clock);
    };
    // Reboot with the same identity, channel settings and contact roster; RF
    // geofence crossings start with a new baseline after recovery.
    struct Store {
        std::vector<uint8_t> bytes;int readError=0,writeError=0;
        int read(const char *key,uint8_t *dst,size_t cap){
            assert(!strcmp(key,WaypointHistory::Key));if(readError)return readError;
            if(bytes.empty())return -ENOENT;if(bytes.size()>cap)return -E2BIG;
            memcpy(dst,bytes.data(),bytes.size());return int(bytes.size());
        }
        int write(const char *,const uint8_t *src,size_t n){
            if(writeError)return writeError;bytes.assign(src,src+n);return 0;
        }
    } disk;
    WaypointHistory::Domain binding{27,9,{1},{2}};
    WaypointHistory::Contacts peers{3,true};WaypointHistory journal;
    assert(journal.load(disk,module,binding,peers,0)==0);
    assert(send(true,27,100,{0,1})==Result::Stored);
    assert(send(true,27,101,{28,1})==Result::Stored);
    assert(!journal.due(module,9,100));
    assert(journal.due(module,9,100+WaypointHistory::AutosaveMs));
    disk.writeError=-EIO;assert(journal.save(disk,module,binding,peers,1000)==-EIO);
    assert(!journal.due(module,9,60999)&&journal.due(module,9,61000));
    disk.writeError=0;assert(journal.save(disk,module,binding,peers,61000)==0);
    auto restore=[&](auto identity,auto roster,unsigned count,int rc=0){
        WaypointModule cold; cold.domain(identity.node,1);WaypointHistory restarted;
        assert(restarted.load(disk,cold,identity,roster,0)==rc);assert(cold.count()==count);
        if(count){assert(cold.subscribed()&&cold.newest()->id==wp.id&&cold.newest()->notificationPreferences==7);
            assert(cold.newest()->geometry.hasBox&&!strcmp(cold.newest()->name,wp.name));}
    };
    restore(binding,peers,2);restore(binding,WaypointHistory::Contacts{4,true},1);
    restore(binding,WaypointHistory::Contacts{0,false},1);
    auto mismatch=binding;mismatch.channelGeneration++;restore(mismatch,peers,0);
    mismatch=binding;mismatch.channelHash[1]^=1;restore(mismatch,peers,0);
    mismatch=binding;mismatch.publicKey[3]^=1;restore(mismatch,peers,0);
    for(size_t i=0;i<disk.bytes.size();++i){disk.bytes[i]^=1;restore(binding,peers,0,-EILSEQ);disk.bytes[i]^=1;}
    disk.readError=-EIO;restore(binding,peers,0,-EIO);disk.readError=0;
    journal.syncContacts(module,{4,true});assert(module.count()==1&&module.newest()->privatePeer==0);
    module.domain(27,2);
    assert(send(false,28,1)==Result::Stored);stock.addFromPacket(packet,false,nullptr);
    assert(module.newest()->notificationPreferences==0 && !stock.waypoints.front().notificationPreferences);
    assert(send(true,27,2)==Result::Stored);stock.addFromPacket(packet,true,nullptr);
    assert(module.newest()->notificationPreferences==stock.waypoints.front().notificationPreferences);
    wp.notify_on_enter=wp.notify_on_exit=wp.notify_favorites_only=false;
    assert(send(false,28,3)==Result::Stored);stock.addFromPacket(packet,false,nullptr);
    assert(send(false,28,3)==Result::Ignored && module.count()==1);
    std::deque<Crossing> notices;
    auto fix=[&](int32_t raw,bool favorite,Scope scope=Scope{0,0},uint32_t peer=28){
        meshtastic_Position p{};p.has_latitude_i=p.has_longitude_i=true;p.latitude_i=p.longitude_i=raw;
        uint8_t bytes[80];auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&out,meshtastic_Position_fields,&p));
        module.evaluatePosition(peer,scope,bytes,out.bytes_written,donor::clock,favorite,[&](const Entry&e,Crossing c){assert(e.id==76);notices.push_back(c);});
    };
    for(bool circle:{false,true}){
        wp.has_bounding_box=!circle;wp.has_latitude_i=wp.has_longitude_i=circle;wp.latitude_i=wp.longitude_i=200000000;wp.geofence_radius=circle?1000:0;
        wp.notify_on_enter=wp.notify_on_exit=wp.notify_favorites_only=true;
        assert(send(true,27,4+unsigned(circle))==Result::Stored);stock.addFromPacket(packet,true,nullptr);
        notices.clear();bool first=true,previous=false;
        for(auto raw:{100000000,200000000,201000000,220000000,200000000}) {
            const auto &s=stock.waypoints.front();
            assert(donor::GeofenceModule::shouldTrack(s.waypoint,s.notificationPreferences,donor::clock));
            const bool inside=donor::GeofenceModule::inside(s.waypoint,raw,raw);
            auto predicted=donor::GeofenceModule::classify(first,previous,inside,true,true);
            first=false;previous=inside;
            const auto n=notices.size();fix(raw,true);
            assert(notices.size()==n+(predicted!=donor::GeofenceModule::Crossing::None));
            if(notices.size()>n)assert(int(notices.back())==int(predicted));
        }
    }
    notices.clear();fix(100000000,false);fix(200000000,false);fix(100000000,true,{0,1});
    fix(200000000,true,{29,0},29);fix(100000000,true,{0,0},27);assert(notices.empty());
    // A reorder or unrelated replacement must not erase an established peer baseline.
    auto const original=wp;
    wp.id=321;wp.notify_on_enter=wp.notify_on_exit=wp.notify_favorites_only=false;
    assert(send(false,28,15)==Result::Stored);
    wp=original;assert(send(false,28,16)==Result::Stored);
    wp.geofence_radius=1;assert(send(false,28,17)==Result::Ignored);
    wp=original;wp.expire=1;assert(send(false,28,18)==Result::Ignored);
    for(unsigned id=500;id<520;++id){wp={};wp.id=id;assert(send(false,28,id)==Result::Stored);}
    assert(module.count()==10);
    notices.clear();fix(100000000,true);assert(notices.size()==1&&notices[0]==Crossing::Exit);
    wp=original;wp.id=44;wp.notify_on_enter=wp.notify_on_exit=wp.notify_favorites_only=false;
    assert(send(false,28,54,{28,0})==Result::Stored);
    const auto beforeForget=module.count();module.forgetPeer(28);
    assert(module.count()+1==beforeForget);
    notices.clear();fix(200000000,true);
    assert(notices.empty()); // forgotten peer gets a new silent first sighting on the surviving public fence
    fix(100000000,true);assert(notices.size()==1&&notices[0]==Crossing::Exit);
    // The stock bound counts pairs. One fence supports 256 peers, preserves
    // established states at capacity, and reuses a slot after retirement.
    module.domain(27,4);wp=original;assert(send(true,27,77)==Result::Stored);
    notices.clear();
    for(unsigned i=0;i<257;++i)fix(100000000,true,{0,0},1000+i);
    for(unsigned i=0;i<257;++i)fix(200000000,true,{0,0},1000+i);
    assert(notices.size()==256);
    module.forgetPeer(1005);
    fix(100000000,true,{0,0},1256);fix(200000000,true,{0,0},1256);assert(notices.size()==257);
    // A separate fresh domain exercises the stock lock/tombstone and ordinary ten-entry eviction.
    module.domain(27,3);stock.waypoints.clear();wp=original;
    wp.locked_to=27;send(true,27,22);stock.addFromPacket(packet,true,nullptr);
    wp.expire=1;
    assert(send(false,28,23)==Result::Ignored);stock.addFromPacket(packet,false,nullptr);
    assert(module.count()==1&&stock.waypoints.size()==1);
    assert(send(true,27,24)==Result::Deleted);stock.addFromPacket(packet,true,nullptr);
    assert(!module.count()&&stock.waypoints.empty());
    wp.expire=200;wp.locked_to=0;
    for(unsigned i=1;i<15;++i){wp.id=i;assert(send(false,28,30+i)==Result::Stored);stock.addFromPacket(packet,false,nullptr);}
    assert(module.count()==10&&stock.waypoints.size()==10&&module.newest()->id==stock.waypoints.front().waypoint.id);
    module.purgeExpired(0);stock.purgeExpired(0);assert(module.count()==10);
    module.purgeExpired(200);stock.purgeExpired(200);assert(!module.count()&&stock.waypoints.empty());
    mt::stock::StatusLedRadio led;donor::StatusLEDModule orig;
    for(uint32_t t:{0u,UINT32_MAX-23}){donor::clock=t;orig.handleLoRaRx(0);led.handleLoRaRx(t);
        for(unsigned dt:{0u,49u,99u,100u,101u}){donor::clock=t+dt;orig.poll();assert(led.runOnce(donor::clock)==donor::physical);}}
    printf("PASS stock Waypoint ownership, recency, local-only preferences, expiry, circle/box geofence crossings and StatusLED; waypoint module %zu B\n",sizeof(module));
    endpointCases();
}
