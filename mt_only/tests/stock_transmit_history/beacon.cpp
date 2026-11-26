// SPDX-License-Identifier: GPL-3.0-only
// Reuse the existing real beacon, radio, settings, wire and private ownership fixture.
#include "beacon-harness.inc"
using mt::history::Clock;using mt::history::Kind;using mt::history::TransmitHistory;
static Fixture *owner=nullptr;static Clock tick{};
static int preWave(){
  auto &f=*owner;const bool rx=f.service.receiving(),armed=f.service.endpointArmed();
  const int rc=f.service.pauseForStorage();if(rc)return rc;
  assert(f.service.storageQuiescent()&&!f.radio.state);
  auto &h=*f.service.broadcastHistory;h.record(Kind::Beacon);const int saved=h.save(f.memory,tick.uptimeMs);
  if(rx){assert(f.service.start(false,uint32_t(tick.uptimeMs)));assert(!armed||f.service.armEndpoint(true));}
  return saved;
}
static void bind(Fixture &f,TransmitHistory &h) {
  assert(!h.load(f.memory,0x12345678,nullptr,[](){return tick;}));
  f.service.broadcastHistory=&h;f.service.saveBeaconHistory=preWave;owner=&f;
}
static void poll(Fixture &f,mt::modules::BeaconController &owner,uint64_t ms,uint32_t utc){
  tick={ms,utc};assert(f.clock.set(ms));
  owner.poll(f.service,f.edit,true,ms,utc,777,[&](const uint8_t *p,size_t n,uint32_t id){assert(id);f.local.emplace_back(p,p+n);});
}
int main(){
  {
    Fixture f;f.config();TransmitHistory h;bind(f,h);
    tick={12000,1800000000};assert(!f.edit.markBeaconSent(tick.utc,uint32_t(tick.uptimeMs)));
    const unsigned before=f.memory.writes;poll(f,f.controller,60000,1800000060);
    assert(!h.has(Kind::Beacon)&&f.local.empty()&&f.memory.writes==before);
    poll(f,f.controller,3600000,1800003600);
    assert(h.has(Kind::Beacon)&&f.local.size()==1&&f.memory.writes==before+1);
    assert(f.edit.config().lastSentUtc()==1800000000); // Existing key remains readable on downgrade.
    Fixture reboot;reboot.memory.files=f.memory.files;assert(!reboot.journal.load(reboot.memory)&&reboot.edit.load());
    TransmitHistory restored;bind(reboot,restored);
    poll(reboot,reboot.controller,60000,1800003659);assert(reboot.local.empty()&&!reboot.service.moduleAnnouncementPending());
  }
  {
    Fixture f;f.config();TransmitHistory h;bind(f,h);f.memory.fail=true;
    poll(f,f.controller,60000,1800000000);assert(f.local.empty()&&!f.service.moduleAnnouncementPending());
  }
  puts("PASS MeshBeacon honors a previous ZT timestamp, writes exactly one verified new bank before RF, restores and rejects failed storage");
}
