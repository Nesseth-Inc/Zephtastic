// SPDX-License-Identifier: GPL-3.0-only
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>

#include "serial/PositionOutput.h"
#include "meshtastic/mesh.pb.h"

namespace oracle_nmea {
struct meshtastic_PositionLite { int32_t latitude_i, longitude_i, altitude; };
struct GeoCoord {
    struct DMS { unsigned latDeg, latMin, latSec, lonDeg, lonMin, lonSec; char latCP,lonCP; } dms{};
    int32_t lat,lon,alt;
    static void latLongToDMS(double lat, double lon, DMS &out);
    GeoCoord(int32_t a,int32_t b,int32_t c): lat(a),lon(b),alt(c) { latLongToDMS(lat*1e-7,lon*1e-7,dms); }
    int getDMSLatDeg() const { return dms.latDeg; }
    int getDMSLonDeg() const { return dms.lonDeg; }
    int getLatitude() const { return lat; }
    int getLongitude() const { return lon; }
    int getAltitude() const { return alt; }
    char getDMSLatCP() const { return dms.latCP; }
    char getDMSLonCP() const { return dms.lonCP; }
};
enum class RTCQuality { RTCQualityDevice };
unsigned rtc;
int getRTCQuality() { return rtc!=0; }
uint32_t getValidTime(RTCQuality) { return rtc; }
#include "donor_nmea.inc"
}

void nmeaCases()
{
    const int32_t values[]={0,-1,1,-899999999,899999999,-900000000,900000000,-354125678,16666665,9999999,10000000,10000001};
    for (auto lat : values) for (int32_t lon : {-1800000000,1800000000,0,9999999,-898765432}) {
        mt_serial::Position p{};
        p.valid=true;p.latitude_i=lat;p.longitude_i=lon;p.altitude_m=-18;p.geoidal_separation_m=-2;
        p.hdop=124;p.fix_quality=3;p.satellites=9;p.utc_valid=true;
        meshtastic_Position stock{};
        stock.latitude_i=lat;stock.longitude_i=lon;stock.altitude=-18;stock.altitude_geoidal_separation=-2;
        stock.HDOP=124;stock.fix_quality=3;stock.sats_in_view=9;stock.timestamp=704;
        char expected[180], actual[180];
        for(auto rtc : {0u, 1710437552u}) for(auto adjust : {-123,0,35,999}) {
            oracle_nmea::rtc=rtc;
            const time_t stamp=rtc ? rtc : stock.timestamp;
            const auto t=*gmtime(&stamp);
            p.utc_hour=t.tm_hour;p.utc_minute=t.tm_min;p.utc_second=t.tm_sec;
            p.timestamp_millis_adjust=stock.timestamp_millis_adjust=adjust;
            const auto length=oracle_nmea::printGGA(expected,sizeof(expected),stock);
            assert(mt_serial::PositionOutput::formatGga(p,actual,sizeof(actual))==length && !strcmp(expected,actual));
        }
        for(bool cal : {false,true}) for(bool local : {false,true}) {
            const auto mode=cal ? mt_serial::Mode::CalTopo : mt_serial::Mode::Nmea;
            const oracle_nmea::meshtastic_PositionLite lite{lat,lon,-18};
            const auto length=local&&cal ? oracle_nmea::printWPL(expected,sizeof(expected),lite,"test___x",cal)
                                         : oracle_nmea::printWPL(expected,sizeof(expected),stock,"test___x",cal);
            assert(mt_serial::PositionOutput::formatWpl(p,"test,*\nx",mode,actual,sizeof(actual),local)==length);
            assert(!strcmp(actual,expected));
            assert(mt_serial::PositionOutput::formatWpl(p,"test___x",mode,actual,length,local)==0);
        }
    }
    puts("PASS NMEAWPL.cpp both position overloads and GGA, stock GeoCoord DMS, RTC/GPS, signed time adjustments, protected UART writes");
}
