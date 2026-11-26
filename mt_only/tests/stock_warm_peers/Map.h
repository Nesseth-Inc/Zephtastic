// SPDX-License-Identifier: GPL-3.0-only
#pragma once
struct Map {
    std::map<int,std::vector<uint8_t>> files;
    unsigned reads=0,seq=0;bool fail=false,after=false;
    int nonce(void *p,size_t n){memset(p,++seq,n);return 0;}
    int get(int key,void *p,size_t n){
        ++reads;const auto i=files.find(key);if(i==files.end())return -ENOENT;
        if(i->second.size()!=n)return -EILSEQ;memcpy(p,i->second.data(),n);return 0;
    }
    int put(int key,const void *p,size_t n){
        if(fail&&!after)return -EIO;
        const auto bytes=static_cast<const uint8_t*>(p);files[key]={bytes,bytes+n};return fail?-EIO:0;
    }
    int get(int page,unsigned bank,void *p,size_t n){return get(page<0?-1:page*2+int(bank),p,n);}
    int put(int page,unsigned bank,const void *p,size_t n){return put(page<0?-1:page*2+int(bank),p,n);}
    void retire(int page,unsigned bank){files.erase(page*2+int(bank));}
};
