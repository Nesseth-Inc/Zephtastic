// SPDX-License-Identifier: GPL-3.0-only
// Port of src/gps/NMEAWPL.cpp at Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c.
#include "NMEAWPL.h"
#include <stdio.h>
#include <string.h>

namespace mt::stock::nmea {
namespace {
struct Coordinate {
    int degree;
    double minutes;
    char cardinal;
};

Coordinate coordinate(int32_t wire, bool latitude)
{
    const int32_t absolute = wire < 0 ? -wire : wire;
    const int degree = int(absolute * 1e-7); // GeoCoord::latLongToDMS floor on positive degrees
    return {degree, (absolute - degree * 1e7) * 6e-6,
            latitude ? (wire < 0 ? 'S' : 'N') : (wire < 0 ? 'W' : 'E')};
}

size_t finish(char *buf, size_t capacity, int written)
{
    // The ZT UART owner only sends complete sentences.
    if (written < 0 || size_t(written) >= capacity)
        return 0;
    unsigned checksum = 0;
    const char *c = strchr(buf, '$');
    for (++c; *c && *c != '*'; ++c)
        checksum ^= static_cast<uint8_t>(*c);
    const int suffix = snprintf(buf + written, capacity - written, "*%02X\r\n", checksum);
    return suffix >= 0 && size_t(suffix) < capacity - written ? size_t(written + suffix) : 0;
}
}

size_t printWPL(char *buf, size_t capacity, const mt_serial::Position &pos, const char *name,
                bool calTopo, bool localListing)
{
    const auto lat = coordinate(pos.latitude_i, true), lon = coordinate(pos.longitude_i, false);
    const int len = snprintf(buf, capacity, "%s$G%cWPL,%02d%07.4f,%c,%03d%07.4f,%c,%s",
                             localListing ? "\r\n" : "", calTopo ? 'P' : 'N',
                             lat.degree, lat.minutes, lat.cardinal, lon.degree, lon.minutes, lon.cardinal, name);
    return finish(buf, capacity, len);
}

size_t printGGA(char *buf, size_t capacity, const mt_serial::Position &pos)
{
    const auto lat = coordinate(pos.latitude_i, true), lon = coordinate(pos.longitude_i, false);
    const int len = snprintf(buf, capacity,
        "$GNGGA,%02d%02d%02d.%02d,%02d%07.4f,%c,%03d%07.4f,%c,%u,%02u,%04u,%04d,%c,%04d,%c,%d,%04d",
        pos.utc_hour, pos.utc_minute, pos.utc_second, int(pos.timestamp_millis_adjust),
        lat.degree, lat.minutes, lat.cardinal, lon.degree, lon.minutes, lon.cardinal,
        unsigned(pos.fix_quality), unsigned(pos.satellites), unsigned(pos.hdop),
        int(pos.altitude_m), 'M', int(pos.geoidal_separation_m), 'M', 0, 0);
    return finish(buf, capacity, len);
}
}
