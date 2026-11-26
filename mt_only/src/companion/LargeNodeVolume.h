// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../NodeJournal.h"
#include <zephyr/fs/fs.h>
#include <zephyr/random/random.h>

namespace mt::companion {
template<class Volume> class LargeNodeVolume final:public LargeNodeFiles {
    Volume &volume_;
    static constexpr const char *stage="/zt-history/nodedb.stage";
    static const char *path(unsigned slot){return slot?"/zt-history/nodedb.1":"/zt-history/nodedb.0";}
    static int remove(const char *p){int rc=fs_unlink(p);return rc==-ENOENT?0:rc;}
    static int writePart(fs_file_t &f,const void *p,size_t size){
        const ssize_t n=fs_write(&f,p,size);
        return n<0?int(n):size_t(n)==size?0:-EIO;
    }
    static int readPart(fs_file_t &f,void *p,size_t size){
        const ssize_t n=fs_read(&f,p,size);
        return n<0?int(n):size_t(n)==size?0:-EILSEQ;
    }
    // Only reclaim our own unopened private stage or a recognized same-domain
    // file left by an interrupted migration. Refuse a conflicting file.
    static int owned(const char *p,const Token &current,bool isStage,bool enrolled=false){
        fs_file_t f{};fs_file_t_init(&f);
        int rc=fs_open(&f,p,FS_O_READ);if(rc==-ENOENT)return enrolled?-EILSEQ:0;if(rc)return rc;
        Token old{};const ssize_t n=fs_read(&f,&old,sizeof(old));
        const int closed=fs_close(&f);if(n<0)return int(n);if(closed)return closed;
        if(!n&&isStage)return 0;
        if(enrolled)return n==int(sizeof(old))&&!memcmp(&old,&current,sizeof(old))?0:-EILSEQ;
        uint8_t used=0;for(unsigned i=12;i<sizeof(old);++i)used|=old.bytes[i];
        return n==int(sizeof(old))&&used&&!memcmp(old.bytes,current.bytes,8)&&
            (isStage?old.bytes[8]<2:old.bytes[8]==current.bytes[8])&&
            !old.bytes[9]&&!old.bytes[10]&&!old.bytes[11]?0:-EILSEQ;
    }
public:
    explicit LargeNodeVolume(Volume &volume):volume_(volume){}
    bool mounted()const override{return volume_.mounted();}
    int nonce(uint8_t *out,size_t n)override{return sys_csrand_get(out,n);}
    int load(unsigned slot,const Token &token,uint8_t *out,size_t capacity)override{
        if(capacity<NodeJournal::LargeRecordSize)return -E2BIG;
        int rc=volume_.mount(false,false);if(rc)return rc;
        fs_file_t f{};fs_file_t_init(&f);
        rc=fs_open(&f,path(slot),FS_O_READ);if(rc)return rc;
        Token stored{};rc=readPart(f,&stored,sizeof(stored));
        if(!rc&&memcmp(&token,&stored,sizeof(stored)))rc=-EILSEQ;
        if(!rc)rc=readPart(f,out,NodeJournal::LargeRecordSize);
        uint8_t extra=0;if(!rc){const ssize_t n=fs_read(&f,&extra,1);if(n)rc=n<0?int(n):-EILSEQ;}
        const int closed=fs_close(&f);
        return rc?rc:closed?closed:int(NodeJournal::LargeRecordSize);
    }
    int replace(unsigned slot,const Token &token,const uint8_t *raw,size_t n,bool enrolled)override{
        if(!mounted())return -ENODEV;
        if(n!=NodeJournal::LargeRecordSize)return -EINVAL;
        fs_file_t f{};fs_file_t_init(&f);
        int rc=owned(path(slot),token,false,enrolled);if(rc)return rc;
        rc=owned(stage,token,true);if(rc)return rc;
        rc=fs_open(&f,stage,FS_O_CREATE|FS_O_TRUNC|FS_O_WRITE);if(rc)return rc;
        rc=writePart(f,&token,sizeof(token));if(!rc)rc=writePart(f,raw,n);
        if(!rc)rc=fs_sync(&f);const int closed=fs_close(&f);if(!rc)rc=closed;
        if(!rc)rc=fs_rename(stage,path(slot));
        if(rc)(void)remove(stage);
        return rc;
    }
    int discard(unsigned slot)override{return mounted()?remove(path(slot)):-ENODEV;}
    int erase(){
        const int a=remove(path(0));if(a)return a;
        const int b=remove(path(1));return b?b:remove(stage);
    }
};
}
