// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic's 128-byte protobuf XModem protocol, firmware 8e6a88d.
#pragma once
#include <meshtastic/xmodem.pb.h>
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
#include "PublicTree.h"
#endif
#include <stdint.h>
#include <string.h>

namespace mt::stock::files {
using Packet=meshtastic_XModem;
using Control=meshtastic_XModem_Control;
inline constexpr Control SOH=meshtastic_XModem_Control_SOH;
inline constexpr Control STX=meshtastic_XModem_Control_STX;
inline constexpr Control EOT=meshtastic_XModem_Control_EOT;
inline constexpr Control ACK=meshtastic_XModem_Control_ACK;
inline constexpr Control NAK=meshtastic_XModem_Control_NAK;
inline constexpr Control CAN=meshtastic_XModem_Control_CAN;
inline constexpr char PublicName[]="/static/rangetest.csv";

inline uint16_t crc(const uint8_t *bytes,size_t n){
    uint16_t v=0;
    for(size_t i=0;i<n;++i){
        v^=uint16_t(bytes[i])<<8;
        for(unsigned bit=0;bit<8;++bit)v=uint16_t((v<<1)^((v&0x8000)?0x1021:0));
    }
    return v;
}
inline void control(Packet &out,Control c){out={};out.control=c;}

class XModem {
    enum class Mode { Idle, Read, Write } mode_=Mode::Idle;
    uint16_t seq_=0;
    unsigned retries_=0;
    size_t lastSize_=0;
    static bool name(const Packet &p){
        return (p.buffer.size==sizeof(PublicName)-1 || (p.buffer.size==sizeof(PublicName)&&p.buffer.bytes[sizeof(PublicName)-1]==0))
            && !memcmp(p.buffer.bytes,PublicName,sizeof(PublicName)-1);
    }
    template<class File> void data(File &f,Packet &out){
        control(out,SOH);out.seq=seq_;
        const int n=f.read(uint32_t(seq_-1)*sizeof(out.buffer.bytes),out.buffer.bytes,sizeof(out.buffer.bytes));
        if(n<0||size_t(n)>sizeof(out.buffer.bytes)){reset(f);control(out,CAN);return;}
        lastSize_=out.buffer.size=uint16_t(n);out.crc16=crc(out.buffer.bytes,out.buffer.size);
    }
public:
    bool active()const{return mode_!=Mode::Idle;}
    template<class File> void reset(File &f){if(active())f.close(false);mode_=Mode::Idle;seq_=0;}
    template<class File> void handle(File &f,const Packet &in,Packet &out){
        control(out,NAK);
        if(in.buffer.size>sizeof(in.buffer.bytes))return;
        if(!active()){
            if((in.control!=SOH&&in.control!=STX)||in.seq)return;
            const bool upload=in.control==SOH;
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
            size_t n=in.buffer.size;if(n&&in.buffer.bytes[n-1]==0)--n;
            const char *path=reinterpret_cast<const char*>(in.buffer.bytes);
            if(!PublicTree::valid(path,n)||f.open(upload,path,n))return;
#else
            if(!name(in)||f.open(upload))return;
#endif
            mode_=upload?Mode::Write:Mode::Read;seq_=1;retries_=25;
            if(upload)control(out,ACK);else data(f,out);
            return;
        }
        if(in.control==CAN){reset(f);control(out,ACK);return;}
        if(mode_==Mode::Write){
            if(in.control==EOT){
                const bool ok=f.close(true)==0;mode_=Mode::Idle;control(out,ok?ACK:CAN);return;
            }
            if((in.control!=SOH&&in.control!=STX)||in.seq!=seq_||!seq_||
               crc(in.buffer.bytes,in.buffer.size)!=in.crc16)return;
            if(f.write(in.buffer.bytes,in.buffer.size)){reset(f);control(out,CAN);return;}
            ++seq_;control(out,ACK);return;
        }
        if(in.control==NAK){
            if(--retries_){data(f,out);return;}
        }else if(in.control==ACK){
            if(lastSize_<sizeof(out.buffer.bytes)){reset(f);control(out,EOT);return;}
            if(seq_!=UINT16_MAX){++seq_;retries_=25;data(f,out);return;}
        }
        reset(f);control(out,CAN);
    }
};
}
