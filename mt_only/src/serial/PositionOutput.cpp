// SPDX-License-Identifier: GPL-3.0-only
#include "PositionOutput.h"

#include "../stock/peripherals/NMEAWPL.h"
#include <string.h>

namespace mt_serial {
namespace {
bool coordinates(const Position &p) {
  return p.valid && p.latitude_i>=-900000000 && p.latitude_i<=900000000 &&
         p.longitude_i>=-1800000000 && p.longitude_i<=1800000000;
}
void writeBytes(PositionWrite write, void *context, const char *data, size_t size) {
  if(write && size) write(context,reinterpret_cast<const uint8_t *>(data),size);
}
}

size_t PositionOutput::formatGga(const Position &p, char *out, size_t capacity) {
  if(!out || capacity==0 || !coordinates(p) || !p.utc_valid ||
     p.utc_hour>23 || p.utc_minute>59 || p.utc_second>60)
    return 0;
  return mt::stock::nmea::printGGA(out,capacity,p);
}

size_t PositionOutput::formatWpl(const Position &p, const char *sourceName,
                                 Mode mode, char *out, size_t capacity,
                                 bool localCalTopoListing) {
  if(!out || capacity==0 || !coordinates(p) ||
     (mode!=Mode::Nmea && mode!=Mode::CalTopo)) return 0;
  const char *name=sourceName ? sourceName : "";
  char safeName[48]; size_t n=0;
  while(name[n] && n<sizeof(safeName)-1) {
    const unsigned char c=static_cast<unsigned char>(name[n]);
    safeName[n]=(c<0x20 || c==',' || c=='*') ? '_' : char(c);
    ++n;
  }
  safeName[n]='\0';
  const bool caltopo=mode==Mode::CalTopo;
  return mt::stock::nmea::printWPL(out,capacity,p,safeName,caltopo,caltopo && localCalTopoListing);
}

bool PositionOutput::pollNmea(uint32_t nowMs, const Position &position,
                              PositionWrite write, void *context) {
  if(!haveNmeaTime_) { haveNmeaTime_=true; lastNmeaMs_=nowMs; return false; }
  if(static_cast<uint32_t>(nowMs-lastNmeaMs_)<2000) return false;
  lastNmeaMs_=nowMs;
  char sentence[128];
  const size_t length=formatGga(position,sentence,sizeof(sentence));
  if(!length || !write) return false;
  writeBytes(write,context,sentence,length);
  return true;
}

size_t PositionOutput::pollCalTopo(uint32_t nowMs, const Waypoint *waypoints,
                                   size_t count, PositionWrite write, void *context) {
  if(!haveCalTopoTime_) { haveCalTopoTime_=true; lastCalTopoMs_=nowMs; return 0; }
  if(static_cast<uint32_t>(nowMs-lastCalTopoMs_)<10000 || !write) return 0;
  lastCalTopoMs_=nowMs;
  if(!waypoints) return 0;
  size_t written=0;
  if(count>64) count=64;
  char sentence[128];
  for(size_t i=0;i<count;++i) {
    const size_t length=formatWpl(waypoints[i].position,waypoints[i].source_name,
                                  Mode::CalTopo,sentence,sizeof(sentence),true);
    if(length) { writeBytes(write,context,sentence,length); ++written; }
  }
  return written;
}

bool PositionOutput::meshWaypoint(const Position &position, const char *sourceName,
                                  Mode mode, PositionWrite write, void *context) {
  if((mode!=Mode::Nmea && mode!=Mode::CalTopo) || !write) return false;
  char sentence[128];
  const size_t length=formatWpl(position,sourceName,mode,sentence,sizeof(sentence));
  if(!length) return false;
  writeBytes(write,context,sentence,length);
  return true;
}
} // namespace mt_serial
