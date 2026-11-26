// SPDX-License-Identifier: GPL-3.0-only
// Response-hop selection follows Meshtastic d5f1619 NodeDB.cpp and RoutingModule.cpp.
// ZT supplies the decoded bitfield presence and validated packet/header values.
#pragma once
#include <stdint.h>

namespace mt::stock::routing {
inline int getHopsAway(unsigned start, unsigned remaining, bool decodedHasBitfield)
{
    if ((start == 0 && !decodedHasBitfield) || start > 7 || remaining > start)
        return -1;
    return int(start - remaining);
}

inline unsigned getHopLimitForResponse(unsigned start, unsigned remaining, unsigned configured,
                                       bool decodedHasBitfield)
{
    const int hopsUsed = getHopsAway(start, remaining, decodedHasBitfield);
    const unsigned responseHopLimit = configured < 7 ? configured : 7;
    if (hopsUsed >= 0) {
        if (unsigned(hopsUsed) > responseHopLimit)
            return unsigned(hopsUsed);
        if (start == 0)
            return 0;
        if (unsigned(hopsUsed + 2) < responseHopLimit)
            return unsigned(hopsUsed + 2);
    }
    return responseHopLimit;
}

template<class Known,class Unlicensed>
bool accepts(bool encrypted,uint8_t channel,uint32_t from,uint32_t to,unsigned mode,bool licensed,
             Known known,Unlicensed unlicensed)
{
    // Stock checks opaque LOCAL_ONLY/KNOWN_ONLY before the licensed branch.
    if(encrypted && (mode==2 || mode==3)) {
        const bool maybePKI=channel==0 && to!=UINT32_MAX;
        return maybePKI && (known(from) || known(to));
    }
    return !licensed || (!unlicensed(from) && !unlicensed(to));
}
}
