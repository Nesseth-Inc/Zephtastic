// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
int main(){
 for(uint8_t model:{81,127})for(bool ble:{false,true}){
  auto f=std::make_unique<Fixture>(model);
  for(uint32_t secs:{86401u,604800u,2147483u}){
   f->admin(device(secs),ble);assert((ble?f->ble:f->usb).error()==0);
   assert(getter(*f,!ble)==secs&&f->service.config.valid());
   mt::SettingsJournal reload;assert(reload.load(f->backend)==0&&reload.config().node().interval()==secs);
   auto writes=f->backend.writes;f->admin(device(secs),ble);assert(f->backend.writes==writes);
  }
  auto before=f->service.config;auto writes=f->backend.writes;
  for(uint32_t secs:{2147484u,2147483647u,UINT32_MAX}){
   f->admin(device(secs),ble);assert((ble?f->ble:f->usb).error()==meshtastic_Routing_Error_BAD_REQUEST);
   assert(f->service.config.same(before)&&f->backend.writes==writes);
  }
  assert(!f->radio.sends&&!f->radio.starts);
 }
 std::cout<<"PASS extended intervals, durable reload, no-op and rejected overflow on both models/transports\n";
 for(uint32_t start:{0u,0x7fffffffu,0xfffffff0u})for(uint32_t secs:{86401u,604800u,2147483u}){
  mt::Discovery d;d.observeArm(true,start);d.admitted(start,secs);
  auto due=start+secs*1000u;
  assert(!d.periodic(start)&&!d.periodic(due-1)&&d.periodic(due));
  d.reschedule(3600);assert(!d.periodic(start+3599999u)&&d.periodic(start+3600000u));
  d.reschedule(secs);assert(!d.periodic(due-1)&&d.periodic(due));
  d.cancel();assert(!d.periodic(due));
 }
 std::cout<<"PASS maximum signed deadline and grow/shrink across uint32 clock wrap\n";
 mt::Config base;base.bytes[4]=1;base.setFixedPosition(123,456,true,7);base.setPositionSharing(true);
 base.setTxEnabled(false);base.setChannelMuted(true);base.setDisplay(42,true,55);
 std::vector<uint8_t> old(base.bytes,base.bytes+mt::Config::FormerSize);old[2]=7;memset(old.data()+104,0,6);
 const auto sealOld=[](std::vector<uint8_t> &b){const auto sum=mt::Config::checksum(b.data(),b.size()-2);b[b.size()-2]=uint8_t(sum);b.back()=uint8_t(sum>>8);};sealOld(old);
 mt::Config image;memcpy(image.bytes,old.data(),old.size());assert(image.validBody(7));
 mt::Config next;assert(mt::Config::decode(old.data(),old.size(),next)&&next.bytes[2]==mt::Config::Version);
 assert(!memcmp(next.bytes+3,old.data()+3,101)&&next.role()==1&&next.rebroadcastMode()==0&&!next.primaryName()[0]&&next.modemPreset()==0&&next.channelNumber()==20&&next.positionSharing());
 auto corrupt=old;bench::MtNodeRecord n;memcpy(n.bytes,corrupt.data()+6,72);n.write32(bench::MtNodeRecord::Interval,86401);n.seal();memcpy(corrupt.data()+6,n.bytes,72);sealOld(corrupt);
 const auto before=next;assert(!mt::Config::decode(corrupt.data(),corrupt.size(),next)&&next.same(before));
 auto future=next;future.bytes[2]=mt::Config::Version+1;future.seal();assert(!mt::Config::decode(future.bytes,future.Size,next)&&next.same(before));
 std::cout<<"PASS v7 migration preserves all settings/consent, rejects invalid legacy and future records\n";
}
