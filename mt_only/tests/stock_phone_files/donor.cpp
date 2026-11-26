// SPDX-License-Identifier: GPL-3.0-only
#include "stock/files/PhoneFiles.h"
#include <map>
#include <string>
#include <vector>
#include <cassert>
#include <algorithm>
#include <cstring>
using namespace mt::stock::files;
static std::map<std::string,std::string> volume;
struct File {
    std::string *body=nullptr;size_t pos=0;
    explicit operator bool()const{return body!=nullptr;}
    size_t read(uint8_t *p,size_t n){n=std::min(n,body->size()-pos);memcpy(p,body->data()+pos,n);pos+=n;return n;}
    size_t write(const uint8_t *p,size_t n){body->append((const char*)p,n);pos+=n;return n;}
    void flush(){} void close(){} void seek(size_t at){pos=at;}
};
struct FS {
    bool exists(const char *p){return volume.count(p);}
    void remove(const char *p){volume.erase(p);}
    File open(const char *p,bool upload){return upload||exists(p)?File{&volume[p],0}:File{};}
} FSCom;
struct Lock {void lock(){}void unlock(){}} lock;
#define spiLock (&lock)
#define FILE_O_READ false
#define FILE_O_WRITE true
#define MAXRETRANS 25
#define LOG_DEBUG(...)
#define LOG_WARN(...)
#define LOG_INFO(...)
struct XModemAdapter {
    bool isReceiving=false,isTransmitting=false,isEOT=false;int retrans=MAXRETRANS;
    uint16_t packetno=0;File file;char filename[128]{};Packet xmodemStore{};
    struct {void notifyObservers(uint16_t){}} packetReady;
    bool isValidFilename(const char*);unsigned short crc16_ccitt(const pb_byte_t*,int);
    int check(const pb_byte_t*,int,unsigned short);void sendControl(meshtastic_XModem_Control);
    void handlePacket(Packet);
};
#include "stock.inc"
struct FileOwner {
    std::string saved,stage;bool reading=false,failWrite=false,failCommit=false,closed=false;
    int info(uint32_t &n){n=saved.size();return 0;}
    int open(bool upload){reading=!upload;closed=false;stage.clear();return 0;}
    int read(uint32_t at,uint8_t *out,size_t n){assert(reading);n=std::min(n,saved.size()-at);memcpy(out,saved.data()+at,n);return int(n);}
    int write(const uint8_t *p,size_t n){if(failWrite)return -1;stage.append((const char*)p,n);return 0;}
    int close(bool commit){closed=true;if(commit&&failCommit)return -1;if(commit)saved=stage;return 0;}
};
static Packet make(Control c,unsigned seq=0,const std::string &s={}){
    Packet p{};control(p,c);p.seq=seq;p.buffer.size=s.size();memcpy(p.buffer.bytes,s.data(),s.size());p.crc16=crc(p.buffer.bytes,p.buffer.size);return p;
}
void satelliteCases();
void companionCases();
int main(){
    for(unsigned n:{0u,1u,127u,128u,129u,256u,417u}){
        std::string bytes;for(unsigned i=0;i<n;++i)bytes.push_back(char(i*91));
        FileOwner f;XModem port;XModemAdapter stock;Packet actual{};
        volume.clear();volume[PublicName]=f.saved="old";
        auto compare=[&](Packet p){
            stock.handlePacket(p);port.handle(f,p,actual);const auto &expected=stock.xmodemStore;
            assert(expected.control==actual.control&&expected.seq==actual.seq&&expected.crc16==actual.crc16&&expected.buffer.size==actual.buffer.size);
            assert(!memcmp(expected.buffer.bytes,actual.buffer.bytes,actual.buffer.size));
        };
        compare(make(SOH,0,PublicName));unsigned seq=1;
        for(unsigned i=0;i<n;i+=128){
            auto block=make(SOH,seq++,bytes.substr(i,128));auto damaged=block;damaged.crc16^=31;compare(damaged);compare(block);
        }
        compare(make(EOT));assert(f.saved==bytes&&volume[PublicName]==bytes);
        compare(make(STX,0,PublicName));
        while(actual.control==SOH){compare(make(NAK));compare(make(ACK));}
        assert(actual.control==EOT&&!port.active());
    }
    FileOwner f;XModem p;Packet out{};f.saved="preserved";
    auto accepted=[&](Control c,unsigned seq,const std::string &body,Control expected){p.handle(f,make(c,seq,body),out);assert(out.control==expected);};
    accepted(SOH,0,"/prefs/db",NAK);accepted(STX,0,std::string(PublicName)+std::string("\0x",2),NAK);
    accepted(SOH,0,PublicName,ACK);f.failWrite=true;accepted(SOH,1,"record",CAN);assert(f.saved=="preserved");
    f.failWrite=false;accepted(SOH,0,PublicName,ACK);accepted(SOH,1,"record",ACK);f.failCommit=true;
    accepted(EOT,0,"",CAN);assert(f.saved=="preserved");
    accepted(STX,0,PublicName,SOH);for(unsigned i=0;i<24;++i)accepted(NAK,0,"",SOH);accepted(NAK,0,"",CAN);
    PhoneFiles<FileOwner> queue(f);assert(queue.submit(1,make(SOH,0,PublicName),UINT32_MAX-5));queue.poll(UINT32_MAX-4);
    assert(queue.response(1,out)&&out.control==ACK);queue.sent(1);queue.poll(30000);assert(f.closed&&f.saved=="preserved");
    satelliteCases();companionCases();
    puts("PASS pinned XModem CRC, control/sequence/bytes, 7 download/upload sizes, bounded resend, atomic failure/cancel, idle timeout across clock wrap");
}
