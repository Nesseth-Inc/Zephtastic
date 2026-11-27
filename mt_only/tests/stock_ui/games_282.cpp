// SPDX-License-Identifier: GPL-3.0-only
#include "stock/ui/GamesModule.h"
#define TetrisGame StockTetrisGame
#include "stock-games/Tetris.h"
#undef TetrisGame
#include <cassert>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// Actual stock methods are compiled in this namespace. The fixture captures
// redraws, the thread deadline and picker callbacks instead of driving a screen.
namespace donor {
#include "game-types.inc"
struct InputEvent {input_broker_event inputEvent; unsigned char kbchar;};
static uint32_t now, randomWord, epoch;
static uint32_t millis(){return now;}
static uint32_t random(){return randomWord;}
static constexpr int RTCQualityDevice=0;
static uint32_t getValidTime(int,bool){return epoch;}
struct {char short_name[5]="ZT";} owner;
struct {uint32_t id=42;uint32_t getNodeNum(){return id;}} database,*nodeDB=&database;
struct ScoreTable : HighScoreTableBase {
    struct Entry {uint32_t score,nodeNum,epoch;char shortName[5];};
    Entry entries_[HS_COUNT]{};
    unsigned saves=0;
    void save() override{++saves;}
    void load() override{}
    bool loaded() const override{return true;}
#include "score-methods.inc"
};
struct GamesModule;
struct Game {
    ScoreTable table;
    virtual ~Game()=default;
    virtual void start(uint32_t){}
    virtual bool tick(){return true;}
    virtual bool isPlaying()const{return true;}
    virtual uint32_t score()const{return 0;}
    virtual int32_t tickIntervalMs()const{return 100;}
    virtual void handleInput(const InputEvent*){}
    bool wantsBackButton()const{return false;}
    bool wantsPeriodicMesh()const{return false;}
    int32_t meshTick(GamesModule&){return -1;}
    ScoreTable& scores(){return table;}
};
struct Tetris:Game {
    StockTetrisGame game;
#include "tetris-adapter.inc"
    int32_t tickIntervalMs() const override;
    void handleInput(const InputEvent*) override;
};
#include "tetris-input.inc"
namespace graphics {
struct BannerOverlayOptions {
    const char *message=nullptr;const char **optionsArrayPtr=nullptr;int optionsCount=0;
    std::function<void(int)> bannerCallback;
};
}
struct Screen {
    bool shown=true;
    uint32_t deadline=0;
    int curSelected=0;
    void resetBanner(){confirmation={};curSelected=0;}
    void overlayInput(input_broker_event code){
        InputEvent inEvent{code,0};
        const int *optionsEnumPtr=nullptr;
        const int alertBannerOptions=2;
        const uint32_t alertBannerUntil=deadline;
        auto alertBannerCallback=confirmation;
#include "game-banner-input.inc"
    }
    std::function<void(int)> confirmation;
    std::function<void(const std::string&)> initials;
    bool isGamesFrameShown()const{return shown;}
    bool isOverlayBannerShowing()const{return bool(confirmation)||bool(initials);}
    void showHomeFrame(){shown=false;}
    void showOverlayBanner(const graphics::BannerOverlayOptions &options){
        assert(options.optionsCount==2 && std::string(options.optionsArrayPtr[0])=="No" &&
               std::string(options.optionsArrayPtr[1])=="Yes");
        confirmation=options.bannerCallback;
        curSelected=0;deadline=now+confirmDuration;if(!deadline)deadline=1;
    }
    void showAlphanumericPicker(const char*,const char *initial,uint32_t duration,unsigned length,
                                std::function<void(const std::string&)> callback){
        assert(std::string(initial)=="AAA" && duration==60000 && length==3);
        initials=callback;
    }
} display,*screen=&display;
constexpr int EVENT_PRESS=1;
struct {void trigger(int value){assert(value==EVENT_PRESS);}} powerFSM;
#define LOG_INFO(...) ((void)0)
struct GamesModule {
    std::vector<Game*> games;
    GamesUiState uiState=GAMES_IDLE;
    Game *active=nullptr;
    uint8_t selected=0;
    uint32_t lastScore=0,lastActivityMs=0,lastAwakeKickMs=0;
    int lastRank=-1;bool lastWasNewTop=false,enabled=true;
    uint32_t next=0;
    void noteActivity(){lastActivityMs=millis();}
    void requestRedraw(){}
    void setIntervalFromNow(unsigned delay){next=now+delay;}
    int32_t disable(){enabled=false;return -1;}
    void launchGame();void startPlaying();void goHome();void enterGameOver();
    void promptForInitials();void recordHighScore(const char*);void exitToIdle();void kickTick();
    int handleInputEvent(const InputEvent*);int32_t runOnce();
};
static GamesModule *gamesModule;
#include "games-methods.inc"
}

using ZT = mt::stock::ui::GamesModule;
using State = ZT::State;
struct Comparison {
    ZT zt; donor::GamesModule stock;
    donor::Game other[3]; donor::Tetris tetris;
    unsigned cases=0;
    explicit Comparison(uint32_t start){
        stock.games={&other[0],&tetris,&other[1],&other[2]};
        donor::gamesModule=&stock;donor::display={};
        zt.node=donor::database.id;zt.epoch=donor::epoch=999;
        strcpy(zt.ownerShortName,donor::owner.short_name);
        donor::now=start;donor::randomWord=zt.node*2654435761u;
        zt.visible(true,start);stock.noteActivity();
    }
    void compare(){
        State expected;
        switch(stock.uiState){
        case donor::GAMES_IDLE:expected=State::Attract;break;
        case donor::GAMES_PLAYING:expected=State::Playing;break;
        case donor::GAMES_PAUSED:expected=State::Paused;break;
        case donor::GAMES_GAMEOVER:expected=donor::display.initials?State::Initials:State::GameOver;break;
        case donor::GAMES_HISCORES:expected=donor::display.confirmation?State::ConfirmClear:State::Scores;break;
        default:assert(false);return;
        }
        assert(zt.state==expected && zt.selected==stock.selected);
        assert(zt.lastScore==stock.lastScore);
        if(stock.uiState==donor::GAMES_PLAYING && stock.active==&tetris){
            const auto &a=zt.tetris;const auto &b=tetris.game;
            assert(a.score()==b.score() && a.isPlaying()==b.isPlaying() && zt.interval()==unsigned(tetris.tickIntervalMs()));
            assert(a.current().row==b.current().row && a.current().col==b.current().col &&
                   a.current().rot==b.current().rot && a.current().type==b.current().type);
            assert(!memcmp(a.board,b.board,sizeof(a.board)) && zt.next==stock.next);
        }
        for(unsigned slot=0;slot<5;++slot){
            const auto &entry=zt.scores[1].entries[slot];const auto &original=tetris.table.entries_[slot];
            assert(entry.value==original.score && entry.node==original.nodeNum && entry.epoch==original.epoch);
            assert(!strcmp(entry.initials,original.shortName));
        }
        ++cases;
    }
    void key(donor::input_broker_event ev,uint8_t button=0){
        ++donor::now;
        const donor::InputEvent event{ev,button};
        bool handled=stock.handleInputEvent(&event);
        if(!handled && ev==donor::INPUT_BROKER_SELECT && stock.uiState==donor::GAMES_IDLE){
            // Screen owns SELECT on the current games frame and hands off launchGame().
            stock.launchGame();handled=true;
        }
        assert(handled==zt.input(ev,donor::now,button));compare();
    }
    void tick(uint32_t at){
        donor::now=at;
        const bool expired=zt.poll(at);
        const int32_t delay=stock.runOnce();
        if(delay>=0)stock.next=at+uint32_t(delay);
        assert(expired==!donor::display.shown);
        if(expired)zt.visible(false,at);
        compare();
    }
    void overlayKey(donor::input_broker_event ev){
        assert(donor::display.confirmation);
        donor::display.overlayInput(ev);
        zt.input(ev,++donor::now);
        if(donor::display.confirmation) assert(zt.clearYes==(donor::display.curSelected==1));
        compare();
    }
};

static unsigned exercise(uint32_t start){
    using namespace donor;
    Comparison f(start);
    for(unsigned i=0;i<5;++i)f.key(INPUT_BROKER_UP);
    for(unsigned i=0;i<6;++i)f.key(INPUT_BROKER_DOWN);
    assert(f.zt.selected==1);f.key(INPUT_BROKER_SELECT_LONG);
    f.key(INPUT_BROKER_BACK);
    f.key(INPUT_BROKER_SELECT);assert(f.zt.state==State::Playing);
    for(auto ev:{INPUT_BROKER_UP, INPUT_BROKER_LEFT, INPUT_BROKER_RIGHT, INPUT_BROKER_DOWN,
                 INPUT_BROKER_SELECT, INPUT_BROKER_SELECT_LONG, INPUT_BROKER_ALT_PRESS})f.key(ev);
    // Both supported Start codes pause and resume. An ordinary SELECT drops.
    for(uint8_t code:{0xc9,0xdb}){f.key(INPUT_BROKER_SELECT,code);assert(f.zt.state==State::Paused);
        f.key(INPUT_BROKER_SELECT,code);assert(f.zt.state==State::Playing);}
    f.tick(f.zt.next);
    f.key(INPUT_BROKER_CANCEL);f.key(INPUT_BROKER_UP);
    f.key(INPUT_BROKER_BACK);f.key(INPUT_BROKER_BACK);
    f.key(INPUT_BROKER_SELECT);
    for(unsigned i=0;f.stock.uiState==GAMES_PLAYING;++i){assert(i<64);f.key(INPUT_BROKER_SELECT);}
    assert(f.zt.state==State::Initials);
    f.tick(now+16000);assert(f.zt.state==State::Initials);
    display.initials("ACE");display.initials={};f.zt.recordScore("ACE");f.compare();
    assert(f.zt.scores[1].entries[0].value);
    f.key(INPUT_BROKER_SELECT);f.key(INPUT_BROKER_SELECT_LONG);
    f.tick(now+16000);assert(f.zt.state==State::ConfirmClear);
    for(auto ev:{INPUT_BROKER_BACK,INPUT_BROKER_UP_LONG,INPUT_BROKER_DOWN_LONG,INPUT_BROKER_ALT_PRESS,
                 INPUT_BROKER_USER_PRESS,INPUT_BROKER_UP,INPUT_BROKER_LEFT,INPUT_BROKER_DOWN,INPUT_BROKER_RIGHT})f.overlayKey(ev);
    f.overlayKey(INPUT_BROKER_SELECT);assert(f.zt.scores[1].entries[0].value);
    for(auto cancel:{INPUT_BROKER_ALT_LONG,INPUT_BROKER_CANCEL}){
        f.key(INPUT_BROKER_SELECT_LONG);f.overlayKey(INPUT_BROKER_RIGHT);f.overlayKey(cancel);
        assert(f.zt.scores[1].entries[0].value);
    }
    f.key(INPUT_BROKER_SELECT_LONG);f.overlayKey(INPUT_BROKER_DOWN);f.overlayKey(INPUT_BROKER_SELECT);
    assert(!f.zt.scores[1].entries[0].value);
    f.key(INPUT_BROKER_SELECT_LONG);
    // Capture the stock banner owner's expiration, then allow the games thread to poll.
    f.tick(display.deadline-1);assert(f.zt.state==State::ConfirmClear);
    const uint32_t expires=display.deadline;display.confirmation={};
    f.tick(expires);assert(f.zt.state==State::Scores);
    f.key(INPUT_BROKER_SELECT);f.tick(now+14999);f.tick(now+1);
    assert(!display.shown&&!f.zt.onScreen);
    return f.cases;
}
int main(){
    const unsigned states=exercise(100)+exercise(UINT32_MAX-500);
    std::printf("GAMES282 transitions=%u\n",states);
}
