#include "stock/ui/HeadlessButton.h"
#include "stock/ui/BuzzerFeedback.h"
int main(){
    using namespace mt::ui;
    using Commands=mt::stock::ui::SystemCommandsModule;
    using Action=Commands::Action;
    using Games=mt::stock::ui::GamesModule;
    using Keyboard=mt::stock::ui::OnScreenKeyboardModule;
    static_assert(Commands::decode({17,0})==Action::None);
    static_assert(Commands::decode({17,0x11})==Action::Brighten);
    static_assert(Commands::decode({0x9a,0})==Action::FactoryReset);
    Commands resetMenu;resetMenu.begin(100);resetMenu.move(false,101);
    assert(Commands::decode(Commands::Menu[resetMenu.row].event)==Action::Shutdown);
    resetMenu.begin(110,true);resetMenu.move(false,111);unsigned resets=0;
    auto reset=[&](Action a,uint32_t){assert(a==Action::FactoryReset);++resets;return 0;};
    resetMenu.select(112,reset);assert(resetMenu.confirm&&!resets);
    resetMenu.select(113,reset);assert(!resetMenu.confirm&&resets==1);
    static_assert(Commands::decode({0xaf,0xf2})==Action::None);
    mt::stock::ui::HeadlessButton headless;
    unsigned activityCount=0;
    auto sample=[&](bool down,uint32_t t){return headless.update(down,t,[&]{++activityCount;});};
    sample(false,0);sample(true,10);sample(true,45);sample(false,4050);assert(sample(false,4085)==Action::None);
    auto clicks=[&](uint32_t start,unsigned count){
        for(unsigned i=0;i<count;++i){uint32_t t=start+i*135;sample(true,t);sample(true,t+31);
            sample(false,t+60);sample(false,t+95);}
        return sample(false,start+(count-1)*135+350);
    };
    assert(clicks(30001,2)==Action::Ping&&clicks(32000,3)==Action::Gps);
    sample(true,35000);sample(true,35032);sample(false,39100);assert(sample(false,39132)==Action::Shutdown);
    assert(activityCount==7);
    mt::stock::ui::BuzzerFeedback feedback;
    unsigned hz=0;auto out=[&](unsigned value){hz=value;return true;};
    feedback.input(0xaf,2,false,0,out);assert(!hz);
    feedback.input(0xaf,0,false,0,out);assert(hz==196);feedback.poll(80,out);assert(!hz);
    feedback.poll(104,out);assert(hz==247);feedback.input(27,3,false,110,out);assert(hz==220);
    feedback.poll(181,out);assert(!hz&&!feedback.notes);feedback.input(11,0,true,182,out);assert(hz==233);
    static int calls=0;
    setSystemCommandOwner([](Action a,uint32_t){++calls;return a==Action::Ping?0:-ENOTSUP;},true);
    mt::Service s;s.config.seconds=UINT32_MAX;
    auto tick=[&](uint32_t now){clockNow=now;poll(s,true,true,true,true,now,true,{});};
    auto key=[&](unsigned code){assert(input(code,++clockNow)==0);tick(clockNow);};
    auto press=[&](bool hold){uint32_t t=clockNow+1;keyDown=1;tick(t);tick(t+35);keyDown=0;
        unsigned duration=hold?810:120;tick(t+duration);tick(t+duration+35);tick(t+duration+420);};
    init(s.config);tick(2000);
    model().page=Page::Status;key(10);assert(!model().commands.open);
    press(true);assert(model().commands.open&&model().commands.row==0&&calls==0);
    assert(input(0xaf,clockNow)==-EBUSY&&input(10,clockNow)==-EBUSY);
    press(false);assert(model().commands.row==1);press(true);assert(model().commands.confirm&&calls==0);
    press(true);assert(model().commands.result==0&&calls==1);
    pairing(true,123456,clockNow);assert(!model().commands.open&&input(10,clockNow)==-EBUSY);
    pairing(false,0,++clockNow);tick(clockNow);
    setCannedContent("",true,true);setCannedRoutes(5,12);
    model().page=Page::Canned;model().selected=model().canned.count()+1;
    key(10);assert(model().keyboard.active&&model().keyboard.cursor==27);
    while(model().keyboard.cursor)key(20);key(10);key(18);key(11);
    assert(!strcmp(model().keyboard.text,"1Q"));
    key(27);assert(model().keyboard.active&&!strcmp(model().keyboard.text,"1Q"));
    for(int i=0;i<10;++i)key(20);
    key(10);assert(!model().keyboard.active&&model().cannedSubstate==CannedSubstate::Destinations);
    key(10);key(18);key(10);assert(model().cannedConfirm);key(10);
    PendingCannedSend req{};assert(takePendingCannedSend(req)&&!takePendingCannedSend(req));
    assert(req.index==255&&req.channel==2&&req.destination==UINT32_MAX&&!strcmp(req.draft,"1Q"));
    assert(!model().pendingCanned.draft[0]);
    model().selected=model().canned.count()+1;key(10);key(10);assert(model().keyboard.active);
    setCannedRoutes(1,13);assert(!model().keyboard.active&&!model().keyboard.text[0]);
    key(10);assert(model().keyboard.active);tick(clockNow+60001);assert(!model().keyboard.active);
    Keyboard keyboard;keyboard.start(UINT32_MAX-200,"abcdef");keyboard.cursor=0;keyboard.input(19,50);assert(keyboard.cursor==43);
    keyboard.input(17,51);keyboard.input(17,52);keyboard.input(17,53);assert(keyboard.cursor==10);
    keyboard.input(11,54);assert(!strcmp(keyboard.text,"a"));keyboard.input(24,55);assert(!keyboard.active);
    keyboard.start(clockNow);
    assert(keyboard.openPicker(clockNow));keyboard.input(10,clockNow);
    assert(keyboard.length==4&&!strcmp(keyboard.text,"👍")&&!keyboard.picker);
    keyboard.openPicker(clockNow);keyboard.input(18,clockNow);keyboard.input(10,clockNow);
    assert(!strcmp(keyboard.text,"👍👎"));keyboard.cursor=10;keyboard.input(10,clockNow);
    assert(!strcmp(keyboard.text,"👍"));
    model().page=Page::Canned;model().selected=model().canned.count()+1;key(10);
    const auto time=clockNow+1;keyDown=1;tick(time);tick(time+35);keyDown=0;tick(time+1605);tick(time+1640);
    assert(model().keyboard.picker);press(false);press(true);assert(!strcmp(model().keyboard.text,"👎"));
    pairing(true,123456,clockNow);assert(!model().keyboard.active&&!model().keyboard.text[0]);pairing(false,0,++clockNow);
    model().page=Page::Games;tick(++clockNow);
    auto &g=localGames();assert(g.onScreen);
    for(unsigned i=0;i<4;++i){
        assert(g.selected==i);key(10);assert(g.state==Games::State::Playing);
        const uint32_t due=g.next;tick(due-1);assert(g.next==due);tick(due);assert(g.next!=due);
        if(i==0){auto initial=g.snake.direction();g.input(20,clockNow,0xc0);tick(g.next);assert(initial!=g.snake.direction());}
        if(i==1){key(17);key(10);assert(g.tetris.score()>0);}
        if(i==2){key(10);assert(g.chirpy.isPlaying());}
        if(i==3){assert(g.breakout.isBallDocked());key(27);assert(!g.breakout.isBallDocked());}
        key(27);assert(g.state==Games::State::Paused);key(10);assert(g.state==Games::State::Playing);
        key(27);key(27);assert(g.state==Games::State::Attract);key(18);
    }
    g.lastScore=123;g.node=42;g.epoch=43;g.recordScore("ZT");g.lastScore=10;g.recordScore("ABC");
    assert(g.dirty&&g.scores[0].entries[0].node==42&&g.scores[0].entries[0].epoch==43&&g.scores[0].entries[1].value==10);
    key(10);assert(g.state==Games::State::Scores);key(11);key(10);assert(g.scores[0].entries[0].value==123);
    key(11);key(18);key(10);assert(!g.scores[0].entries[0].value);key(27);
    key(10);tick(clockNow+15001);assert(model().page==Page::Home);
    std::cout<<"PASS keyboard compose/routing, physical commands/pairing, four live games, timing and score menu\n";
}
