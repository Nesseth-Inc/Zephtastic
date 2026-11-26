// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "NodeJournal.h"
#include <vector>
struct Files:mt::LargeNodeFiles {
    std::vector<uint8_t> slots[2];int writes=0,nonces=0,error=0;bool enabled=true,afterRename=false;
    bool mounted()const override{return enabled;}
    int nonce(uint8_t *out,size_t n)override{memset(out,++nonces,n);return 0;}
    int load(unsigned slot,const Token &t,uint8_t *p,size_t n)override{
        if(!enabled)return -ENODEV;
        auto &v=slots[slot];if(v.size()<sizeof(t))return -ENOENT;
        if(memcmp(&t,v.data(),sizeof(t)))return -EILSEQ;
        const size_t len=v.size()-sizeof(t);if(len>n)return -E2BIG;
        memcpy(p,v.data()+sizeof(t),len);return int(len);
    }
    int replace(unsigned slot,const Token &t,const uint8_t *p,size_t n,bool)override{
        if(error&&!afterRename)return error;
        auto &v=slots[slot];v.assign(t.bytes,t.bytes+sizeof(t));v.insert(v.end(),p,p+n);++writes;
        return error;
    }
    int discard(unsigned s)override{slots[s].clear();return 0;}
};
