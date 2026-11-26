// SPDX-License-Identifier: GPL-3.0-only
#include "stock/message/TextMessageModule.h"
#include "stock/verification/KeyVerificationModule.h"
#include "pki/verification/KeyVerification.h"
#include <openssl/sha.h>
#include <pb_encode.h>
#include <cassert>
#include <cstdio>
#include <vector>

namespace donor {
struct {
    meshtastic_ModuleConfig_ExternalNotificationConfig external_notification{};
    meshtastic_ModuleConfig_RangeTestConfig range_test{};
} moduleConfig;
struct { meshtastic_Config_DeviceConfig device{}; meshtastic_Config_DisplayConfig display{}; } config;
struct Power { int charge; int getBatteryChargePercent(){ return charge; } } power;
Power *powerStatus;
struct MeshService {
#include "want.inc"
    static bool isAlertPayload(const meshtastic_MeshPacket &);
};
constexpr unsigned kAsciiBell=7, EVENT_RECEIVED_MSG=1, TEXT_PACKET_LIST_SIZE=50;
bool muted=false,stored=true,woke=false,observed=false;
bool isMutedForPacket(const meshtastic_MeshPacket &){return muted;}
struct {void trigger(unsigned){woke=true;}} powerFSM;
struct StoredMessage {};
struct {StoredMessage entry; StoredMessage *tryAddFromPacket(const meshtastic_MeshPacket &){return stored?&entry:nullptr;}} messageStore;
struct Screen {void *getDisplayDevice(){return nullptr;}} *screen=nullptr;
namespace graphics { struct MessageRenderer {static void handleNewMessage(void*,const StoredMessage&,const meshtastic_MeshPacket&){} }; }
#define IF_SCREEN(...) __VA_ARGS__
#define IS_ONE_OF(x,a,b,c,d) ((x)==(a)||(x)==(b)||(x)==(c)||(x)==(d))
enum class ProcessMessage { CONTINUE };
struct TextMessageModule {
    uint32_t textPacketList[TEXT_PACKET_LIST_SIZE]{}; size_t textPacketListIndex=0;
    ProcessMessage handleReceived(const meshtastic_MeshPacket&);
    bool wantPacket(const meshtastic_MeshPacket*);
    void notifyObservers(const meshtastic_MeshPacket*){observed=true;}
};
struct Hash {
    std::vector<uint8_t> bytes;
    void reset(){bytes.clear();}
    void update(const void *p,size_t size){auto b=static_cast<const uint8_t*>(p);bytes.insert(bytes.end(),b,b+size);}
    void finalize(uint8_t *out,size_t n){assert(n==32);::SHA256(bytes.data(),bytes.size(),out);}
};
#define LOG_WARN(...) ((void)0)
struct KeyVerificationModule {
    uint32_t ourNodeNum=33,currentRemoteNode=44,currentSecurityNumber=0;
    uint64_t currentNonce=0;
    struct { decltype(meshtastic_User::public_key) public_key{}; } owner;
    decltype(meshtastic_User::public_key) remotePublic{};
    uint8_t hash1[32]{},hash2[32]{};
    meshtastic_KeyVerification capture{};
    void generateVerificationCode(char *);
    void process(uint32_t incomingNumber) {
        Hash hash; uint8_t scratch_hash[32]{};
#include "proof.inc"
};
#include "oracle.inc"
}

static void textCases(){
    unsigned cases=0;
    for(unsigned role: {0u,1u,2u,3u,8u,11u,12u})
    for(int charge: {-1,0,9,10,100})
    for(unsigned port: {1u,11u,66u,73u,4u})
    for(unsigned flags=0;flags<64;++flags){
        mt::stock::text::Settings s{};
        s.role=role;s.batteryPercent=charge;s.rangeEnabled=flags&1;s.externalNotifications=flags&2;s.bellOutput=flags&4;
        bool muted=flags&8,bell=flags&16,accept=flags&32;
        donor::config.device.role=static_cast<meshtastic_Config_DeviceConfig_Role>(role);
        donor::powerStatus=charge<0?nullptr:&donor::power;donor::power.charge=charge;
        donor::moduleConfig.range_test.enabled=s.rangeEnabled;
        donor::moduleConfig.external_notification.enabled=s.externalNotifications;
        donor::moduleConfig.external_notification.alert_bell=s.bellOutput;
        donor::muted=muted;donor::stored=accept;donor::woke=donor::observed=false;
        donor::TextMessageModule donor;meshtastic_MeshPacket m{};m.id=3;
        m.decoded.portnum=static_cast<meshtastic_PortNum>(port);m.decoded.payload.size=2;
        m.decoded.payload.bytes[0]=0;m.decoded.payload.bytes[1]=bell?7:'a';
        bool wants=donor.wantPacket(&m);if(wants)donor.handleReceived(m);
        assert(wants==mt::stock::text::wantPacket(port,s.rangeEnabled));
        bool got=false,storeCalled=false;
        mt::stock::text::handleReceived(port,m.decoded.payload.bytes,2,muted,false,s,
            [&]{storeCalled=true;return accept;},[&]{got=true;});
        assert(storeCalled==wants&&got==donor::woke);++cases;
    }
    printf("PASS %u pinned text classification, alert and wake combinations\n",cases);
}

using Session=mt::pki::verification::KeyVerification;
struct Peer : Session::Port {
    uint32_t node,remote; uint8_t key[32],other[32];
    meshtastic_KeyVerification sent{}; bool encrypted=false,pinned=true,staged=false;
    meshtastic_ClientNotification event{};unsigned commits=0;
    explicit Peer(uint32_t n,uint32_t r):node(n),remote(r){
        for(unsigned i=0;i<32;++i){key[i]=uint8_t(i+n);other[i]=uint8_t(i+r);}
    }
    bool localIdentity(uint32_t &id,uint8_t out[32])const override{id=node;memcpy(out,key,32);return true;}
    bool peerPublicKey(uint32_t id,uint8_t out[32])const override{if(id!=remote||!pinned)return false;memcpy(out,other,32);return true;}
    int stagePendingCandidate(uint32_t id,const uint8_t *candidate)override{
        if(id!=remote||pinned||memcmp(candidate,other,32))return -EINVAL;
        staged=true;return 0;
    }
    void clearPendingCandidate(uint32_t id,const uint8_t *candidate)override{
        assert(id==remote&&!memcmp(candidate,other,32));staged=false;
    }
    bool random(uint8_t *out,size_t n) override{for(size_t i=0;i<n;++i)out[i]=uint8_t(3+i);return true;}
    bool sha256(const uint8_t *p,size_t n,uint8_t out[32]) override{return ::SHA256(p,n,out)!=nullptr;}
    int send(uint32_t id,uint32_t,const meshtastic_KeyVerification &m,bool pki,const uint8_t *k,size_t n,bool,uint32_t) override{
        assert(id==remote&&(!pki||(k&&n==32&&!memcmp(other,k,32)))); sent=m;encrypted=pki;return 0;
    }
    int commitVerifiedPeer(uint32_t id,const uint8_t k[32]) override{assert(id==remote&&!memcmp(other,k,32));assert(pinned||staged);pinned=true;++commits;return 0;}
    void notify(const meshtastic_ClientNotification &e) override{event=e;}
    int deliver(Session &receiver,unsigned id) {
        uint8_t bytes[128];auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
        assert(pb_encode(&stream,meshtastic_KeyVerification_fields,&sent));
        return receiver.receive(node,id,encrypted,encrypted?key:nullptr,encrypted?32:0,bytes,stream.bytes_written,id);
    }
};

static void handshake(bool existing){
    Peer a(33,44),b(44,33);Session sender(a),receiver(b);a.pinned=b.pinned=existing;meshtastic_KeyVerificationAdmin cmd{};
    cmd.message_type=meshtastic_KeyVerificationAdmin_MessageType_INITIATE_VERIFICATION;
    cmd.remote_nodenum=b.node;
    assert(sender.handleAdmin(cmd,1)==0&&a.deliver(receiver,2)==0);
    assert(a.encrypted==existing&&b.encrypted==existing);
    meshtastic_KeyVerificationAdmin early{};early.message_type=meshtastic_KeyVerificationAdmin_MessageType_DO_VERIFY;
    early.nonce=a.sent.nonce;assert(sender.handleAdmin(early,2)<0&&receiver.handleAdmin(early,2)<0);
    assert(!a.commits&&!b.commits);
    auto number=b.event.payload_variant.key_verification_number_inform.security_number;
    donor::KeyVerificationModule stock;stock.currentNonce=b.sent.nonce;
    stock.owner.public_key.size=32;memcpy(stock.owner.public_key.bytes,a.key,32);memcpy(stock.remotePublic.bytes,b.key,32);
    memcpy(stock.hash2,b.sent.hash2.bytes,32);
    stock.process(number%999999+1);assert(stock.capture.hash1.size==0);
    stock.process(number);assert(stock.capture.hash1.size==32);
    assert(b.deliver(sender,3)==0);
    cmd={};cmd.message_type=meshtastic_KeyVerificationAdmin_MessageType_PROVIDE_SECURITY_NUMBER;
    cmd.nonce=stock.currentNonce;cmd.has_security_number=true;cmd.security_number=number%999999+1;
    assert(sender.handleAdmin(cmd,4)<0);
    cmd.security_number=number;assert(sender.handleAdmin(cmd,5)==0&&a.encrypted);
    assert(a.sent.nonce==stock.capture.nonce&&a.sent.hash1.size==32&&a.sent.hash2.size==0&&!memcmp(a.sent.hash1.bytes,stock.capture.hash1.bytes,32));
    assert(a.deliver(receiver,6)==0);
    char code[10]{};stock.generateVerificationCode(code);
    assert(!strcmp(code,a.event.payload_variant.key_verification_final.verification_characters)&&
           !strcmp(code,b.event.payload_variant.key_verification_final.verification_characters));
    assert(a.commits==0&&b.commits==0);
    cmd={};cmd.message_type=meshtastic_KeyVerificationAdmin_MessageType_DO_VERIFY;cmd.nonce=stock.currentNonce;
    assert(sender.handleAdmin(cmd,7)==0&&receiver.handleAdmin(cmd,7)==0&&a.commits==1&&b.commits==1);
    puts("PASS donor SHA-256 transcript, solicited proof, on-screen code and both local approvals");
}
int main(){textCases();handshake(true);handshake(false);}
