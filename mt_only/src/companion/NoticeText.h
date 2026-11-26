// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if (defined(__ZEPHYR__) && defined(CONFIG_MT_PKI_FRESH_DOMAIN_ONLY)) || defined(MT_TEST_COMPACT_NOTICES)
#include "mt_notice_table.h"
namespace mt::companion::notice {
constexpr uint32_t id(const char *s){
    uint32_t result=2166136261u;
    while(*s)result=(result^uint8_t(*s++))*16777619u;
    return result;
}
inline void expand(uint8_t token,char *out,size_t limit,size_t &used){
    if(used==limit)return;
    if(token<128){out[used++]=char(token);return;}
    const auto &pair=pairs[token-128];
    expand(pair[0],out,limit,used);
    expand(pair[1],out,limit,used);
}
}
#define MT_NOTICE(literal) (::mt::companion::notice::Text<::mt::companion::notice::id(literal)>::bytes)
#else
#define MT_NOTICE(literal) literal
#endif

namespace mt::companion::notice {
inline void copy(char *out,size_t capacity,const char *message){
    if(!capacity)return;
    size_t used=0;
#if (defined(__ZEPHYR__) && defined(CONFIG_MT_PKI_FRESH_DOMAIN_ONLY)) || defined(MT_TEST_COMPACT_NOTICES)
    if(*message==1){
        ++message;
        while(*message && used<capacity-1)expand(uint8_t(*message++),out,capacity-1,used);
    }else
#endif
    {
        while(*message && used<capacity-1)out[used++]=*message++;
    }
    out[used]=0;
}
}
