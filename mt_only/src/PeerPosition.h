// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <string.h>

namespace mt {
// An untrusted peer's most recently admitted Position, in RAM only. Keep all
// scalar values and optional-field presence without a target enum/alignment
// dependency. This is neither local positioning nor a durable NodeDB record.
class PeerPosition {
  // Seven optional flags, two validated enum numbers, then 21 four-byte scalar
  // slots. memcpy preserves signed scalar bits without unaligned typed loads.
  uint8_t bytes_[87]{};
  template<class Value> void put(unsigned slot,const Value &value) {
    static_assert(sizeof(Value)==4,"Position scalar width");
    memcpy(bytes_+3+slot*4,&value,4);
  }
  template<class Value> void get(unsigned slot,Value &value)const {
    static_assert(sizeof(Value)==4,"Position scalar width");
    memcpy(&value,bytes_+3+slot*4,4);
  }
  template<class Enum> static uint32_t enumNumber(const Enum &value) {
    static_assert(sizeof(Enum)==1||sizeof(Enum)==2||sizeof(Enum)==4);
    if constexpr(sizeof(Enum)==1){uint8_t n;memcpy(&n,&value,1);return n;}
    else if constexpr(sizeof(Enum)==2){uint16_t n;memcpy(&n,&value,2);return n;}
    else {uint32_t n;memcpy(&n,&value,4);return n;}
  }
public:
  bool present()const {return (bytes_[0]&3u)==3u;}
  // Canonical protobuf/packet admission belongs to the receive caller. Validate
  // the same coordinate/source/precision bounds before atomically replacing
  // the whole snapshot. Absent optional values clear their prior slots.
  template<class Position> bool assign(const Position &p) {
    const uint32_t location=enumNumber(p.location_source),altitude=enumNumber(p.altitude_source);
    if(!p.has_latitude_i||!p.has_longitude_i||p.latitude_i < -900000000||p.latitude_i>900000000||
       p.longitude_i < -1800000000||p.longitude_i>1800000000||p.precision_bits>32||location>3||altitude>4)return false;
    PeerPosition next;
    next.bytes_[0]=3u|(p.has_altitude?4u:0u)|(p.has_altitude_hae?8u:0u)|
      (p.has_altitude_geoidal_separation?16u:0u)|(p.has_ground_speed?32u:0u)|(p.has_ground_track?64u:0u);
    next.bytes_[1]=uint8_t(location);next.bytes_[2]=uint8_t(altitude);
    next.put(0,p.latitude_i);next.put(1,p.longitude_i);
    if(p.has_altitude)next.put(2,p.altitude);
    next.put(3,p.time);next.put(4,p.timestamp);next.put(5,p.timestamp_millis_adjust);
    if(p.has_altitude_hae)next.put(6,p.altitude_hae);
    if(p.has_altitude_geoidal_separation)next.put(7,p.altitude_geoidal_separation);
    next.put(8,p.PDOP);next.put(9,p.HDOP);next.put(10,p.VDOP);next.put(11,p.gps_accuracy);
    if(p.has_ground_speed)next.put(12,p.ground_speed);
    if(p.has_ground_track)next.put(13,p.ground_track);
    next.put(14,p.fix_quality);next.put(15,p.fix_type);next.put(16,p.sats_in_view);
    next.put(17,p.sensor_id);next.put(18,p.next_update);next.put(19,p.seq_number);next.put(20,p.precision_bits);
    *this=next;return true;
  }
  void retainFixTime(const PeerPosition &previous){
    uint32_t time;get(3,time);if(!time){previous.get(3,time);put(3,time);}
  }
  // Tagged positions reconstructed by PhoneAPI use the stock PositionLite projection.
  template<class Position> void exportReplay(Position &p)const {
    p=Position{};if(!present())return;
    get(0,p.latitude_i);get(1,p.longitude_i);get(2,p.altitude);get(3,p.time);get(20,p.precision_bits);
    p.has_latitude_i=p.latitude_i!=0;p.has_longitude_i=p.longitude_i!=0;p.has_altitude=p.altitude!=0;
    p.location_source=static_cast<decltype(p.location_source)>(bytes_[1]);
    if(!p.precision_bits)p.precision_bits=32;
  }
  template<class Position> void exportTo(Position &p)const {
    p=Position{};if(!present())return;
    const uint8_t flags=bytes_[0];
    p.has_latitude_i=true;p.has_longitude_i=true;p.has_altitude=flags&4u;
    p.has_altitude_hae=flags&8u;p.has_altitude_geoidal_separation=flags&16u;
    p.has_ground_speed=flags&32u;p.has_ground_track=flags&64u;
    p.location_source=static_cast<decltype(p.location_source)>(bytes_[1]);
    p.altitude_source=static_cast<decltype(p.altitude_source)>(bytes_[2]);
    get(0,p.latitude_i);get(1,p.longitude_i);get(2,p.altitude);
    get(3,p.time);get(4,p.timestamp);get(5,p.timestamp_millis_adjust);
    get(6,p.altitude_hae);get(7,p.altitude_geoidal_separation);
    get(8,p.PDOP);get(9,p.HDOP);get(10,p.VDOP);get(11,p.gps_accuracy);
    get(12,p.ground_speed);get(13,p.ground_track);get(14,p.fix_quality);get(15,p.fix_type);
    get(16,p.sats_in_view);get(17,p.sensor_id);get(18,p.next_update);get(19,p.seq_number);get(20,p.precision_bits);
  }
};
static_assert(sizeof(PeerPosition)==87,"Volatile peer Position RAM budget");
}
