// SPDX-License-Identifier: GPL-3.0-only
#include "StoreForwardControl.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

using mt::modules::StoreForwardArchive;
using mt::modules::StoreForwardControl;

namespace {
unsigned groups = 0;
void pass(const char *name) { ++groups; printf("PASS %s\n", name); }

struct Fixture {
  StoreForwardArchive::Record records[8]{};
  StoreForwardArchive::Cursor cursors[8]{};
  StoreForwardArchive archive{records, 8, cursors, 8};

  void add(uint32_t time, uint32_t from, uint32_t to, uint32_t id) {
    static const uint8_t text[] = {'x'};
    StoreForwardArchive::MessageView m{};
    m.time = time;
    m.from = from;
    m.to = to;
    m.id = id;
    m.payload = text;
    m.payloadSize = sizeof(text);
    assert(archive.add(m) == StoreForwardArchive::AddResult::Added);
  }
};

StoreForwardControl::Config enabledConfig(uint32_t max=25, uint32_t window=240) {
  StoreForwardControl::Config c{};
  c.enabled = true;
  c.isServer = true;
  c.returnMax = max;
  c.returnWindowMinutes = window;
  return c;
}

meshtastic_StoreAndForward historyRequest(uint32_t minutes=0) {
  meshtastic_StoreAndForward m = meshtastic_StoreAndForward_init_zero;
  m.rr = meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY;
  if (minutes) {
    m.which_variant = meshtastic_StoreAndForward_history_tag;
    m.variant.history.window = minutes;
  }
  return m;
}

void disabledPublicAndBusyDenials() {
  Fixture f;
  StoreForwardControl::Config off{};
  StoreForwardControl controlOff(f.archive, off);
  StoreForwardControl::Context context{};
  context.peer=41;context.nowEpochSeconds=10000;context.wantResponse=true;
  auto request=historyRequest();
  auto action=controlOff.receive(request,context);
  assert(action.kind==StoreForwardControl::ActionKind::None);
  assert(controlOff.requests()==0);

  StoreForwardControl control(f.archive,enabledConfig());
  context.defaultChannel=true;
  action=control.receive(request,context);
  assert(action.kind==StoreForwardControl::ActionKind::ErrorText);
  assert(action.errorText==StoreForwardControl::ErrorTextKind::PublicChannel);
  assert(action.wantResponse&&action.countsAsResponse);
  assert(control.historyRequests()==1&&!control.active());

  context.defaultChannel=false;
  f.add(9999,7,StoreForwardArchive::BroadcastNode,1);
  action=control.receive(request,context);
  assert(action.kind==StoreForwardControl::ActionKind::ControlMessage);
  assert(action.control.rr==meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY);
  assert(action.control.variant.history.history_messages==1);
  assert(action.control.variant.history.window==240u*60u*1000u);
  assert(action.requestWindowSeconds==240u*60u);
  assert(control.active()&&control.activeRequester()==41);

  context.peer=42;
  action=control.receive(request,context);
  assert(action.kind==StoreForwardControl::ActionKind::ErrorText);
  assert(action.errorText==StoreForwardControl::ErrorTextKind::Busy);
  assert(control.activeRequester()==41);
  meshtastic_StoreAndForward stats=meshtastic_StoreAndForward_init_zero;
  stats.rr=meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS;
  action=control.receive(stats,context);
  assert(action.kind==StoreForwardControl::ActionKind::ControlMessage);
  assert(action.control.rr==meshtastic_StoreAndForward_RequestResponse_ROUTER_BUSY);
  pass("disabled admission public denial busy denial and default window");
}

void selectedWindowCursorAndRequesterAbort() {
  Fixture f;
  f.add(0,3,StoreForwardArchive::BroadcastNode,1); // no timestamp is excluded
  f.add(9879,3,StoreForwardArchive::BroadcastNode,2); // <= cutoff excluded
  f.add(9881,3,StoreForwardArchive::BroadcastNode,3); // eligible in 2 min
  f.add(9900,50,StoreForwardArchive::BroadcastNode,4); // eligible broadcast
  f.add(9910,41,StoreForwardArchive::BroadcastNode,5); // requester source excluded
  f.add(9920,3,99,6); // directed elsewhere excluded
  f.add(9930,3,41,7); // directed to requester eligible

  StoreForwardControl control(f.archive,enabledConfig());
  StoreForwardControl::Context ctx{};
  ctx.peer=41;ctx.nowMillis=500;ctx.nowEpochSeconds=10000;
  auto action=control.receive(historyRequest(2),ctx);
  assert(action.kind==StoreForwardControl::ActionKind::ControlMessage);
  assert(action.control.variant.history.history_messages==3);
  assert(action.control.variant.history.window==120000);
  assert(action.requestWindowSeconds==120);
  assert(action.control.variant.history.last_request==0);

  ctx.peer=42;
  meshtastic_StoreAndForward abort=meshtastic_StoreAndForward_init_zero;
  abort.rr=meshtastic_StoreAndForward_RequestResponse_CLIENT_ABORT;
  action=control.receive(abort,ctx);
  assert(action.kind==StoreForwardControl::ActionKind::None);
  assert(control.activeRequester()==41);
  ctx.peer=41;
  action=control.receive(abort,ctx);
  assert(action.kind==StoreForwardControl::ActionKind::Cancelled);
  assert(!control.active());
  size_t cursor=99;
  assert(f.archive.cursor(41,cursor)&&cursor==0); // abort before replay did not advance it
  pass("minute request window strict filtering cursor and requester-only abort");
}

void pacingUtilizationAndBoundedReplay() {
  Fixture f;
  for(uint32_t i=0;i<4;++i) f.add(100+i,7,StoreForwardArchive::BroadcastNode,10+i);
  StoreForwardControl control(f.archive,enabledConfig(2,240));
  StoreForwardControl::Context ctx{};
  ctx.peer=41;ctx.nowEpochSeconds=1000;ctx.nowMillis=1000;
  auto action=control.receive(historyRequest(),ctx);
  assert(action.control.variant.history.history_messages==2);
  assert(control.active());
  action=control.poll(5999,true);
  assert(action.kind==StoreForwardControl::ActionKind::None);
  action=control.poll(6000,false);
  assert(action.kind==StoreForwardControl::ActionKind::None);
  action=control.poll(6000,true);
  assert(action.kind==StoreForwardControl::ActionKind::Replay);
  assert(action.destination==41&&action.replay.id==10&&!action.finalReplay);
  action=control.poll(10999,true);
  assert(action.kind==StoreForwardControl::ActionKind::None);
  action=control.poll(11000,true);
  assert(action.kind==StoreForwardControl::ActionKind::Replay);
  assert(action.replay.id==11&&action.finalReplay);
  // Caller decides whether delivery was admitted, then ends the bounded run.
  control.finishReplay();
  assert(!control.active());
  size_t cursor=0;
  assert(f.archive.cursor(41,cursor)&&cursor==2);
  pass("5s pacing utilization gate and return maximum");
}

void emptyStatsPingAndHeartbeat() {
  Fixture f;
  auto cfg=enabledConfig();
  cfg.returnMax=0; // stock zero override retains module default
  cfg.returnWindowMinutes=0;
  cfg.heartbeat=true;
  StoreForwardControl control(f.archive,cfg);
  StoreForwardControl::Context ctx{};
  ctx.peer=9;ctx.nowMillis=1600;ctx.nowEpochSeconds=500;
  meshtastic_StoreAndForward ping=meshtastic_StoreAndForward_init_zero;
  ping.rr=meshtastic_StoreAndForward_RequestResponse_CLIENT_PING;
  auto action=control.receive(ping,ctx);
  assert(action.kind==StoreForwardControl::ActionKind::ControlMessage);
  assert(action.control.rr==meshtastic_StoreAndForward_RequestResponse_ROUTER_PONG);

  meshtastic_StoreAndForward stats=meshtastic_StoreAndForward_init_zero;
  stats.rr=meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS;
  action=control.receive(stats,ctx);
  assert(action.control.rr==meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS);
  assert(action.control.variant.stats.messages_total==8);
  assert(action.control.variant.stats.messages_saved==0);
  assert(action.control.variant.stats.up_time==1);
  assert(action.control.variant.stats.return_max==25);
  assert(action.control.variant.stats.return_window==240);
  assert(action.control.variant.stats.heartbeat);

  action=control.receive(historyRequest(),ctx);
  assert(action.control.variant.history.history_messages==0);
  assert(!control.active()); // empty request still gets history summary

  action=control.poll(899999,true);
  assert(action.kind==StoreForwardControl::ActionKind::None);
  action=control.poll(900000,false);
  assert(action.kind==StoreForwardControl::ActionKind::None);
  action=control.poll(900000,true);
  assert(action.kind==StoreForwardControl::ActionKind::ControlMessage);
  assert(action.destination==StoreForwardControl::BroadcastNode);
  assert(action.control.rr==meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT);
  assert(action.control.variant.heartbeat.period==900);

  meshtastic_StoreAndForward received=meshtastic_StoreAndForward_init_zero;
  received.rr=meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT;
  received.which_variant=meshtastic_StoreAndForward_heartbeat_tag;
  received.variant.heartbeat.period=45;
  control.observeRouterHeartbeat(received,12345);
  assert(control.observedHeartbeatPeriodSeconds()==45);
  assert(control.heartbeatPeriodSeconds()==900&&control.lastHeartbeatMillis()==12345);
  pass("empty history stats ping pong heartbeat defaults and observation");
}

void tickWrapAndDisableCancellation() {
  Fixture f;
  f.add(900,4,StoreForwardArchive::BroadcastNode,55);
  StoreForwardControl control(f.archive,enabledConfig());
  StoreForwardControl::Context ctx{};
  ctx.peer=77;ctx.nowMillis=UINT32_MAX-2000;ctx.nowEpochSeconds=1000;
  auto action=control.receive(historyRequest(),ctx);
  assert(action.kind==StoreForwardControl::ActionKind::ControlMessage);
  const uint32_t deadline=ctx.nowMillis+StoreForwardControl::PacketIntervalMs;
  action=control.poll(deadline-1,true);
  assert(action.kind==StoreForwardControl::ActionKind::None);
  action=control.poll(deadline,true);
  assert(action.kind==StoreForwardControl::ActionKind::Replay);
  auto cfg=enabledConfig();
  cfg.enabled=false;
  control.configure(cfg);
  assert(!control.active());
  assert(control.poll(deadline+5000,true).kind==StoreForwardControl::ActionKind::None);
  pass("wrap-safe deadline and disabling cancels active sender");
}
}  // namespace

int main() {
  disabledPublicAndBusyDenials();
  selectedWindowCursorAndRequesterAbort();
  pacingUtilizationAndBoundedReplay();
  emptyStatsPingAndHeartbeat();
  tickWrapAndDisableCancellation();
  printf("RESULT PASS groups=%u\n", groups);
  return 0;
}
