// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d RoutingModule.cpp. The adapter supplies validated RF
// headers and node observations. Unknown license status is distinct from false.
#pragma once
#include <stdint.h>

namespace mt::stock::routing {
inline unsigned getHopLimitForResponse(unsigned start,unsigned remaining,unsigned configured)
{
    const unsigned hopsUsed=start-remaining;
    if(hopsUsed>configured)return hopsUsed;
    if(!start)return 0;
    return hopsUsed+2<configured ? hopsUsed+2 : configured;
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
