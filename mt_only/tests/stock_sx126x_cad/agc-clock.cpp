#include "../../src/AgcMaintenance.h"
#include <cassert>

int main() {
    mt::AgcMaintenance clock;
    clock.poll(59999,0,true); assert(!clock.take(59999,0));
    clock.poll(60000,1,true); assert(clock.take(60000,1)); clock.succeeded(60000);
    clock.poll(120000,119998,true); assert(!clock.take(120000,119998));
    clock.poll(180000,119998,true); assert(!clock.take(180020,180005)); // heard before resume
    clock.poll(240000,119998,false); assert(!clock.take(240000,119998)); // queued TX
    clock.poll(300000,119998,true); assert(clock.take(300000,119998)); // uncredited failure
    clock.poll(360000,119998,true); assert(clock.take(360000,119998)); clock.succeeded(360000);
    constexpr uint32_t day = 86400000;
    clock.poll(day+300000,day+299999,true); assert(!clock.take(day+300000,day+299999));
    clock.poll(day+360000,day+359999,true); assert(clock.take(day+360000,day+359999));
    clock.succeeded(UINT32_MAX-1000); clock.poll(UINT32_MAX-20,UINT32_MAX-50,true);
    clock.poll(60000,59000,true); assert(!clock.take(60000,59000));
}
