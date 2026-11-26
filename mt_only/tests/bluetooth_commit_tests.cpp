// SPDX-License-Identifier: GPL-3.0-only
#include "BluetoothSettingsCommit.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
struct Backend final:mt::SettingsBackend{
 std::map<std::string,std::vector<uint8_t>> files;unsigned writes=0;int writeError=0;bool badReadback=false;
 int read(const char*k,uint8_t*p,size_t cap)override{
  auto i=files.find(k);if(i==files.end())return -ENOENT;
  if(i->second.size()>cap)return -E2BIG;
  memcpy(p,i->second.data(),i->second.size());if(badReadback&&writes&&i->second.size())p[0]^=1;
  return int(i->second.size());
 }
 int write(const char*k,const uint8_t*p,size_t n)override{++writes;if(writeError)return writeError;files[k]={p,p+n};return 0;}
};
struct Radio final:mt::Radio{
 int state=0,startError=0,pauseError=0;bool busyNow=false;unsigned pauses=0;
 bool ready()override{return true;}int start()override{if(startError)return startError;state=1;return 0;}
 int stop()override{state=0;return 0;}int restore()override{state=1;return 0;}
 int stopForStorage()override{++pauses;return pauseError?pauseError:stop();}
 int health()override{return state;}bool busy()override{return busyNow;}uint32_t airtime(size_t)override{return 100;}
 int send(uint8_t*,size_t)override{return 0;}bool completion(int&)override{return false;}
};
struct Fixture{
 Radio radio;Backend backend;mt::BluetoothJournal journal;mt::Service service;
 mt::test::KnownChannelFixtureClock clock;
 explicit Fixture(bool armed=true):service(radio,true,0x12345678,127,true,false){
  clock.bind(service);mt::Config c;c.bytes[4]=1;c.setTxEnabled(true);auto node=c.node();node.setPublic();c.node(node);
  assert(service.apply(c)&&service.start(false,0)&&service.armEndpoint(armed)&&journal.load(backend)==0);
 }
 int save(const mt::BluetoothSettings&c,bool force=false){return mt::BluetoothSettingsCommit::save(service,radio,journal,backend,c,10,force);}
};
int main(){
 mt::BluetoothSettings candidate;candidate.mode=mt::BluetoothSettings::FixedPin;candidate.fixedPin=0;
 for(bool armed:{false,true}){Fixture f(armed);assert(!f.save(candidate)&&f.backend.writes==1&&f.journal.config().same(candidate));assert(f.service.receiving()&&f.service.endpointArmed()==armed);}
 puts("PASS Bluetooth config save verifies its journal and restores exact prior RX/armed intent");
 Fixture inert;assert(!inert.save(mt::BluetoothSettings{})&&!inert.radio.pauses&&!inert.backend.writes);
 inert.radio.busyNow=true;assert(inert.save(candidate)==-EBUSY&&!inert.backend.writes&&inert.service.receiving());
 Fixture pause;pause.radio.pauseError=-EAGAIN;assert(pause.save(candidate)==-EBUSY&&!pause.backend.writes&&pause.service.receiving());
 puts("PASS Bluetooth no-op, native busy and raced RX admission never report a save or discard RX");
 for(int error:{-EIO,-ENOSPC,-ENOMEM,-E2BIG,-EBUSY}){Fixture f;f.backend.writeError=error;assert(f.save(candidate)==-EIO);assert(f.journal.state()==mt::BluetoothJournal::State::IoError&&f.service.receiving()&&f.service.endpointArmed());assert(f.save(candidate)==-EACCES&&f.backend.writes==1);}
 Fixture corrupt;corrupt.backend.badReadback=true;assert(corrupt.save(candidate)==-EIO&&corrupt.journal.state()==mt::BluetoothJournal::State::IoError&&corrupt.service.receiving()&&corrupt.service.endpointArmed());
 for(int error:{-ESHUTDOWN,-ENODEV,-EBUSY}){Fixture f;f.radio.pauseError=error;assert(f.save(candidate)==-EIO&&f.service.faulted()&&!f.backend.writes);}
 puts("PASS Bluetooth native/storage uncertainty maps to interface error, poisons uncertain writes and preserves settlement");
 Fixture restore;restore.radio.startError=-EIO;assert(restore.save(candidate)==-EIO&&restore.backend.writes==1&&restore.journal.config().same(candidate));assert(!restore.service.receiving()&&!restore.service.endpointArmed());
 puts("PASS Bluetooth committed config with RX restore failure is reported degraded and never as success");
 Fixture recovered;assert(!recovered.journal.save(recovered.backend,candidate));auto newer=candidate;newer.fixedPin=999999;assert(!recovered.journal.save(recovered.backend,newer));recovered.backend.files["mtble/state1"][0]^=1;assert(!recovered.journal.load(recovered.backend)&&recovered.journal.state()==mt::BluetoothJournal::State::Recovered);assert(recovered.save(candidate)==-EACCES);assert(!recovered.save(candidate,true)&&recovered.backend.writes==3&&recovered.journal.state()==mt::BluetoothJournal::State::Loaded);
 puts("PASS Bluetooth single-good-slot policy requires explicit verified rewrite before recovery clears");
}
