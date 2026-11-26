// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d, GeofenceModule::insideAny/classify/evaluatePosition.
#pragma once
#include "../modules/RangeTestDistance.h"
#include "meshtastic/mesh.pb.h"
namespace mt::stock::waypoint {
struct Geometry {
    int32_t latitude,longitude;
    uint32_t radius;
    meshtastic_BoundingBox box;
    bool hasBox,hasCenter;
    bool same(const Geometry &g) const {
        return latitude==g.latitude&&longitude==g.longitude&&radius==g.radius&&hasBox==g.hasBox&&hasCenter==g.hasCenter&&
               (!hasBox||(box.longitude_west_i==g.box.longitude_west_i&&box.longitude_east_i==g.box.longitude_east_i&&
                          box.latitude_south_i==g.box.latitude_south_i&&box.latitude_north_i==g.box.latitude_north_i));
    }
    bool eligible() const {return (radius||hasBox)&&(!radius||hasCenter);}
    bool inside(int32_t lat,int32_t lon) const {
        if(radius&&range::latLongToMeter(lat*1e-7,lon*1e-7,latitude*1e-7,longitude*1e-7)<=float(radius))return true;
        return hasBox && lat>=box.latitude_south_i && lat<=box.latitude_north_i &&
               lon>=box.longitude_west_i && lon<=box.longitude_east_i;
    }
};

enum class Crossing {None,Enter,Exit};
inline Crossing classify(bool first,bool previous,bool inside,bool enter,bool exit)
{
    if(first||previous==inside)return Crossing::None;
    return inside ? (enter?Crossing::Enter:Crossing::None) : (exit?Crossing::Exit:Crossing::None);
}
}
