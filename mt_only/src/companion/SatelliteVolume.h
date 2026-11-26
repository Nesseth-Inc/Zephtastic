// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

namespace mt::companion {
template<class Read,class Format> int initializeErasedHistory(size_t size,uint8_t erased,Read read,Format format){
    if(!size)return -EINVAL;
    uint8_t page[128];
    for(size_t at=0;at<size;at+=sizeof(page)){
        const size_t n=size-at<sizeof(page)?size-at:sizeof(page);
        int rc=read(at,page,n);if(rc)return rc;
        for(size_t i=0;i<n;i++)if(page[i]!=erased)return -ENOTEMPTY;
    }
    return format();
}

struct SatelliteFiles {
    static constexpr const char *file="/zt-history/.zt-sat-v1";
    static constexpr const char *temp="/zt-history/.zt-sat-v1.tmp";
    template<class Unlink> static int eraseOwned(Unlink unlink){
        const char *const owned[]={temp,file};
        for(const char *p:owned){int rc=unlink(p);if(rc&&rc!=-ENOENT)return rc;}
        return 0;
    }
};
template<class Stat,class Remove> int eraseSharedRangeLog(Stat statPublic,Remove removeCsv){
    const int found=statPublic();
    return found==-ENOENT?0:found?found:removeCsv();
}

// Reset recovery can boot before any normal filesystem owner has started.
// Mount without formatting. The only empty-volume shortcut is an erased partition.
template<class Volume,class Operation> int withExistingHistory(Volume &volume,Operation operation){
    const int rc=volume.mount(false,false);
    if(!rc)return operation();
    const int blank=volume.verifyErased();
    return blank==-ENOTEMPTY?-EAGAIN:blank;
}
template<class Volume,class EraseDirectories> int erasePrivateHistory(Volume &volume,EraseDirectories directories){
    return withExistingHistory(volume,[&](){
        const int rc=directories();return rc?rc:volume.eraseFiles();
    });
}
}
#if defined(__ZEPHYR__)
#include <zephyr/devicetree.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/flash_map.h>
extern "C" { FS_FSTAB_DECLARE_ENTRY(DT_NODELABEL(mt_sat_fs)); }
namespace mt::companion {
class SatelliteVolume:private SatelliteFiles {
    bool mounted_=false;
    fs_file_t handle_{};
    int open(const char *name,fs_mode_t mode){
        fs_file_t_init(&handle_);return fs_open(&handle_,name,mode);
    }
public:
    bool mounted()const{return mounted_;}
private:
    int mountOnce(bool allowInitialize){
        fs_mount_t &volume=FS_FSTAB_ENTRY(DT_NODELABEL(mt_sat_fs));
        volume.flags|=FS_MOUNT_FLAG_NO_FORMAT;
        (void)allowInitialize;int rc=fs_mount(&volume);
#if defined(CONFIG_MT_SATELLITE_INITIALIZE_ERASED)
        if(rc&&allowInitialize){
            const struct flash_area *part=nullptr;
            int opened=flash_area_open(uintptr_t(volume.storage_dev),&part);if(opened)return opened;
            rc=initializeErasedHistory(part->fa_size,flash_area_erased_val(part),
              [&](size_t at,void *out,size_t n){return flash_area_read(part,at,out,n);},
              [&](){
                  volume.flags&=~FS_MOUNT_FLAG_NO_FORMAT;
                  const int initialized=fs_mount(&volume);volume.flags|=FS_MOUNT_FLAG_NO_FORMAT;
                  return initialized;
              });
            flash_area_close(part);
        }
#endif
        return rc;
    }
public:
    int mount(bool allowInitialize,bool preparePublic=true){
        const int rc=mounted_?0:mountOnce(allowInitialize);
        mounted_=!rc;
        (void)preparePublic;
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
        if(mounted_&&preparePublic){
            // The public file owner checks this exact directory itself. A
            // failure or a non-directory collision leaves private history usable.
            fs_dirent dir{};if(fs_stat("/zt-history/public",&dir)==-ENOENT)
                (void)fs_mkdir("/zt-history/public");
        }
#endif
        return rc;
    }
    int openRead(){return open(file,FS_O_READ);}
    int read(void *p,size_t n){return fs_read(&handle_,p,n);}
    int exact(void *p,size_t n){int rc=read(p,n);return rc==int(n)?0:rc<0?rc:-EILSEQ;}
    int rewind(){return fs_seek(&handle_,0,FS_SEEK_SET);}
    int close(){return fs_close(&handle_);}
    int stage(){return open(temp,FS_O_CREATE|FS_O_WRITE|FS_O_TRUNC);}
    int write(const void *p,size_t n){int rc=fs_write(&handle_,p,n);return rc==int(n)?0:rc<0?rc:-EIO;}
    void abort(){(void)close();}
    int commit(){
        int rc=fs_sync(&handle_);int closed=close();if(!rc)rc=closed;
        return rc?rc:fs_rename(temp,file);
    }
    int verifyErased(){
        const fs_mount_t &volume=FS_FSTAB_ENTRY(DT_NODELABEL(mt_sat_fs));
        const struct flash_area *part=nullptr;
        int rc=flash_area_open(uintptr_t(volume.storage_dev),&part);if(rc)return rc;
        rc=initializeErasedHistory(part->fa_size,flash_area_erased_val(part),
            [&](size_t at,void *buf,size_t n){return flash_area_read(part,at,buf,n);},[](){return 0;});
        flash_area_close(part);return rc;
    }
    int eraseFiles(){return eraseOwned(fs_unlink);}
};
}
#endif
