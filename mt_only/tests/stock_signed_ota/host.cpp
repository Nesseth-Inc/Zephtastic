// SPDX-License-Identifier: GPL-3.0-only
#include "SignedOta.h"
#include "../../src/Version.h"
#include "../../src/network/MqttMapReport.h"
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <openssl/sha.h>
#include <string>
#include <iostream>
#include <fstream>
#include <iterator>
#include <vector>
#include <cassert>
#include <cstring>
using B=std::vector<uint8_t>;
static B slots[2];static flash_area areas[2]={{0},{1}};
static bool confirmed=true,failWrite=false,failRead=false,corruptWrite=false;
static unsigned primaryReads;
static int swap=BOOT_SWAP_TYPE_NONE,requests=0,opens=0,writes=0;
int flash_area_open(int id,const flash_area **f){assert(id==0||id==1);*f=&areas[id];return 0;}
void flash_area_close(const flash_area*){}
int flash_area_read(const flash_area* f,size_t off,void *out,size_t n){
    if(f->id==0)++primaryReads;
    const auto &b=slots[f->id];if((failRead&&f->id==1)||off>b.size()||n>b.size()-off)return -1;
    memcpy(out,b.data()+off,n);return 0;
}
bool boot_is_img_confirmed(){return confirmed;}
int mcuboot_swap_type(){return swap;}
int boot_request_upgrade(int type){assert(type==BOOT_UPGRADE_TEST);++requests;swap=BOOT_SWAP_TYPE_TEST;return 0;}
int boot_write_img_confirmed(){confirmed=true;return 0;}
int flash_img_init_id(flash_img_context *c,int id){assert(id==1);++opens;c->offset=0;c->flash_area=&areas[id];slots[1].clear();return 0;}
int flash_img_buffered_write(flash_img_context *c,const uint8_t *p,size_t n,bool last){
    assert(c->flash_area->id==1);if(failWrite)return -1;
    slots[1].insert(slots[1].end(),p,p+n);++writes;
    if(last){if(corruptWrite)slots[1][160]^=1;c->flash_area=nullptr;}return 0;
}
void exerciseWire(const B&,const uint8_t[32],const std::string&);
using mt::update::signedOta::Session;
static B file(const char *p){std::ifstream f(p,std::ios::binary);return B(std::istreambuf_iterator<char>(f),{});}
static std::string command(const B &image,uint8_t (&hash)[32]){
    SHA256(image.data(),image.size(),hash);std::string cmd="OTA "+std::to_string(image.size())+" ";
    constexpr char hex[]="0123456789abcdef";for(auto c:hash){cmd+=hex[c>>4];cmd+=hex[c&15];}return cmd;
}
static bool transfer(const B &image){
    uint8_t hash[32];auto cmd=command(image,hash);const int before=requests;Session s(hash);
    assert(std::string(s.command(cmd.c_str()))=="ERASING\nOK\n");
    for(size_t pos=0;pos<image.size();){
        size_t n=std::min(size_t(733),image.size()-pos);std::string r=s.write(image.data()+pos,n);pos+=n;
        if(r.starts_with("ERR")){assert(before==requests);return false;}
        assert(r==(pos<image.size()?"ACK\n":"OK\n"));
    }
    assert(requests==before+1&&s.complete());swap=BOOT_SWAP_TYPE_NONE;return true;
}
int main(int argc,char **argv){
    assert(argc==4);slots[0]=file(argv[1]);auto active=slots[0],good=file(argv[2]),wrongKey=file(argv[3]);
    assert(good.size()>2048&&slots[0].size()>2048);
    uint8_t hash[32];auto cmd=command(good,hash);
    // Boot is the only reader for version reporting. A bad or oversized active
    // header falls back for telemetry and leaves the loader's VERSION unavailable.
    for(bool badMagic:{true,false}){
        slots[0]=active;
        if(badMagic)slots[0][0]=0;else memset(slots[0].data()+20,255,8);
        mt::update::signedOta::initializeBootVersion();
        assert(!strcmp(mt::firmwareVersion(),mt::FirmwareVersion));
        Session s(hash);
        assert(!strcmp(s.command("VERSION"),"ERR Image\n"));
    }
    slots[0]=active;
    mt::update::signedOta::initializeBootVersion();
    assert(primaryReads==3);
    mt::mqtt::MqttMapReport::Source source{};
    source.longName="Fixture";source.shortName="FX";source.firmwareVersion=mt::firmwareVersion();
    meshtastic_MapReport map{};
    mt::stock::MQTT::fillMapReport(map,source,14);
    assert(!strcmp(map.firmware_version,"2.8.1+100"));
    {Session s(hash);
     for(unsigned i=0;i<8;++i){
         assert(!strcmp(mt::firmwareVersion(),"2.8.1+100"));
         assert(std::string(s.command("VERSION")).starts_with("OK 81 2.8.1+100 "));
     }
     assert(primaryReads==3);
     for(const auto *bad:{"REBOOT","OTA 0 ","OTA 18446744073709551616 abc","OTA -1 abc","OTA 12 abc"})
         assert(std::string(s.command(bad)).starts_with("ERR"));
     auto wrong=cmd;wrong.back()=wrong.back()=='1'?'0':'1';assert(std::string(s.command(wrong.c_str())).starts_with("ERR"));
     assert(opens==0&&writes==0);confirmed=false;assert(std::string(s.command(cmd.c_str())).starts_with("ERR"));
     confirmed=true;swap=BOOT_SWAP_TYPE_TEST;assert(std::string(s.command(cmd.c_str())).starts_with("ERR"));swap=BOOT_SWAP_TYPE_NONE;
     assert(std::string(s.command(cmd.c_str())).starts_with("ERASING"));assert(s.write(good.data(),120)==std::string("ACK\n"));}
    assert(requests==0&&slots[0]==active); // interrupted transfer cannot select the slot
    assert(transfer(good));assert(!transfer(wrongKey));
    auto bad=good;bad.back()^=1;assert(!transfer(bad));bad=good;bad[200]^=1;assert(!transfer(bad));
    bad=good;bad[24]=99;assert(!transfer(bad));bad=good;bad[24]=100;assert(!transfer(bad));
    bad=good;bad.push_back(0xff);assert(!transfer(bad));bad=good;bad[8]=0xff;assert(!transfer(bad));
    failRead=true;assert(!transfer(good));failRead=false;failWrite=true;assert(!transfer(good));failWrite=false;
    corruptWrite=true;assert(!transfer(good));corruptWrite=false;assert(transfer(good));
    assert(slots[0]==active&&requests==2);exerciseWire(good,hash,cmd);assert(slots[0]==active&&requests==3);std::cout<<"stock command, segmented transfer, signed version, actual readback, interrupted retry and selector checks passed\n";
}
