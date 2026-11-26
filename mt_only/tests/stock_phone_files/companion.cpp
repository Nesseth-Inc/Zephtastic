// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include "stock/files/PhoneFiles.h"
#include <algorithm>
#include <string>

using namespace mt::stock::files;
struct MemoryFile{
    std::string saved="record\n",staged;
    int info(uint32_t &n){n=saved.size();return 0;}
    int open(bool){staged.clear();return 0;}
    int write(const uint8_t *p,size_t n){staged.append((const char*)p,n);return 0;}
    int read(uint32_t offset,uint8_t *p,size_t n){
        n=std::min(saved.size()-offset,n);memcpy(p,saved.data()+offset,n);return int(n);
    }
    int close(bool commit){if(commit)saved=staged;return 0;}
};
struct FileHost:Host{
    MemoryFile file;
    PhoneFiles<MemoryFile> files{file};
    bool console=true,secure=true,blocked=false;
    bool many=false;unsigned listed=0;
    using Host::Host;
    bool consoleAllowed()const override{return console;}
    Security bluetoothSecurity()const override{return {true,false,0,uint8_t(secure?2:1),0};}
    bool send(const uint8_t *p,size_t n)override{return !blocked&&Host::send(p,n);}
    int appFileInfo(uint8_t o,meshtastic_FileInfo &f)override{
        if(many){if(listed==3)return 0;snprintf(f.file_name,sizeof(f.file_name),"/static/file-%u",listed);f.size_bytes=100+listed;return 2;}
        uint32_t n=0;int result=files.manifest(o,n);if(result>0){strcpy(f.file_name,PublicName);f.size_bytes=n;}return result;}
    void appFileInfoSent(uint8_t o)override{if(many){if(listed<3)++listed;}else files.manifestSent(o);}
    bool appFileSubmit(uint8_t o,const Packet &p,uint32_t now)override{return files.submit(o,p,now);}
    bool appFileResponse(uint8_t o,Packet &p)override{return files.response(o,p);}
    void appFileSent(uint8_t o)override{files.sent(o);}
    void appFileDisconnect(uint8_t o)override{listed=0;files.disconnect(o);}
};
void companionCases(){
    auto f=std::make_unique<Fixture>();
    FileHost h(f->s);
    mt::companion::CompanionWorkspace workspace;
    mt::Companion usb(f->s,h,127,workspace,0),ble(f->s,h,127,workspace,1),tcp(f->s,h,81,workspace,2);
    h.many=true;meshtastic_ToRadio start{};start.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;start.want_config_id=321;
    input(usb,start,1);h.blocked=true;usb.poll(mt::Companion::WorkspaceProof{true});h.blocked=false;
    for(unsigned i=0;i<8;++i)usb.poll(mt::Companion::WorkspaceProof{true});
    unsigned count=0;bool complete=false;
    for(const auto &msg:h.out){
        if(msg.which_payload_variant==meshtastic_FromRadio_fileInfo_tag){assert(!complete&&msg.fileInfo.size_bytes==100+count);++count;}
        if(msg.which_payload_variant==meshtastic_FromRadio_config_complete_id_tag)complete=true;
    }
    assert(complete&&count==3);h.many=false;
    auto handshake=[&](mt::Companion &client,unsigned nonce,bool expects){
        h.out.clear();meshtastic_ToRadio t{};t.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;t.want_config_id=nonce;
        input(client,t,1);client.poll(mt::Companion::WorkspaceProof{true});h.files.poll(2);client.poll(mt::Companion::WorkspaceProof{true});assert(!h.out.empty());
        const bool seen=std::any_of(h.out.begin(),h.out.end(),[&](const auto &o){return o.which_payload_variant==meshtastic_FromRadio_fileInfo_tag
            &&!strcmp(o.fileInfo.file_name,PublicName)&&o.fileInfo.size_bytes==h.file.saved.size();});
        assert(seen==expects&&h.out.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag);
    };
    handshake(usb,123,true);handshake(usb,69421,false);handshake(usb,1,true);handshake(tcp,2,false);
    h.secure=false;handshake(ble,2,false);h.secure=true;handshake(ble,2,true);
    meshtastic_ToRadio t{};t.which_payload_variant=meshtastic_ToRadio_xmodemPacket_tag;
    auto &p=t.xmodemPacket;p.control=meshtastic_XModem_Control_STX;p.buffer.size=sizeof(PublicName)-1;memcpy(p.buffer.bytes,PublicName,p.buffer.size);
    h.out.clear();input(tcp,t,20);h.files.poll(21);tcp.poll(mt::Companion::WorkspaceProof{true});assert(h.out.empty());
    input(usb,t,30);h.files.poll(31);h.blocked=true;usb.poll(mt::Companion::WorkspaceProof{true});h.blocked=false;ble.poll(mt::Companion::WorkspaceProof{true});assert(h.out.empty());
    usb.poll(mt::Companion::WorkspaceProof{true});assert(h.out.size()==1&&h.out.back().xmodemPacket.control==SOH&&h.out.back().xmodemPacket.seq==1);
    p={};p.control=meshtastic_XModem_Control_ACK;input(ble,t,33);
    assert(h.out.back().xmodemPacket.control==NAK);
    input(usb,t,34);h.files.poll(35);usb.poll(mt::Companion::WorkspaceProof{true});assert(h.out.back().xmodemPacket.control==EOT);
    p={};p.control=meshtastic_XModem_Control_SOH;p.buffer.size=sizeof(PublicName);memcpy(p.buffer.bytes,PublicName,sizeof(PublicName));
    input(ble,t,36);h.files.poll(37);ble.poll(mt::Companion::WorkspaceProof{true});assert(h.out.back().xmodemPacket.control==ACK);
    h.secure=false;ble.poll(mt::Companion::WorkspaceProof{true});h.files.poll(40);h.secure=true;
    assert(h.file.saved=="record\n");
    h.out.clear();h.console=false;input(usb,t,42);h.files.poll(43);usb.poll(mt::Companion::WorkspaceProof{true});assert(h.out.empty());
    auto cfg=f->s.config;assert(cfg.setSecurityControls(true,true));f->s.stop();assert(f->s.apply(cfg));
    h.console=true;handshake(usb,4,false);
    input(usb,t,50);h.files.poll(51);h.out.clear();usb.poll(mt::Companion::WorkspaceProof{true});assert(h.out.empty());
    puts("PASS real FromRadio file manifest and XModem envelope, backpressure, authorized USB/BLE, node-only handshake, managed/closed console/weak BLE/TCP isolation");
}
