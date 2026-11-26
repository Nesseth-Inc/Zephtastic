// SPDX-License-Identifier: GPL-3.0-only
// Line adapter for actual stock Python toggle-muted-node vectors; memory only.
#include "node_mute_fixture.h"
#include <iostream>
#include <sstream>
static std::string hex(const uint8_t *p,size_t n){static const char d[]="0123456789abcdef";std::string out;for(size_t i=0;i<n;++i){out+=d[p[i]>>4];out+=d[p[i]&15];}return out;}
int main(int argc,char **argv){assert(argc==2);const unsigned model=unsigned(std::stoul(argv[1]));assert(model==81||model==127);
  MuteFixture fixture{uint8_t(model)};fixture.seed();std::string line;unsigned count=0;
  while(std::getline(std::cin,line)){
    assert(++count<=128&&line.size()<=1100);std::istringstream input(line);std::string mode,raw;input>>mode>>raw;
    const bool ble=mode=="ble";assert(ble||mode=="usb");assert(!raw.empty()&&raw.size()%2==0&&raw.size()<=1024);
    std::vector<uint8_t> bytes;for(size_t i=0;i<raw.size();i+=2){unsigned value=0;for(unsigned j=0;j<2;++j){const char c=raw[i+j];assert((c>='0'&&c<='9')||(c>='a'&&c<='f'));value=value*16+unsigned(c<='9'?c-'0':c-'a'+10);}bytes.push_back(uint8_t(value));}
    fixture.usb.records.clear();fixture.ble.records.clear();fixture.input([&]{meshtastic_ToRadio value{};auto stream=pb_istream_from_buffer(bytes.data(),bytes.size());assert(pb_decode(&stream,meshtastic_ToRadio_fields,&value));return value;}(),ble,1000*count);
    const auto &records=(ble?fixture.ble:fixture.usb).records;
    std::cout<<"{\"records_hex\":[";bool first=true;for(const auto &record:records){if(!first)std::cout<<',';first=false;std::cout<<'"'<<hex(record.data(),record.size())<<'"';}
    std::cout<<"],\"muted\":"<<(fixture.service.nodes.muted(MutePeer)?"true":"false")<<",\"writes\":"<<fixture.backend.writes
      <<",\"sends\":"<<fixture.radio.sends<<",\"model\":"<<model<<"}"<<std::endl;
    assert(!fixture.radio.sends);
  }
}
