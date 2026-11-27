// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Companion.h"
#include <iostream>
#include <string>

namespace usbRecovery {
static unsigned generation=1,usb=0,usbEpochChanged=0,usbDrops=0;
static uint32_t now=5,usbReadOnlyPrefix=0;
static uint32_t k_uptime_get_32(){return now;}
static unsigned irq_lock(){return 0;}
static void irq_unlock(unsigned){}
static unsigned atomic_set(unsigned *p,unsigned v){const auto previous=*p;*p=v;return previous;}
struct Line {bool measured,connected;unsigned generation;};
static Line observeUsbLineLocked(unsigned){return {true,true,generation};}
static void pollUsbTxLocked(unsigned){}
static Bytes usbRx;
static unsigned ring_buf_size_get(Bytes *q){return unsigned(q->size());}
static unsigned ring_buf_get_claim(Bytes *q,uint8_t **p,unsigned capacity){
    *p=q->data();return std::min(capacity,unsigned(q->size()));
}
static int ring_buf_get_finish(Bytes *q,unsigned n){q->erase(q->begin(),q->begin()+n);return 0;}
static void ring_buf_reset(Bytes *q){q->clear();}
static mt::Companion::WorkspaceProof companionWorkspaceProof(){return mt::Companion::WorkspaceProof{true};}
static Fixture *fixture;
static mt::Companion *client;
struct Host final:mt::Companion::Host {
    unsigned saves=0;
    std::vector<Bytes> records;
    bool ready()const override{return !fixture->runtime.radioHeld();}
    int save(const mt::Config &)override{++saves;return -EBUSY;}
    bool acceptTransportFrame(uint32_t frame)override{return frame==generation;}
    bool send(const uint8_t *p,size_t n)override{
        assert(n>4&&p[0]==0x94&&p[1]==0xc3&&size_t(p[2]*256+p[3])+4==n);
        records.emplace_back(p+4,p+n);return true;
    }
    bool adminKeys(meshtastic_Config_SecurityConfig &out)const override{out={};return true;}
    bool bluetooth()const override{return true;}
    Serial serialStatus()const override{return {true,115200,true};}
    bool localSnapshotReady()const{return true;}
} clientHost;
#define companion (*client)
#include "usb-rx-owner.inc"
#undef companion
static void drainHomeRadioEvents(){assert(false);}
static unsigned beaconSettingsEpoch(){return 1;}
static void pass(){
    auto &beaconTargets=fixture->runtime;
    auto &service=fixture->service;
    const bool initialized=true,storageReady=true;
    struct {bool trusted(){return true;}} extendedEdit;
    struct {bool open(){return false;}} settingsEdit;
    struct {bool pending(){return false;}} rebootSchedule;
    assert(fixture->clock.set(now));
    if(beaconTargets.radioHeld()){
#include "usb-held-owner.inc"
    }else{
        drainUsbCompanion(false);
        client->poll(companionWorkspaceProof());
    }
}
static Bytes decode(const std::string &wire){
    assert(wire.size()%2==0);
    Bytes bytes;
    for(size_t i=0;i<wire.size();i+=2)bytes.push_back(uint8_t(std::stoul(wire.substr(i,2),nullptr,16)));
    return bytes;
}
static int run(const std::string &fault){
    Fixture f;fixture=&f;
    const bool lost=fault=="lost";
    assert(lost||fault=="home");
    mt::companion::CompanionWorkspace workspace;
    mt::Companion usbClient(f.service,clientHost,127,workspace);client=&usbClient;
    std::string line;
    while(std::getline(std::cin,line)){
        clientHost.records.clear();
        if(line=="fault"){
            const auto target=f.resolve(Medium);draw=1;now+=10;
            assert(f.enqueue(301,target,now));f.poll(now);now+=f.delay(draw);f.poll(now);
            assert(f.radio.borrowed&&f.radio.frames.size()==1);
            if(lost){
                f.radio.joinFailures=UINT32_MAX;
                mt::RadioTiming::Deadline deadline{};
                assert(mt::RadioTiming::resolve(f.radio.airtime(f.radio.borrowedSize),deadline));
                now+=deadline.hostMs;pass();
            }else{
                f.radio.homeFailures=UINT32_MAX;
                f.radio.complete=true;++now;pass();
            }
            assert(f.runtime.recovering()&&f.runtime.radioHeld());
        }else if(line=="release"){
            f.radio.joinFailures=f.radio.homeFailures=0;
            for(unsigned i=0;i<6&&f.runtime.radioHeld();++i){now+=Runtime::RecoveryRetryMs;pass();}
            assert(!f.runtime.radioHeld()&&!f.runtime.recovering()&&f.service.receiving());
        }else if(line=="epoch"){
            wipeUsbRxLocked();++generation;usbEpochChanged=1;now+=5;pass();
        }else if(line=="poll"){
            now+=17;pass();
        }else if(line=="retry"){
            now+=Runtime::RecoveryRetryMs;pass();
        }else if(line.compare(0,6,"queue ")==0){
            const auto bytes=decode(line.substr(6));usbRx.insert(usbRx.end(),bytes.begin(),bytes.end());
        }else return 2;
        if(f.runtime.recovering()){
            assert(f.radio.frames.size()==1&&f.service.localStats().packetsTx==unsigned(!lost));
            assert(!lost||(f.radio.borrowed&&!memcmp(f.radio.borrowed,f.radio.frames[0].data(),f.radio.borrowedSize)));
        }
        std::cout<<"{\"recovering\":"<<f.runtime.recovering()<<",\"saves\":"<<clientHost.saves
                 <<",\"sends\":"<<f.radio.frames.size()<<",\"records\":[";
        for(size_t j=0;j<clientHost.records.size();++j){
            if(j)std::cout<<',';
            std::cout<<'"';
            for(uint8_t byte:clientHost.records[j])std::printf("%02x",byte);
            std::cout<<'"';
        }
        std::cout<<"]}"<<std::endl;
    }
    return 0;
}
}
