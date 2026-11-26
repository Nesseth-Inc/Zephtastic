// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <string.h>

namespace mt::stock::optional {
// Print a binary32 at one or two decimal places. Rounding ties to even matches
// printf's default rounding mode without linking a second floating-point formatter.
inline void fixedDecimal(float value,unsigned places,char (&out)[48]){
    uint32_t bits;
    static_assert(sizeof(value)==sizeof(bits));
    memcpy(&bits,&value,sizeof(bits));
    char *dst=out;
    if(bits>>31)*dst++='-';
    const unsigned exponent=(bits>>23)&255u;
    uint32_t mantissa=bits&0x7fffffu;
    if(exponent==255){strcpy(dst,mantissa?"nan":"inf");return;}
    if(exponent)mantissa+=1u<<23;
    uint32_t scaled=mantissa*(places==1?10u:100u);
    int shift=exponent ? int(exponent)-150 : -149;
    if(shift<0){
        const unsigned right=unsigned(-shift);
        if(right>31)scaled=0;
        else {
            const uint32_t half=1u<<(right-1);
            const uint32_t discarded=scaled&((half-1)|half);
            scaled>>=right;
            if(discarded>half || (discarded==half && (scaled&1u)))++scaled;
        }
    }
    // The largest finite float uses 39 integral digits and two fractional digits.
    uint8_t digits[42]={};unsigned count=0;
    do{digits[count++]=scaled%10;scaled/=10;}while(scaled);
    for(;shift>0;--shift){
        unsigned carry=0;
        for(unsigned i=0;i<count;++i){
            const unsigned n=digits[i]*2u+carry;
            digits[i]=n%10;carry=n/10;
        }
        if(carry)digits[count++]=carry;
    }
    while(count<=places)digits[count++]=0;
    while(count){
        if(count==places)*dst++='.';
        *dst++=char('0'+digits[--count]);
    }
    *dst=0;
}
}
