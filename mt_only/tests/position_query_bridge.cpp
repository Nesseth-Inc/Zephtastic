// SPDX-License-Identifier: GPL-3.0-only
// Memory transport plus injected Python stock-encoded peer Data; no responder claim.
#include "position_query_fixture.h"
static std::vector<uint8_t> unhex(const char *p){std::string s=p;assert(s.size()%2==0);std::vector<uint8_t> b;for(size_t i=0;i<s.size();i+=2)b.push_back(uint8_t(std::stoul(s.substr(i,2),nullptr,16)));return b;}
static std::string hex(const uint8_t *p,size_t n){constexpr char h[]="0123456789abcdef";std::string s;for(size_t i=0;i<n;++i){s+=h[p[i]>>4];s+=h[p[i]&15];}return s;}
static void records(const Host &h,const char *name){for(const auto &o:h.out){uint8_t b[512]{};auto out=pb_ostream_from_buffer(b,sizeof(b));assert(pb_encode(&out,meshtastic_FromRadio_fields,&o));std::cout<<name<<" "<<hex(b,out.bytes_written)<<"\n";}}
int main(int argc,char **argv){
  (void)&group;(void)&results;(void)&positions;(void)&beginQuery;(void)&goodFrame;
  if(argc!=6&&argc!=8)return 64;
  const auto request=unhex(argv[1]),data=unhex(argv[2]);const int model=std::stoi(argv[3]);
  const bool ble=std::string(argv[4])=="ble";const std::string mode=argv[5];Fixture f(true,model);
  mt::Companion &owner=ble?f.bc:f.c;
  auto feed=[&](const std::vector<uint8_t> &v,uint32_t now){
    if(ble)owner.packet(v.data(),v.size(),now,mt::Companion::WorkspaceProof{true});
    else {owner.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});owner.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});owner.byte(uint8_t(v.size()>>8),now,0,mt::Companion::WorkspaceProof{true});owner.byte(uint8_t(v.size()),now,0,mt::Companion::WorkspaceProof{true});for(auto b:v)owner.byte(b,now,0,mt::Companion::WorkspaceProof{true});}
  };
  feed(request,100);
  f.s.poll(300,0);if(f.radio.sends!=1)return 65;f.done(301);
  const auto &sent=f.radio.frames[0];const auto id=bench::le32(sent.data()+8);
  std::cout<<"RF "<<hex(sent.data(),sent.size())<<"\n";
  if(argc==8){
    assert(mode=="two-pending");const auto second=unhex(argv[6]),secondData=unhex(argv[7]);
    feed(second,600);f.s.poll(800,0);assert(f.radio.sends==2);f.done(801);
    const auto &sent2=f.radio.frames[1];assert(bench::le32(sent2.data()+8)!=id);
    std::cout<<"RF "<<hex(sent2.data(),sent2.size())<<"\n";
    f.rx(rawFrame(secondData,PEER,OURS,701),1000);f.clients();
    f.rx(rawFrame(data,PEER,OURS,700),1100);f.clients();
    f.rx(rawFrame(secondData,PEER,OURS,701),1200);f.clients();
    f.rx(rawFrame(data,PEER,OURS,700),1300);f.clients();
    records(f.h,"USB");records(f.bh,"BLE");return 0;
  }
  if(mode=="ack-first") {f.rx(f.wire(PEER,OURS,698,id,0),400);f.clients();assert(!results(ble?f.bh:f.h,id));}
  if(mode=="malformed") {auto bad=data;bad.insert(bad.end(),{8,3});f.rx(rawFrame(bad),400);f.clients();assert(!positions(ble?f.bh:f.h,id));}
  f.rx(rawFrame(data),500);f.clients();f.rx(rawFrame(data),600);f.clients();
  records(f.h,"USB");records(f.bh,"BLE");return 0;
}
