// SPDX-License-Identifier: GPL-3.0-only
// Coordinated Config + NodeDB commit tests. Host-only; no serial, RF or keys.
#include "SettingsCommit.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "pb_encode.h"

static constexpr uint32_t Self=0x12345678,Peer=0x87654321;
using Bytes=std::vector<uint8_t>;

struct Backend final:mt::SettingsBackend {
  std::map<std::string,Bytes> files;unsigned reads=0,writes=0,configWrites=0,nodeWrites=0;
  std::string failPrefix;
  int read(const char *key,uint8_t *out,size_t capacity)override {
    ++reads;auto it=files.find(key);if(it==files.end())return -ENOENT;
    if(it->second.size()>capacity)return -E2BIG;
    if(!it->second.empty())memcpy(out,it->second.data(),it->second.size());
    return int(it->second.size());
  }
  int write(const char *key,const uint8_t *data,size_t size)override {
    ++writes;const std::string name(key);
    if(name.find("state")!=std::string::npos)++configWrites;
    if(name.find("nodes")!=std::string::npos)++nodeWrites;
    if(!failPrefix.empty()&&name.rfind(failPrefix,0)==0)return -EIO;
    files[name]=Bytes(data,data+size);return 0;
  }
};

struct Radio final:mt::Radio {
  int state=0,stopError=0,storageStopError=0,startError=0;bool available=true,busyState=false;
  bool complete=false,rxEvent=false,injectRxOnStorageStop=false;
  unsigned starts=0,stops=0,storageStops=0,sends=0;
  bool ready()override{return available;}
  int start()override{++starts;if(startError)return startError;state=1;return 0;}
  int restore()override{state=1;return 0;}
  int stop()override{++stops;if(stopError)return stopError;state=0;return 0;}
  int stopForStorage()override{
    ++storageStops;
    if(rxEvent)return -EAGAIN;
    if(injectRxOnStorageStop){injectRxOnStorageStop=false;rxEvent=true;return -EAGAIN;}
    if(storageStopError){if(storageStopError==-ESHUTDOWN)state=0;return storageStopError;}
    return stop();
  }
  int health()override{return state;}
  bool busy()override{return busyState;}
  uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *,size_t)override{++sends;return 0;}
  bool completion(int &result)override{
    if(!complete)return false;
    complete=false;result=0;return true;
  }
  bool takeRx(){const bool ready=rxEvent;rxEvent=false;return ready;}
};

static mt::NodeCache::Entry peer() {
  mt::NodeCache::Entry e;e.identity(Peer,127,1);
  assert(mt::NodeCache::Entry::text(
    e.bytes+mt::NodeCache::Entry::Name,40,"Commit peer",12));
  assert(mt::NodeCache::Entry::text(
    e.bytes+mt::NodeCache::Entry::Short,5,"CP",3));
  return e;
}

struct Fixture {
  Radio radio;Backend backend;mt::SettingsJournal settingsJournal;
  mt::test::KnownChannelFixtureClock knownChannelClock;
  mt::Service service;mt::SettingsEdit edit;mt::NodeJournal nodeJournal;
  mt::SettingsCommit coordinator;
  Fixture(bool enabled=true,bool discovery=false):service(radio,true,Self,81,true,discovery),
    edit(service,settingsJournal,backend),
    coordinator(service,radio,edit,nodeJournal,backend) {
    knownChannelClock.bind(service);
    assert(settingsJournal.load(backend)==0);
    auto config=settingsJournal.config();config.bytes[4]=1;config.setTxEnabled(enabled);
    assert(settingsJournal.save(backend,config)==0&&service.apply(config));
    assert(nodeJournal.load(backend,service.nodes,Self)==0);
    assert(service.start(false,0)&&service.armEndpoint(enabled));
  }
  void dirtyNode(){assert(service.nodes.accept(peer(),Self,1,0)==0&&service.nodes.dirty());}
  void begin(){assert(edit.begin(10)==0&&edit.open());}
  void dirtyConfig(){
    auto config=service.config;assert(config.setDisplay(45,true));
    assert(edit.save(config,20)==0&&edit.dirty());
  }
};

static Bytes inboundText(Fixture &f,uint32_t id) {
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
  data.has_bitfield=true;data.payload.size=1;data.payload.bytes[0]='x';
  uint8_t encoded[64];auto stream=pb_ostream_from_buffer(encoded,sizeof(encoded));
  assert(pb_encode(&stream,meshtastic_Data_fields,&data));
  Bytes wire(255);size_t length=0;
  assert(f.service.codec.encodeZeroHopData(
    Peer,Self,id,encoded,stream.bytes_written,wire.data(),wire.size(),length));
  wire.resize(length);wire[12]=8;return wire;
}

static Bytes outboundText(Fixture &f,uint32_t id) {
  Bytes wire(255);size_t length=0;
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
  data.has_bitfield=true;data.payload.size=1;data.payload.bytes[0]='o';
  uint8_t encoded[64];auto stream=pb_ostream_from_buffer(encoded,sizeof(encoded));
  assert(pb_encode(&stream,meshtastic_Data_fields,&data));
  assert(f.service.codec.encodeZeroHopData(
    Self,Peer,id,encoded,stream.bytes_written,wire.data(),wire.size(),length));
  wire.resize(length);wire[12]=0;return wire;
}

static mt::Config changedDisplay(const Fixture &f) {
  auto config=f.service.config;assert(config.setDisplay(45,true));return config;
}

static Bytes inboundNodeRequest(Fixture &f,uint32_t id) {
  bench::Frame frame;
  assert(mt::Discovery::encode(
    f.service.codec,f.service.config.node(),Peer,127,Self,id,true,0,frame));
  frame.bytes[12]=0;return {frame.bytes,frame.bytes+frame.len};
}

static unsigned groups=0;
#define PASS(message) do {++groups;puts(message);} while(0)

int main() {
  {
    Fixture f;auto original=f.settingsJournal.config();const auto first=f.settingsJournal.channelGeneration();
    auto display=original;assert(display.setDisplay(45,true)&&f.settingsJournal.save(f.backend,display)==0);
    mt::SettingsJournal reboot;assert(reboot.load(f.backend)==0&&reboot.channelGeneration()==first);
    auto alternate=display;assert(alternate.setPrimaryName("Secondary")&&reboot.save(f.backend,alternate)==0);
    assert(reboot.channelGeneration()==first+1&&reboot.save(f.backend,display)==0);
    mt::SettingsJournal restored;assert(restored.load(f.backend)==0&&restored.channelGeneration()==first+2);
    PASS("PASS display edit preserves waypoint scope, channel away and back invalidates history across reboot");
  }
  {
    Fixture f;const auto writes=f.backend.writes;const auto starts=f.radio.starts,stops=f.radio.stops;
    assert(f.coordinator.commit(30)==0&&f.backend.writes==writes);
    assert(f.radio.starts==starts&&f.radio.stops==stops&&f.service.receiving()&&f.service.endpointArmed());
    PASS("PASS clean no-open commit is inert and preserves normal RX/TX");
  }
  {
    Fixture f;f.begin();const auto writes=f.backend.writes;assert(!f.service.endpointArmed());
    assert(f.coordinator.commit(30)==0&&!f.edit.open()&&f.backend.writes==writes);
    assert(f.service.receiving()&&f.service.endpointArmed()&&f.radio.stops==2&&f.radio.starts==3);
    PASS("PASS clean open transaction quiesces without a write and restores durable enable");
  }
  {
    Fixture f;f.dirtyNode();const auto configWrites=f.backend.configWrites;
    assert(f.coordinator.commit(40)==0&&!f.service.nodes.dirty());
    assert(f.backend.configWrites==configWrites&&f.backend.nodeWrites==1);
    assert(f.service.receiving()&&f.service.endpointArmed());
    mt::NodeCache restored;mt::NodeJournal reboot;
    assert(reboot.load(f.backend,restored,Self)==0&&restored.find(Peer)>=0);
    PASS("PASS commit flushes dirty NodeDB without a Config write and restores normal state");
  }
  {
    Fixture f;f.dirtyNode();f.begin();f.dirtyConfig();
    const auto configWrites=f.backend.configWrites,nodeWrites=f.backend.nodeWrites;
    assert(f.coordinator.commit(50)==0&&!f.edit.open()&&!f.service.nodes.dirty());
    assert(f.backend.configWrites==configWrites+1&&f.backend.nodeWrites==nodeWrites+1);
    mt::SettingsJournal configReboot;mt::NodeJournal nodesReboot;mt::NodeCache restored;
    assert(configReboot.load(f.backend)==0&&configReboot.config().displaySeconds()==45&&configReboot.config().displayFlip());
    assert(nodesReboot.load(f.backend,restored,Self)==0&&restored.find(Peer)>=0);
    assert(f.service.receiving()&&f.service.endpointArmed());
    PASS("PASS one explicit commit verifies both dirty journals before restoring RX/TX");
  }
  {
    Fixture f(false);f.dirtyNode();assert(f.coordinator.commit(60)==0);
    assert(f.service.receiving()&&!f.service.endpointArmed()&&
           f.service.config.txIntent()==mt::Config::TxIntent::Disabled);
    PASS("PASS coordinated commit preserves durable explicit TX disable");
  }
  {
    Fixture f;f.dirtyNode();f.radio.busyState=true;const auto writes=f.backend.writes;
    assert(f.coordinator.commit(70)==-EBUSY&&f.backend.writes==writes&&
           f.service.nodes.dirty()&&f.service.receiving()&&f.service.endpointArmed());
    PASS("PASS native busy guard defers without stopping RF or touching storage");
  }
  {
    Fixture f;const auto wire=inboundText(f,690);
    f.service.rx(wire.data(),wire.size(),0,200,691);
    const auto writes=f.backend.writes,stops=f.radio.stops,storageStops=f.radio.storageStops;
    const auto before=f.service.config;
    assert(f.edit.save(changedDisplay(f),201)==-EBUSY&&f.backend.writes==writes&&
           f.radio.stops==stops&&f.radio.storageStops==storageStops&&f.service.config.same(before));
    f.service.poll(500,692);assert(f.radio.sends==1);
    PASS("PASS direct changed setting preserves queued automatic control work");
  }
  {
    Fixture f;const auto wire=outboundText(f,695);
    assert(f.service.submitMessage(0,wire.data(),wire.size(),false,200)==0);
    const auto writes=f.backend.writes,stops=f.radio.stops;const auto before=f.service.config;
    assert(f.edit.save(changedDisplay(f),201)==-EBUSY&&f.backend.writes==writes&&
           f.radio.stops==stops&&f.service.config.same(before));
    f.service.poll(500,696);assert(f.radio.sends==1);
    PASS("PASS direct changed setting preserves queued user work");
  }
  {
    Fixture f;const auto wire=outboundText(f,697);
    assert(f.service.submitMessage(0,wire.data(),wire.size(),true,200)==0);
    f.service.poll(300,698);assert(f.service.active()&&f.radio.sends==1);
    const auto writes=f.backend.writes,stops=f.radio.stops;const auto before=f.service.config;
    assert(f.edit.save(changedDisplay(f),301)==-EBUSY&&f.backend.writes==writes&&
           f.radio.stops==stops&&f.service.config.same(before));
    f.radio.complete=true;f.service.poll(400,0);assert(!f.service.active());
    PASS("PASS direct changed setting preserves native-active user work");
  }
  {
    for(unsigned operation=0;operation<3;++operation)for(bool raced:{false,true}) {
      Fixture f;if(operation==2)f.dirtyNode();
      if(raced)f.radio.injectRxOnStorageStop=true;else f.radio.rxEvent=true;
      const auto writes=f.backend.writes,stops=f.radio.stops;const auto before=f.service.config;
      int result=0;
      if(operation==0)result=f.edit.begin(200);
      else if(operation==1)result=f.edit.save(changedDisplay(f),200);
      else result=f.coordinator.commit(200);
      assert(result==-EBUSY&&f.backend.writes==writes&&f.radio.stops==stops&&
             f.radio.rxEvent&&f.service.receiving()&&f.service.endpointArmed()&&
             f.service.config.same(before));
      if(operation==0)assert(!f.edit.open());
      assert(f.radio.takeRx());
    }
    PASS("PASS begin, direct save and commit preserve prequeued and stop-racing RX events");
  }
  {
    Fixture f;f.begin();const auto before=f.service.config;
    f.radio.rxEvent=true;const auto writes=f.backend.writes,stops=f.radio.stops;
    assert(f.edit.save(changedDisplay(f),300)==-EBUSY&&f.edit.open()&&!f.edit.dirty()&&
           f.backend.writes==writes&&f.radio.stops==stops&&f.radio.takeRx()&&
           f.service.receiving()&&!f.service.endpointArmed()&&f.service.config.same(before));
    PASS("PASS open-batch save rechecks and preserves a newly queued RX event");
  }
  {
    Fixture f;f.dirtyNode();const auto wire=inboundText(f,700);
    f.service.rx(wire.data(),wire.size(),0,200,701);
    assert(f.service.endpointFree()==4&&!f.service.storageStopSafe());
    const auto writes=f.backend.writes,stops=f.radio.stops;
    assert(f.coordinator.commit(201)==-EBUSY&&f.backend.writes==writes&&
           f.radio.stops==stops&&f.service.receiving()&&f.service.endpointArmed());
    f.service.poll(500,702);assert(f.radio.sends==1);
    PASS("PASS queued automatic ACK blocks commit without cancellation and remains sendable");
  }
  {
    Fixture f;f.dirtyNode();const auto wire=inboundText(f,710);
    f.service.rx(wire.data(),wire.size(),0,200,711);f.service.poll(500,712);
    assert(f.radio.sends==1&&f.service.active());f.radio.complete=true;f.service.poll(600,0);
    assert(!f.service.active()&&f.service.endpointFree()==4&&!f.service.storageStopSafe());
    const auto writes=f.backend.writes,stops=f.radio.stops;
    assert(f.coordinator.commit(601)==-EBUSY&&f.backend.writes==writes&&f.radio.stops==stops);
    f.service.poll(15800,0);f.service.poll(16050,0);assert(f.radio.sends==2);
    PASS("PASS waiting reliable ACK blocks commit without cancellation and preserves retry");
  }
  {
    Fixture f(true,true);f.dirtyNode();const auto wire=inboundNodeRequest(f,720);
    f.service.rx(wire.data(),wire.size(),0,1000,721);
    assert(f.service.endpointFree()==4&&!f.service.storageStopSafe());
    const auto writes=f.backend.writes,stops=f.radio.stops;
    assert(f.coordinator.commit(1001)==-EBUSY&&f.backend.writes==writes&&f.radio.stops==stops);
    f.service.poll(1300,722);assert(f.radio.sends==1);
    PASS("PASS queued automatic NodeInfo response blocks commit and remains sendable");
  }
  {
    Fixture f;const auto wire=inboundText(f,730);
    f.service.rx(wire.data(),wire.size(),0,200,731);
    const auto writes=f.backend.writes,stops=f.radio.stops;
    assert(f.edit.begin(201)==-EBUSY&&!f.edit.open()&&f.backend.writes==writes&&
           f.radio.stops==stops&&f.service.endpointArmed());
    f.service.poll(500,732);assert(f.radio.sends==1);
    PASS("PASS begin-edit refuses queued control work without disarm or cancellation");
  }
  {
    Fixture f;f.dirtyNode();f.radio.stopError=-EIO;const auto writes=f.backend.writes;
    assert(f.coordinator.commit(80)==-EIO&&f.backend.writes==writes&&
           f.service.faulted()&&f.service.receiving()&&f.service.nodes.dirty()&&f.radio.state==1);
    PASS("PASS unknown physical stop failure performs no write and preserves honest live service state while faulted");
  }
  {
    Fixture f;f.dirtyNode();f.radio.storageStopError=-ESHUTDOWN;const auto writes=f.backend.writes;
    assert(f.coordinator.commit(81)==-ESHUTDOWN&&f.backend.writes==writes&&
           f.service.faulted()&&!f.service.receiving()&&f.service.nodes.dirty()&&f.radio.state==0);
    PASS("PASS verified standby cleanup failure performs no write and settles service RF state while faulted");
  }
  {
    Fixture f;f.dirtyNode();f.begin();f.dirtyConfig();
    f.backend.failPrefix="mtbase/state";const auto nodeWrites=f.backend.nodeWrites;
    assert(f.coordinator.commit(90)==-EIO&&f.backend.nodeWrites==nodeWrites&&
           f.edit.open()&&f.edit.dirty()&&f.service.nodes.dirty()&&!f.service.receiving());
    const auto writes=f.backend.writes;assert(f.coordinator.commit(91)==-EACCES&&f.backend.writes==writes);
    PASS("PASS Config failure blocks NodeDB write, RF restore and blind retry");
  }
  {
    Fixture f;f.dirtyNode();f.backend.failPrefix="mtbase/nodes";
    assert(f.coordinator.commit(100)==-EIO&&!f.service.nodes.ready()&&
           !f.service.receiving()&&!f.service.endpointArmed());
    const auto writes=f.backend.writes;assert(f.coordinator.commit(101)==-EACCES&&f.backend.writes==writes);
    PASS("PASS NodeDB uncertainty freezes cache, keeps RF off and blocks blind retry");
  }
  printf("PASS %u coordinated settings commit groups\n",groups);
}
