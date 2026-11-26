// SPDX-License-Identifier: GPL-3.0-only
#include "notifications/HeartbeatController.h"
#include <cassert>
#include <cstdio>
using mt::heartbeat::Mode;
int main(){
  mt::HeartbeatController c;
  c.poll(Mode::All,false,100);assert(!c.light());
  c.configure(true,100);c.poll(Mode::All,false,100);assert(c.light());
  c.poll(Mode::All,false,119);assert(c.light());
  c.poll(Mode::All,false,120);assert(!c.light());
  c.poll(Mode::All,false,4099);assert(!c.light());
  c.poll(Mode::All,false,4100);assert(c.light());
  c.configure(false,4101);assert(!c.light());
  c.poll(Mode::All,false,10000);assert(!c.light());
  puts("PASS disabled/immediate-start/20ms pulse/4000ms cycle");
  c.configure(true,UINT32_MAX-9);c.poll(Mode::All,false,UINT32_MAX-9);assert(c.light());
  c.poll(Mode::All,false,9);assert(c.light());c.poll(Mode::All,false,10);assert(!c.light());
  c.poll(Mode::All,false,3989);assert(!c.light());c.poll(Mode::All,false,3990);assert(c.light());
  puts("PASS wrap-safe independent pulse and cycle deadlines");
  c.configure(false,0);c.configure(true,0);c.poll(Mode::All,false,0);assert(c.light());
  c.poll(Mode::All,false,100000);assert(!c.light());c.poll(Mode::All,false,100000);assert(!c.light());
  c.poll(Mode::All,false,103979);assert(!c.light());c.poll(Mode::All,false,103980);assert(c.light());
  puts("PASS late poll makes one transition and rearms from now without catch-up");
  c.configure(false,0);c.configure(true,0);c.poll(Mode::Unread,false,0);assert(!c.light());
  c.poll(Mode::Unread,true,20);assert(!c.light()); // off uses fresh unread width200
  c.poll(Mode::Unread,true,3819);assert(!c.light());c.poll(Mode::Unread,true,3820);assert(c.light());
  c.poll(Mode::Unread,true,4019);assert(c.light());c.poll(Mode::Unread,true,4020);assert(!c.light());
  c.configure(false,0);c.configure(true,0);c.poll(Mode::Off,true,0);assert(!c.light());
  c.poll(Mode::Off,true,200);assert(!c.light());c.poll(Mode::Hb,true,4000);assert(c.light());
  c.poll(Mode::Hb,true,4020);assert(!c.light());
  puts("PASS donor unread/off/HB timing with fresh phase snapshots");
  puts("HEARTBEAT_CONTROLLER_GROUPS 4");
}
