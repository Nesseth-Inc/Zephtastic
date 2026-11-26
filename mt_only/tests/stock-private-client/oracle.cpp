// SPDX-License-Identifier: GPL-3.0-only
// Host-only adapter: actual Companion parser and output, inert radio, no keys.
#include "Companion.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
static constexpr uint32_t Local=0x12345678;
struct Radio final:mt::Radio {
  unsigned sends=0,starts=0;
  bool ready() override{return true;}
  int start() override{++starts;return 0;}
  int stop() override{return 0;}
  int restore() override{return 0;}
  int health() override{return 1;}
  bool busy() override{return false;}
  uint32_t airtime(size_t) override{return 100;}
  int send(uint8_t *,size_t) override{++sends;return -EIO;}
  bool completion(int &) override{return false;}
};
static std::string hex(const uint8_t *p,size_t n){
  static const char digits[]="0123456789abcdef";std::string result;result.reserve(n*2);
  for(size_t i=0;i<n;++i){result+=digits[p[i]>>4];result+=digits[p[i]&15];}return result;
}
struct Host final:mt::Companion::Host {
  std::vector<std::string> output;
  unsigned saves=0;
  int save(const mt::Config &) override{++saves;return -ENOTSUP;}
  bool ready()const override{return true;}
  bool send(const uint8_t *p,size_t n) override {
    assert(n>=4&&p[0]==0x94&&p[1]==0xc3&&size_t(p[2]*256+p[3])==n-4);
    output.push_back(hex(p+4,n-4));return true;
  }
};
struct Fixture {
  Radio radio;mt::Service service;Host host;mt::companion::CompanionWorkspace workspace;mt::Companion companion;
  explicit Fixture(bool ble):service(radio,false,Local,81,true),companion(service,host,81,workspace,ble?1:0){
    mt::Config config;config.bytes[4]=1;auto node=config.node();node.setPublic();config.node(node);assert(service.apply(config));
  }
};
int main(int argc,char **argv){
  assert(argc==2);const bool ble=std::string(argv[1])=="ble";assert(ble||std::string(argv[1])=="usb");
  std::string line;unsigned count=0;
  while(std::getline(std::cin,line)){
    assert(++count<=128&&line.size()>0&&line.size()%2==0&&line.size()<=1024);
    std::vector<uint8_t> bytes;
    for(size_t i=0;i<line.size();i+=2){unsigned v=0;for(unsigned j=0;j<2;++j){const char c=line[i+j];assert((c>='0'&&c<='9')||(c>='a'&&c<='f'));v=v*16+unsigned(c<='9'?c-'0':c-'a'+10);}bytes.push_back(uint8_t(v));}
    auto f=std::make_unique<Fixture>(ble);
    if(ble)f->companion.packet(bytes.data(),bytes.size(),100,mt::Companion::WorkspaceProof{true});
    else{for(uint8_t b:{uint8_t(0x94),uint8_t(0xc3),uint8_t(bytes.size()>>8),uint8_t(bytes.size())})f->companion.byte(b,100,0,mt::Companion::WorkspaceProof{true});for(uint8_t b:bytes)f->companion.byte(b,100,0,mt::Companion::WorkspaceProof{true});}
    std::cout<<"{\"records_hex\":[";bool first=true;for(const auto &s:f->host.output){if(!first)std::cout<<',';first=false;std::cout<<'\"'<<s<<'\"';}
    std::cout<<"],\"radio_sends\":"<<f->radio.sends<<",\"radio_starts\":"<<f->radio.starts<<",\"settings_saves\":"<<f->host.saves<<",\"parse_errors\":"<<f->companion.parseErrors<<"}\n";
    assert(f->radio.sends==0&&f->radio.starts==0);
  }
}
