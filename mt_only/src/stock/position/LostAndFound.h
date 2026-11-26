// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
namespace mt::stock::position {
// Stock uses printf("%f", wire*1e-7). Splitting the binary64 significand
// keeps each product by 1,000,000 exact when resolving a decimal tie, without
// pulling a floating-point printf implementation into the T096 image.
inline void coordinate6(int32_t wire,char (&out)[18]){
    const uint32_t n=wire<0?uint32_t(-int64_t(wire)):uint32_t(wire);
    uint32_t micro=n/10;
    if(n%10>5)++micro;
    if(n%10==5){
        const double value=double(n)*1e-7;
        uint64_t bits;memcpy(&bits,&value,sizeof(bits));bits&=~((UINT64_C(1)<<27)-1);
        double high;memcpy(&high,&bits,sizeof(high));
        const double error=(high*1000000.0-(double(micro)+0.5))+(value-high)*1000000.0;
        if(error>0||(error==0&&(micro&1)))++micro;
    }
    snprintf(out,sizeof(out),"%s%lu.%06lu",wire<0?"-":"",(unsigned long)(micro/1000000),(unsigned long)(micro%1000000));
}
inline int lostAndFoundText(int32_t latitude,int32_t longitude,char (&out)[85]){
    char lat[18],lon[18];coordinate6(latitude,lat);coordinate6(longitude,lon);
    return snprintf(out,sizeof(out),"🚨I'm lost! Lat / Lon: %s, %s\a",lat,lon);
}
}
