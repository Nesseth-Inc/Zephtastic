// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d BuzzerFeedbackThread.cpp and buzz.cpp. No blocking sleeps.
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace mt::stock::ui {
struct BuzzerFeedback {
    struct Note {uint16_t hz,ms;};
    static constexpr Note beep[]={{247,62}},click[]={{233,1}},chirp[]={{233,20}},boop[]={{220,50}},
        combo[]={{196,80},{247,60},{277,80},{196,60},{277,60},{247,120}};
    const Note *notes=nullptr;unsigned index=0,count=0;uint32_t since=0;bool sounding=false;
    template<class Output> void stop(Output output){if(sounding)output(0);sounding=false;notes=nullptr;}
    template<class Output> void input(unsigned event,unsigned mode,bool encoder,uint32_t now,Output output){
        if(mode!=0&&mode!=3)return; // ALL_ENABLED and SYSTEM_ONLY
        const Note *sequence=nullptr;unsigned size=1;
        switch(event){
        case 10:case 11:sequence=encoder?click:beep;break;
        case 28:case 29:if(!encoder)sequence=beep;break;
        case 12:case 13:case 17:case 18:case 19:case 20:sequence=chirp;break;
        case 24:case 27:sequence=boop;break;
        case 0xaf:sequence=combo;size=sizeof(combo)/sizeof(combo[0]);break;
        }
        if(!sequence)return;
        stop(output);notes=sequence;count=size;index=0;since=now;sounding=output(notes[0].hz);
        if(!sounding)notes=nullptr;
    }
    template<class Output> void poll(uint32_t now,Output output){
        if(!notes)return;
        const uint32_t elapsed=now-since;
        if(sounding&&elapsed>=notes[index].ms){output(0);sounding=false;}
        if(elapsed>=notes[index].ms*13u/10u){
            if(++index>=count){notes=nullptr;return;}
            since=now;sounding=output(notes[index].hz);if(!sounding)notes=nullptr;
        }
    }
};
}
