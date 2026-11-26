// SPDX-License-Identifier: GPL-3.0-only
// See Meshtastic 8e6a88d src/gps/GPS.cpp GPS::probe, GPS::setup.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::gnss::l76k {
enum class Result { Configured, Passive, Cancelled, Error };

class Reply {
    char text_[32]{};
    size_t size_=0;
    bool collecting_=false;
public:
    bool starts(const char *prefix) const {
        const size_t length=strlen(prefix);
        return length<=sizeof(text_) && size_>=length && !memcmp(text_,prefix,length);
    }
    bool byte(char c,bool hardware) {
        if(c=='$'){size_=0;collecting_=true;}
        if(!collecting_)return false;
        if(c=='\r'){
            collecting_=false;
            return hardware ? starts("$GPTXT,01,01,02,HW=ATGM336H") || starts("$GPTXT,01,01,02,HW=ATGM332D")
                            : starts("$GPTXT,01,01,02,SW=") && size_>sizeof("$GPTXT,01,01,02,SW=")-1;
        }
        if(c<' '||c>'~'||size_>=766){collecting_=false;return false;}
        if(size_<sizeof(text_))text_[size_]=c;
        ++size_;
        return false;
    }
};

// The GNSS worker has exclusive use of the suspended NMEA driver's UART during
// setup. Change only the host UART baud. No receiver baud or NVM command.
template<class Io>
Result setup(Io &io)
{
    constexpr uint32_t rates[]={9600,115200,38400,9600,115200,38400,4800,57600,9600};
    constexpr const char *commands[]={
        "$PCAS04,7*1E\r\n",
        "$PCAS03,1,0,0,0,1,0,0,0,0,0,,,0,0*02\r\n",
        "$PCAS11,3*1E\r\n"
    };
    enum class Probe { NoReply, Found, Cancelled, Error };
    auto query=[&](const char *command,bool hardware) {
        // Drop bytes predating this query, even at a reused baud. A saturated
        // FIFO is not a reply. Three hundred bytes exceed the ESP UART FIFO.
        char c;
        unsigned stale=0;
        while(stale<300 && io.read(c))++stale;
        if(stale==300)return Probe::NoReply;
        if(!io.active())return Probe::Cancelled;
        if(!io.send(command))return io.active()?Probe::Error:Probe::Cancelled;
        Reply reply;
        const uint32_t begin=io.now();
        while(uint32_t(io.now()-begin)<500 && io.active()){
            for(unsigned n=0;n<256 && io.read(c);++n){
                if(reply.byte(c,hardware))return Probe::Found;
            }
            if(!io.wait(5))break;
        }
        return io.active()?Probe::NoReply:Probe::Cancelled;
    };
    for(uint32_t rate:rates){
        if(!io.active())return Result::Cancelled;
        if(!io.speed(rate))return Result::Error;
        if(!io.wait(100))return Result::Cancelled;
        // Stock looks for ATGM336H/332D before the broad software-version
        // response shared by CAS devices. Leave this family in passive mode.
        const auto hardware=query("$PCAS06,1*1A\r\n",true);
        if(hardware==Probe::Found)break;
        if(hardware==Probe::Error)return Result::Error;
        if(hardware==Probe::Cancelled)return Result::Cancelled;
        const auto software=query("$PCAS06,0*1B\r\n",false);
        if(software==Probe::Error)return Result::Error;
        if(software==Probe::Cancelled)return Result::Cancelled;
        if(software!=Probe::Found)continue;
        for(const char *command:commands){
            if(!io.active())return Result::Cancelled;
            if(!io.send(command))return io.active()?Result::Error:Result::Cancelled;
            if(!io.wait(250))return Result::Cancelled;
        }
        return Result::Configured;
    }
    if(!io.active())return Result::Cancelled;
    return io.speed(9600)?Result::Passive:Result::Error;
}
}
