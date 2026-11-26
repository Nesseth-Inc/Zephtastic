// SPDX-License-Identifier: GPL-3.0-only
// Adapter for actual stock favorite APIs: raw memory transport bytes, no device.
#include "node_mute_fixture.h"
#include <iostream>
#include <sstream>
static std::string hex(const uint8_t *p,size_t n){static const char d[]="0123456789abcdef";std::string out;for(size_t i=0;i<n;++i){out+=d[p[i]>>4];out+=d[p[i]&15];}return out;}
int main(int argc,char **argv){assert(argc==2);const unsigned model=unsigned(std::stoul(argv[1]));assert(model==81||model==127);
  MuteFixture f{uint8_t(model)};f.seed();std::string line;unsigned count=0;
  while(std::getline(std::cin,line)){
    assert(++count<=128&&line.size()<=1100);std::istringstream input(line);std::string mode,raw;input>>mode>>raw;
    const bool ble=mode=="ble";assert(ble||mode=="usb");assert(!raw.empty()&&raw.size()%2==0&&raw.size()<=1024);
    std::vector<uint8_t> bytes;for(size_t i=0;i<raw.size();i+=2){unsigned value=0;for(unsigned j=0;j<2;++j){const char c=raw[i+j];assert((c>='0'&&c<='9')||(c>='a'&&c<='f'));value=value*16+unsigned(c<='9'?c-'0':c-'a'+10);}bytes.push_back(uint8_t(value));}
    f.usb.records.clear();f.ble.records.clear();
    if(ble)f.wireless.packet(bytes.data(),bytes.size(),1000*count,mt::Companion::WorkspaceProof{true});
    else for(uint8_t byte:MuteFixture::framed(bytes))f.companion.byte(byte,1000*count,0,mt::Companion::WorkspaceProof{true});
    const auto &records=(ble?f.ble:f.usb).records;
    std::cout<<"{\"records_hex\":[";bool first=true;for(const auto &record:records){if(!first)std::cout<<',';first=false;std::cout<<'"'<<hex(record.data(),record.size())<<'"';}
    std::cout<<"],\"writes\":"<<f.backend.writes<<",\"sends\":"<<f.radio.sends<<",\"starts\":"<<f.radio.starts
      <<",\"generation\":"<<f.journal.generation()<<",\"slot\":"<<f.journal.activeSlot()<<",\"count\":"<<f.service.nodes.count()
      <<",\"dirty\":"<<(f.service.nodes.dirty()?"true":"false")<<",\"favorite\":"<<(f.service.nodes.get(0)->favorite()?"true":"false")
      <<",\"missing_present\":"<<(f.service.nodes.find(0x66667777)>=0?"true":"false")<<",\"tx_armed\":"<<(f.service.endpointArmed()?"true":"false")
      <<",\"config_hex\":\""<<hex(f.service.config.bytes,sizeof(f.service.config.bytes))<<"\",\"durable\":{";
    first=true;for(const auto &entry:f.backend.entries){if(!first)std::cout<<',';first=false;std::cout<<'"'<<entry.first<<"\":\""<<hex(entry.second.data(),entry.second.size())<<'"';}
    std::cout<<"}}"<<std::endl;assert(!f.radio.sends&&!f.radio.starts&&!f.service.endpointArmed());
  }
}
