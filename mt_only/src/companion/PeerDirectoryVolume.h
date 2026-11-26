// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PeerDirectoryFiles.h"
#include <zephyr/fs/fs.h>
#include <errno.h>
#if defined(CONFIG_MT_WARM_PEERS)
#include <zephyr/random/random.h>
#endif
namespace mt::companion {
struct PeerFileIO {
    static int read(const char *name,void *dest,size_t length){
        fs_file_t file{};fs_file_t_init(&file);
        int rc=fs_open(&file,name,FS_O_READ);if(rc)return rc;
        ssize_t n=fs_read(&file,dest,length);uint8_t extra;
        if(n<0)rc=int(n);else if(size_t(n)!=length)rc=-EILSEQ;
        else {n=fs_read(&file,&extra,1);if(n)rc=n<0?int(n):-EILSEQ;}
        const int closed=fs_close(&file);return rc?rc:closed;
    }
    static int replace(const char *stage,const char *name,const void *src,size_t length){
        fs_file_t file{};fs_file_t_init(&file);
        int rc=fs_open(&file,stage,FS_O_CREATE|FS_O_TRUNC|FS_O_WRITE);if(rc)return rc;
        ssize_t n=fs_write(&file,src,length);if(n<0)rc=int(n);else if(size_t(n)!=length)rc=-EIO;
        if(!rc)rc=fs_sync(&file);
        const int closed=fs_close(&file);if(!rc)rc=closed;
        // On a failed commit the next verified open reclaims abandoned stages.
        return rc?rc:fs_rename(stage,name);
    }
};
template<class Names> struct PrivatePeerVolume : Names {
    using Names::path;
    using Names::stage;
#if defined(CONFIG_MT_WARM_PEERS)
    int nonce(void *out,size_t n){return sys_csrand_get(out,n);}
#endif
    int get(int page,unsigned bank,void *dest,size_t length){
        char name[40];path(name,page,bank);return PeerFileIO::read(name,dest,length);
    }
    int put(int page,unsigned bank,const void *src,size_t length){
        char name[40];path(name,page,bank);return PeerFileIO::replace(stage,name,src,length);
    }
    int dropStage(){const int rc=fs_unlink(stage);return rc==-ENOENT?0:rc;}
    int dropBank(int page,unsigned bank){
        char name[40];path(name,page,bank);const int rc=fs_unlink(name);return rc==-ENOENT?0:rc;
    }
    void retire(int page,unsigned bank){char name[40];path(name,page,bank);(void)fs_unlink(name);}
    int discardManifest(){
        char name[40];path(name,-1,0);const int rc=fs_unlink(name);return rc==-ENOENT?0:rc;
    }
    int erase(unsigned pages,unsigned banks=2){return erasePeerFiles<Names>(pages,banks,fs_unlink);}
};
using PeerDirectoryVolume=PrivatePeerVolume<PeerDirectoryFiles>;
}
