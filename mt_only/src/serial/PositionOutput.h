// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "SerialEngine.h"

namespace mt_serial {
struct Position {
  bool valid=false;
  bool utc_valid=false;
  int32_t latitude_i=0;   // degrees * 1e7, Meshtastic wire unit
  int32_t longitude_i=0;  // degrees * 1e7, Meshtastic wire unit
  int32_t altitude_m=0;
  int32_t geoidal_separation_m=0;
  uint16_t hdop=0;
  uint8_t fix_quality=0;
  uint8_t satellites=0;
  uint8_t utc_hour=0, utc_minute=0, utc_second=0;
  int32_t timestamp_millis_adjust=0;
};
struct Waypoint {
  Position position{};
  char source_name[48]{};
};
using PositionWrite = void (*)(void *context, const uint8_t *bytes, size_t size);

// Stock NMEA/CalTopo timing and formatting. Position/time/name data are always
// supplied by the module owner; this code never invents a fix or source label.
class PositionOutput {
public:
  static size_t formatGga(const Position &position, char *out, size_t capacity);
  static size_t formatWpl(const Position &position, const char *sourceName,
                          Mode mode, char *out, size_t capacity,
                          bool localCalTopoListing=false);
  bool pollNmea(uint32_t nowMs, const Position &localPosition,
                PositionWrite write, void *context);
  size_t pollCalTopo(uint32_t nowMs, const Waypoint *waypoints, size_t count,
                     PositionWrite write, void *context);
  bool meshWaypoint(const Position &position, const char *sourceName,
                    Mode mode, PositionWrite write, void *context);

private:
  bool haveNmeaTime_=false, haveCalTopoTime_=false;
  uint32_t lastNmeaMs_=0, lastCalTopoMs_=0;
};
} // namespace mt_serial
