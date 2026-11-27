// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d GamesModule states, per-game key bindings, score ranking and tick intervals.
#pragma once
#include "games/Snake.h"
#include "games/Tetris.h"
#include "games/ChirpyRunner.h"
#include "games/Breakout.h"
#include "OnScreenKeyboardModule.h"
#include <stdio.h>

namespace mt::stock::ui {
class GamesModule {
public:
    enum class State { Attract, Playing, Paused, GameOver, Scores, Initials, ConfirmClear };
    struct Score { uint32_t value, node, epoch; char initials[4]; };
    struct Table { Score entries[5]{}; };
    static constexpr const char *Names[]={"Snake","Tetris","Chirpy Dash","Breakout"};
    // Only the running game owns live board state. Each donor core is reset on
    // launch. Class-local placement new works with Zephyr's minimal C++ runtime.
    template<class Core> struct Slot:Core {
        static void *operator new(size_t,void *storage) noexcept{return storage;}
        static void operator delete(void *,void *) noexcept{}
    };
    union {
        Slot<SnakeGame> snake;
        Slot<TetrisGame> tetris;
        Slot<ChirpyRunnerGame> chirpy;
        Slot<BreakoutGame> breakout;
    };
    GamesModule():snake{}{}
    Table scores[4]{};
    bool dirty=false;
    unsigned selected=0;
    State state=State::Attract;
    uint32_t next=0,lastActivity=0,lastScore=0;
    uint32_t node=0,epoch=0;
#if defined(CONFIG_MT_GAMES_ANNOUNCE_HIGH_SCORE)
    char announcement[68]{};
#endif
    char ownerShortName[5]{};
    OnScreenKeyboardModule initials;
    bool clearYes=false;
    bool onScreen=false;

    void visible(bool showing,uint32_t now){
        if(showing!=onScreen){lastActivity=now;onScreen=showing;if(!showing){state=State::Attract;initials.clear();}}
    }
    void start(uint32_t seed,uint32_t now){
        switch(selected){
        case 0:new(&snake) Slot<SnakeGame>;snake.reset(seed);break;
        case 1:new(&tetris) Slot<TetrisGame>;tetris.reset(seed);break;
        case 2:new(&chirpy) Slot<ChirpyRunnerGame>;chirpy.reset(seed);break;
        case 3:new(&breakout) Slot<BreakoutGame>;breakout.reset(seed);break;
        }
        state=State::Playing;lastActivity=now;next=now+250;
    }
    uint32_t score()const{
        switch(selected){case 0:return snake.score();case 1:return tetris.score();case 2:return chirpy.score();default:return breakout.score();}
    }
    unsigned interval()const{
        switch(selected){
        case 0:{int n=160-int(snake.length())*3;return n<70?70:n;}
        case 1:{int n=600-int(tetris.level())*45;return n<50?50:n;}
        case 2:return 33;
        default:{int n=45-int(breakout.level()-1)*5;return (n<20?20:n)/2;}
        }
    }
    void finish(uint32_t now){
        lastScore=score();state=State::GameOver;
        if(lastScore>scores[selected].entries[4].value){
            initials.start(now,"AAA");state=State::Initials;
        }
    }
    void recordScore(const char *name){
        auto &table=scores[selected].entries;
        unsigned pos=0;
        while(pos<5&&lastScore<=table[pos].value)++pos;
        if(pos<5&&lastScore){
            for(unsigned i=4;i>pos;--i)table[i]=table[i-1];
            table[pos]={lastScore,node,epoch,{}};
#if defined(CONFIG_MT_GAMES_ANNOUNCE_HIGH_SCORE)
            if(!pos&&name[0])snprintf(announcement,sizeof(announcement),"New %s high score %lu by %.3s!",
                Names[selected],static_cast<unsigned long>(lastScore),name);
#endif
            if(!name[0])name=ownerShortName;
            const auto len=strnlen(name,3);memcpy(table[pos].initials,name,len);
            dirty=true;
        }
        initials.clear();state=State::GameOver;
    }
    bool poll(uint32_t now){
        if(!onScreen)return false;
        if(state==State::Initials){
            lastActivity=now;if(initials.expired(now))recordScore("");
            return false;
        }
        if(state==State::ConfirmClear){
            // The stock overlay owns its 30-second deadline and suppresses the games inactivity timer.
            lastActivity=now;
            if(int32_t(now-next)>=0)state=State::Scores;
            return false;
        }
        if(uint32_t(now-lastActivity)>=15000)return true;
        if(state!=State::Playing)return false;
        if(int32_t(now-next)<0)return false;
        bool alive=false;
        switch(selected){case 0:alive=snake.step();break;case 1:alive=tetris.step();break;
        case 2:alive=chirpy.step();break;case 3:alive=breakout.step();break;}
        if(!alive)finish(now);else next=now+interval();
        return false;
    }
    bool input(unsigned ev,uint32_t now,uint8_t key=0){
        lastActivity=now;bool back=ev==24||ev==27;
        switch(state){
        case State::Attract:
            if(ev==17||ev==18){selected=(selected+(ev==18?1:3))%4;return true;}
            if(ev==10){start(now^(node*2654435761u),now);return true;}
            if(ev==11){state=State::Scores;return true;}
            return false;
        case State::Playing:
            if((ev==10&&(key==0xc9||key==0xdb))||(back&&!(selected==3&&breakout.isBallDocked()))){state=State::Paused;return true;}
            switch(selected){
            case 0:
                if(key>=0xc0&&key<=0xdf&&(ev==19||ev==20))snake.turn(ev==20);
                else if(ev>=17&&ev<=20){
                    static constexpr SnakeGame::Direction dirs[]={SnakeGame::DIR_UP,SnakeGame::DIR_DOWN,SnakeGame::DIR_LEFT,SnakeGame::DIR_RIGHT};
                    snake.setDirection(dirs[ev-17]);
                }
                break;
            case 1:
                if(ev==17)tetris.rotate();else if(ev==18)tetris.softDrop();
                else if(ev==19)tetris.moveLeft();else if(ev==20)tetris.moveRight();
                else if(ev==10||ev==11)tetris.hardDrop();
                if(!tetris.isPlaying())finish(now);
                break;
            case 2:if(ev==10||ev==11||ev==17)chirpy.jump();break;
            case 3:
                if(ev==19)breakout.moveLeft();else if(ev==20)breakout.moveRight();
                else if(back||ev==10||ev==11)breakout.launchBall();
                break;
            }
            return true;
        case State::Paused:
            if(back)state=State::Attract;
            else if(ev==10||(ev>=17&&ev<=20)){state=State::Playing;next=now+250;}
            return true;
        case State::GameOver:
            if(back)state=State::Attract;else if(ev==10)state=State::Scores;
            return true;
        case State::Scores:
            if(ev==11){state=State::ConfirmClear;clearYes=false;next=now+30000;if(!next)next=1;}
            else if(back||ev==10)state=State::Attract;
            return true;
        case State::ConfirmClear:
            if((ev>=17&&ev<=20)||ev==12||ev==13||ev==28||ev==29)clearYes=!clearYes;
            if(ev==10||ev==24||ev==30){
                if(ev==10&&clearYes){scores[selected]={};dirty=true;}
                state=State::Scores;
            }
            return true;
        case State::Initials:{
            const auto action=initials.input(ev,now);
            if(initials.length>3){initials.length=3;initials.text[3]=0;}
            if(action==OnScreenKeyboardModule::Result::Submit)recordScore(initials.text);
            else if(action==OnScreenKeyboardModule::Result::Cancel)recordScore("");
            return true;
        }}
        return false;
    }
};
}
