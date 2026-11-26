// SPDX-License-Identifier: GPL-3.0-only
#include "gnss/L76kStandby.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <cstring>
#include <initializer_list>

namespace stock {
constexpr bool LOW=false;
constexpr unsigned OUTPUT=1, GNSS_MODEL_MTK_PA1010D=10;
unsigned serialWrites=0;
bool level=false;
void pinMode(unsigned pin,unsigned mode){assert(pin==1 && mode==OUTPUT);}
void digitalWrite(unsigned pin,bool value){assert(pin==1);level=value;}
struct UART {void write(const char*){++serialWrites;}} uart;
struct GPS {unsigned gnssModel=0;UART *_serial_gps=&uart;void writePinStandby(bool);};
#define IS_ONE_OF(m,one) ((m)==(one))
#define LOG_DEBUG_GPS(...) ((void)0)
#include "pinned.inc"
}

int main()
{
    stock::GPS gps;
    std::vector<char> calls;
    std::vector<char> fail;
    auto succeeds=[&](char step){
        calls.push_back(step);
        for(char rejected:fail)if(step==rejected)return -1;
        return 0;
    };
    auto standby=[&](bool asserted){
        gps.writePinStandby(asserted);
        assert(stock::level==!asserted); // GPIO_ACTIVE_LOW in the XIAO overlay
        return succeeds(asserted?'S':'W');
    };
    auto receiver=[&](bool open){return succeeds(open?'O':'C');};
    auto check=[&](bool on,std::initializer_list<char> faults,bool result,const char *expected){
        calls.clear();fail=faults;
        assert(mt::gnss::setL76kAcquiring(on,standby,receiver)==result);
        assert(calls==std::vector<char>(expected,expected+std::strlen(expected)));
    };
    check(true,{},true,"OW");     // pipe ready before waking the receiver
    check(false,{},true,"SC");    // sleeps before closing the pipe
    check(true,{'O'},false,"OSC");
    check(true,{'W'},false,"OWSC");
    check(true,{'O','S','C'},false,"OSC");
    check(false,{'S'},false,"SC");
    check(false,{'C'},false,"SC");
    assert(stock::serialWrites==0);
    puts("PASS stock L76K standby polarity, bounded power transitions, failure rollback");
}
