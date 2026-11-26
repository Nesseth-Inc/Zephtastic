// SPDX-License-Identifier: GPL-3.0-only
// Line adapter for actual stock Python DisplayConfig vectors; memory only.
#include "device_config_fixture.h"
#include <sstream>
static std::string hex(const uint8_t *p,size_t n) {
  static const char digits[]="0123456789abcdef";std::string out;
  for(size_t i=0;i<n;++i){out+=digits[p[i]>>4];out+=digits[p[i]&15];}return out;
}
int main(int argc,char **argv) {
  assert(argc==2);const unsigned model=unsigned(std::stoul(argv[1]));assert(model==81||model==127);
  auto f=std::make_unique<Fixture>(uint8_t(model));std::string line;unsigned count=0;
  while(std::getline(std::cin,line)) {
    assert(++count<=128&&line.size()<=1100);std::istringstream in(line);std::string mode,raw;in>>mode>>raw;
    const bool ble=mode=="ble";assert(ble||mode=="usb");assert(!raw.empty()&&raw.size()%2==0&&raw.size()<=1024);
    std::vector<uint8_t> bytes;for(size_t i=0;i<raw.size();i+=2) {
      unsigned value=0;for(unsigned j=0;j<2;++j){const char c=raw[i+j];assert((c>='0'&&c<='9')||(c>='a'&&c<='f'));value=value*16+unsigned(c<='9'?c-'0':c-'a'+10);}bytes.push_back(uint8_t(value));
    }
    f->usb.records.clear();f->ble.records.clear();f->raw(bytes.data(),bytes.size(),ble,1000*count);
    const auto &host=ble?f->ble:f->usb;
    std::cout<<"{\"records_hex\":[";bool first=true;for(const auto &record:host.records) {
      if(!first)std::cout<<',';
      first=false;std::cout<<'"'<<hex(record.data(),record.size())<<'"';
    }
    std::cout<<"],\"writes\":"<<f->backend.writes<<",\"saves\":"<<(f->usb.saves+f->ble.saves)
      <<",\"starts\":"<<f->radio.starts<<",\"stops\":"<<f->radio.stops<<",\"sends\":"<<f->radio.sends
      <<",\"timeout\":"<<f->service.config.displaySeconds()<<",\"flip\":"<<f->service.config.displayFlip()
      <<",\"carousel\":"<<f->service.config.displayCarouselSeconds()<<"}"<<std::endl;
    assert(!f->radio.sends);
  }
}
