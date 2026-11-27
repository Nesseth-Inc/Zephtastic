#include "stock/ui/OnScreenKeyboardModule.h"
#include <cassert>
#include <iostream>
int main(){
    using Keyboard=mt::stock::ui::OnScreenKeyboardModule;
    for(unsigned code=0;code<256;++code){
        graphics::OnScreenKeyboardModule donor;
        donor.keyboard=std::make_unique<graphics::VirtualKeyboard>();
        donor.keyboard->setInputText("draft");
        Keyboard port;port.start(now,"draft");
        ++now;donor.handleInput({code});
        auto result=port.input(code,now);
        assert(donor.cancelled==(result==Keyboard::Result::Cancel));
        if(donor.cancelled){assert(!port.active&&!port.length);continue;}
        assert(port.active&&donor.keyboard->inputText==port.text);
        assert(port.cursor==unsigned(donor.keyboard->cursorRow)*11+donor.keyboard->cursorCol);
        assert(port.lastActivity==donor.keyboard->lastActivityTime);
    }
    std::cout<<"PASS 256 stock keyboard module input events\n";
    for(unsigned cell=0;cell<44;++cell)for(unsigned action:{10u,11u}){
        graphics::VirtualKeyboard stock;Keyboard port;
        std::string submitted;bool callback=false;
        stock.setInputText("abcdef");stock.setCallback([&](const std::string &s){submitted=s;callback=true;});
        port.start(now,"abcdef");
        assert(port.cursor==unsigned(stock.cursorRow)*11+stock.cursorCol);
        for(unsigned step=0;step<cell;++step){stock.moveCursorRight();port.input(20,++now);}
        assert(port.cursor==unsigned(stock.cursorRow)*11+stock.cursorCol);
        if(action==10)stock.handlePress();else stock.handleLongPress();
        auto result=port.input(action,now);
        assert(stock.inputText==port.text);
        assert(callback==(result==Keyboard::Result::Submit||result==Keyboard::Result::Cancel));
        if(callback)assert(submitted==port.text);
    }
    graphics::VirtualKeyboard stock;Keyboard port;port.start(now);
    for(unsigned step=0;step<5000;++step){
        unsigned ev=17+(step*101u^(step>>3))%4;
        switch(ev){case 17:stock.moveCursorUp();break;case 18:stock.moveCursorDown();break;
        case 19:stock.moveCursorLeft();break;case 20:stock.moveCursorRight();break;}
        port.input(ev,now);assert(port.cursor==unsigned(stock.cursorRow)*11+stock.cursorCol);
        ++now;
    }
    now+=60000;assert(stock.isTimedOut()&&port.expired(now));
    std::cout<<"PASS pinned VirtualKeyboard: 88 stock key actions, 5000 cursor moves and timeout\n";
}
