// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
namespace mt::update::signedOta {
inline uint16_t le16(const uint8_t *p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
inline uint32_t le32(const uint8_t *p){return uint32_t(le16(p))|(uint32_t(le16(p+2))<<16);}
// Exactly the signed imgtool release format. SHA256 covers the header and body.
// The stored key hash binds the signature to this board family's release key.
template<class Read,class Verify> bool imageAllowed(size_t total,const uint8_t running[8],Read read,Verify verify) {
    uint8_t h[32];
    if(total<sizeof(h)||!read(0,h,sizeof(h))||le32(h)!=0x96f3b83du||le32(h+4)!=0||
       le16(h+8)!=32||le16(h+10)!=0||le32(h+16)!=0)return false;
    auto newer=[](const uint8_t *a,const uint8_t *b){
        if(a[0]!=b[0])return a[0]>b[0];
        if(a[1]!=b[1])return a[1]>b[1];
        if(le16(a+2)!=le16(b+2))return le16(a+2)>le16(b+2);
        return le32(a+4)>le32(b+4);
    };
    if(!newer(h+20,running)||le32(h+12)>total-32)return false;
    size_t body=32+le32(h+12),offset=body;
    uint8_t t[4],hash[32],key[32],sig[72];size_t sigLen=0;unsigned got=0;
    if(total-offset<4||!read(offset,t,4)||le16(t)!=0x6907||le16(t+2)!=total-offset)return false;
    offset+=4;
    while(offset<total){
        if(total-offset<4||!read(offset,t,4))return false;
        offset+=4;
        size_t n=le16(t+2);if(n>total-offset)return false;
        uint8_t *dst=nullptr;unsigned flag=0;
        switch(le16(t)){
        case 0x10: if(n==32){dst=hash;flag=1;}break;
        case 0x01: if(n==32){dst=key;flag=2;}break;
        case 0x22: if(n>=8&&n<=sizeof(sig)){dst=sig;flag=4;sigLen=n;}break;
        }
        if(!dst||(got&flag)||!read(offset,dst,n))return false;
        got|=flag;offset+=n;
    }
    return got==7&&verify(body,hash,key,sig,sigLen);
}
inline bool rawP256Signature(const uint8_t *der,size_t n,uint8_t raw[64]){
    if(n<8||n>72||der[0]!=0x30||der[1]!=n-2)return false;
    size_t pos=2;
    for(unsigned i=0;i<2;++i){
        if(n-pos<2||der[pos++]!=2)return false;
        size_t len=der[pos++];if(!len||len>33||len>n-pos||(der[pos]&0x80))return false;
        if(len>1&&der[pos]==0){if(!(der[pos+1]&0x80))return false;++pos;--len;}
        if(len>32)return false;
        memset(raw+i*32,0,32-len);memcpy(raw+i*32+32-len,der+pos,len);pos+=len;
    }
    return pos==n;
}
}
