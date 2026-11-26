// SPDX-License-Identifier: GPL-3.0-only
// Stock CannedMessageModule's emote selector and graphics/emotes.cpp at 8e6a88d.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
namespace mt::stock::ui::emotes {
#include "EmoteTable.inc"
#if defined(MT_PACKED_EMOTES)
#include "mt_emote_table.h"
// The generator checks the maximum pair depth so a local decoder needs only
// eight pending right-hand tokens and the caller's 32-byte glyph buffer.
inline void bitmap(unsigned glyph,uint8_t (&out)[32]){
    const uint8_t *in=glyphData+glyphOffsets[glyph];
    uint8_t pending[8];unsigned depth=0;
    for(unsigned i=0;i<32;++i){
        unsigned token=depth?pending[--depth]:*in++;
        while(token>sizeof(glyphAlphabet)){
            const uint8_t *pair=glyphPairs+2*(token-sizeof(glyphAlphabet)-1);
            pending[depth++]=pair[1];token=pair[0];
        }
        const uint8_t diff=token==sizeof(glyphAlphabet)?*in++:glyphAlphabet[token];
        out[i]=diff^(i>=2?out[i-2]:0);
    }
}
#else
inline void bitmap(unsigned glyph,uint8_t (&out)[32]){memcpy(out,bitmaps[glyph],sizeof(out));}
#endif
inline constexpr size_t Count=sizeof(entries)/sizeof(entries[0]);
inline const char *label(unsigned index){
    const char *p=labels;
    for(unsigned i=0;i<index;++i)p+=entries[i].size;
    return p;
}
inline int match(const char *text,size_t available){
    int best=-1;size_t bestLength=0;
    const char *p=labels;
    for(unsigned i=0;i<Count;++i){
        const auto &e=entries[i];
        if(e.size>bestLength&&e.size<=available&&!memcmp(text,p,e.size)){
            best=int(i);bestLength=e.size;
        }
        p+=e.size;
    }
    return best;
}
}
