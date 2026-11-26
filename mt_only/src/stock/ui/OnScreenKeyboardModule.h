// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d VirtualKeyboard and OnScreenKeyboardModule.
// The owning display renders the grid and consumes a completed draft.
#pragma once
#if defined(CONFIG_MT_BASEUI_EMOTES)
#include "EmotePicker.h"
#endif
#include <stdint.h>
#include <string.h>

namespace mt::stock::ui {
class OnScreenKeyboardModule {
public:
    enum class Result { Unhandled, Editing, Submit, Cancel };
    static constexpr unsigned Rows=4, Columns=11, MaxText=160;
    static constexpr char Keys[Rows][Columns+1]={
        "1234567890\b","qwertyuiop\n","asdfghjkl; ","zxcvbnm.,?\x1b"
    };
    unsigned cursor=0, length=0;
    char text[MaxText+1]{};
    bool active=false;
    uint32_t lastActivity=0;
#if defined(CONFIG_MT_BASEUI_EMOTES)
    bool picker=false;uint8_t emote=0;
    bool openPicker(uint32_t now){if(!active)return false;picker=true;lastActivity=now;return true;}
#endif
    void start(uint32_t now,const char *initial="") {
#if defined(CONFIG_MT_BASEUI_EMOTES)
        picker=false;emote=0;
#endif
        cursor=2*Columns+5;length=strnlen(initial,MaxText); // stock home key H
        memmove(text,initial,length);text[length]=0;
        lastActivity=now;active=true;
    }
    void clear(){active=false;length=0;memset(text,0,sizeof(text));}
    bool expired(uint32_t now)const{return active&&uint32_t(now-lastActivity)>60000;}
    Result input(unsigned event,uint32_t now) {
        if(!active)return Result::Unhandled;
#if defined(CONFIG_MT_BASEUI_EMOTES)
        if(picker){
            if((event==17||event==19||event==29)&&emote) --emote;
            else if((event==18||event==20||event==28)&&emote+1u<emotes::Count) ++emote;
            else if(event==24||event==27)picker=false;
            else if(event==10){
                const auto &choice=emotes::entries[emote];
                if(length+choice.size<=MaxText){
                    memcpy(text+length,emotes::label(emote),choice.size);
                    length+=choice.size;text[length]=0;picker=false;
                }
            }
            lastActivity=now;return Result::Editing;
        }
#endif
        switch(event) {
        case 12:case 17:cursor=(cursor+Columns*(Rows-1))%(Rows*Columns);break;
        case 13:case 18:cursor=(cursor+Columns)%(Rows*Columns);break;
        case 19:case 29:cursor=(cursor+Rows*Columns-1)%(Rows*Columns);break;
        case 20:case 28:cursor=(cursor+1)%(Rows*Columns);break;
        case 24:case 27:clear();return Result::Cancel;
        case 10:case 11:{
            const bool longPress=event==11;
            char key=Keys[cursor/Columns][cursor%Columns];
            if(key=='\n'&&length){active=false;return Result::Submit;}
            if(key=='\x1b'){clear();return Result::Cancel;}
            if(key=='\b'){
                unsigned count=longPress?5:1;
                while(count--&&length){
                    --length;
#if defined(CONFIG_MT_BASEUI_EMOTES)
                    while(length&&(uint8_t(text[length])&0xc0u)==0x80u)--length;
#endif
                }
                text[length]=0;
            }else if(key!='\n'&&length<MaxText){
                if(longPress&&key>='a'&&key<='z')key=char(key-'a'+'A');
                else if(longPress&&key=='?')key='/';
                text[length++]=key;text[length]=0;
            }
            break;
        }
        default:return Result::Unhandled;
        }
        lastActivity=now;return Result::Editing;
    }
};
}
