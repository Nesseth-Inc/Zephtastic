// SPDX-License-Identifier: GPL-3.0-only
// Adapted from the recovered Qwen proposal; executed only by the host runner.
#include "notifications/HeartbeatPulse.h"
#include <cassert>
#include <cstdio>
using namespace mt::heartbeat;
int main(){
  struct Expected {Mode mode;bool unread,light;uint16_t pulse,off;};
  // Literal donor snapshots, deliberately independent of helper expressions.
  const Expected cases[]={{Mode::All,false,true,20,3980},{Mode::All,true,true,200,3800},
    {Mode::Hb,false,true,20,3980},{Mode::Hb,true,true,20,3980},
    {Mode::Unread,false,false,20,3980},{Mode::Unread,true,true,200,3800},
    {Mode::Off,false,false,20,3980},{Mode::Off,true,false,200,3800}};
  for(const auto &e:cases){const auto p=plan(e.mode,e.unread);assert(p.light==e.light&&p.pulseMs==e.pulse&&p.nextAfterOffMs==e.off);}
  puts("PASS all eight donor mode/unread snapshots include pulse and post-off delay");
  assert(!deadlineReached(99,100)&&deadlineReached(100,100)&&deadlineReached(101,100));
  assert(deadlineReached(0x7fffffff,0)&&!deadlineReached(0x80000000,0));
  assert(!deadlineReached(0xffffffff,0)&&deadlineReached(0,0));
  puts("PASS deadline equality and documented unsigned half-range boundaries");
  assert(rearmFrom(0xfffffff0,20)==4&&rearmFrom(0xffffff00,4000)==3744);
  assert(!deadlineReached(3,4)&&deadlineReached(4,4)&&deadlineReached(5,4));
  assert(!deadlineReached(0xfffffff0,4));
  puts("PASS one pulse and one cycle cross uint32 rollover without signed casts");
  // Rearm from the current poll, never from an old deadline/catch-up loop.
  assert(rearmFrom(100000,3980)==103980&&rearmFrom(100000,20)==100020);
  assert(rearmFrom(0,0)==0&&deadlineReached(0,rearmFrom(0,0)));
  puts("PASS late-poll rearm and time-zero have explicit literal outcomes");
  puts("HEARTBEAT_PULSE_GROUPS 4");
}
