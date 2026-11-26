// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::stock::telemetry::accessory {
// Each bus instance belongs to one explicitly declared peripheral. All commands
// and sampling run on the accessory worker, including sensor-specified waits.
struct Bus {
    virtual int write(const uint8_t *,size_t)=0;
    virtual int read(uint8_t *,size_t)=0;
    virtual int readReg(uint8_t,uint8_t *,size_t)=0;
    virtual int recover(){return -ENOTSUP;}
    virtual void wait(unsigned ms)=0;
    virtual uint64_t now()=0;
    virtual uint32_t utc()=0;
    virtual int load(const char *,void *,size_t)=0;
    virtual int save(const char *,const void *,size_t)=0;
    static uint8_t crc(const uint8_t *word) {
        uint8_t c=0xff;
        for(unsigned i=0;i<2;i++){c^=word[i];for(unsigned j=0;j<8;j++)c=c&0x80?(c<<1)^0x31:c<<1;}
        return c;
    }
    int command(uint16_t cmd,unsigned ms=0,const uint16_t *words=nullptr,size_t n=0){
        if(n>4)return -EINVAL;
        uint8_t tx[14]={uint8_t(cmd>>8),uint8_t(cmd)};
        for(size_t i=0;i<n;i++){
            auto *p=tx+2+i*3;p[0]=words[i]>>8;p[1]=words[i];p[2]=crc(p);
        }
        int e=write(tx,2+3*n);
        if(!e&&ms)wait(ms);
        return e;
    }
    int word(uint16_t cmd,uint16_t arg,unsigned ms){return command(cmd,ms,&arg,1);}
    int words(uint16_t cmd,uint16_t *out,size_t n,unsigned ms){
        if(n>16)return -EINVAL;
        uint8_t rx[48];int e=command(cmd,ms);
        if(!e)e=read(rx,3*n);
        if(e)return e;
        for(size_t i=0;i<n;i++)if(rx[i*3+2]!=crc(rx+i*3))return -EIO;
        for(size_t i=0;i<n;i++)out[i]=uint16_t(rx[i*3])<<8|rx[i*3+1];
        return 0;
    }
    int writeReg(uint8_t r,uint8_t value){const uint8_t bytes[]={r,value};return write(bytes,sizeof(bytes));}
    int update(uint8_t r,uint8_t mask,uint8_t value){
        uint8_t old=0;const int e=readReg(r,&old,1);
        return e?e:writeReg(r,(old&~mask)|(value&mask));
    }
};
inline bool validTemp(float t){return t>=-100&&t<=200;}
}
