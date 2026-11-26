// SPDX-License-Identifier: GPL-3.0-only
// In-memory adapter for the actual stock Python client. No device transport.
#include "device_config_fixture.h"
#include <sstream>
static std::string hex(const uint8_t *p,size_t n){
 static const char digits[]="0123456789abcdef";std::string out;
 for(size_t i=0;i<n;++i){out+=digits[p[i]>>4];out+=digits[p[i]&15];}return out;
}
int main(int argc,char **argv) {
 assert(argc==2);const unsigned model=unsigned(std::stoul(argv[1]));assert(model==81||model==127);
 auto f=std::make_unique<Fixture>(model);std::string line;unsigned count=0;
 while(std::getline(std::cin,line)) {
  assert(++count<=128&&line.size()<=1100);std::istringstream in(line);std::string mode,raw;in>>mode;
  bool ble=mode=="ble";f->usb.records.clear();f->ble.records.clear();
  if(mode=="restart") {
   // Recreate all runtime state from the same memory-backed journal, never
   // a firmware reboot command and never a physical device reset.
   auto next=std::make_unique<Fixture>(model);next->backend=f->backend;
   assert(next->journal.load(next->backend)==0&&next->service.apply(next->journal.config()));f=std::move(next);
  } else {
   assert(ble||mode=="usb");in>>raw;assert(raw.size()&&raw.size()%2==0&&raw.size()<=1024);
   std::vector<uint8_t> bytes;
   for(size_t i=0;i<raw.size();i+=2){unsigned v=0;for(unsigned j=0;j<2;++j){const char c=raw[i+j];assert((c>='0'&&c<='9')||(c>='a'&&c<='f'));v=v*16+unsigned(c<='9'?c-'0':c-'a'+10);}bytes.push_back(v);}
   f->raw(bytes.data(),bytes.size(),ble,1000*count);
  }
  const auto &h=ble?f->ble:f->usb;std::cout<<"{\"records_hex\":[";bool first=true;
  for(const auto &r:h.records){if(!first)std::cout<<',';first=false;std::cout<<'\"'<<hex(r.data(),r.size())<<'\"';}
  std::cout<<"],\"writes\":"<<f->backend.writes<<",\"interval\":"<<f->service.config.node().interval()
   <<",\"editing\":"<<f->edit.open()<<",\"dirty\":"<<f->edit.dirty()<<",\"radio_sends\":"<<f->radio.sends
   <<",\"radio_starts\":"<<f->radio.starts<<",\"tx_armed\":"<<f->service.endpointArmed()<<"}"<<std::endl;
  assert(!f->radio.sends&&!f->radio.starts&&!f->service.endpointArmed());
 }
}
