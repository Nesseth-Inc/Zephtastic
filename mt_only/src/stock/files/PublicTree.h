// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

namespace mt::stock::files {
// A dedicated public LittleFS mount. The private settings/history volumes are
// separate mounts and are never walked. XModem packets carry at most 128 bytes.
class PublicTree {
public:
    static constexpr unsigned PathSize=129,Limit=64;
    static constexpr char Prefix[]="/static/",Mount[]=CONFIG_MT_RANGE_TEST_LOG_DIR;
    static constexpr char Stage[]=CONFIG_MT_RANGE_TEST_LOG_DIR "/.zt-upload";
    static constexpr unsigned PhysicalSize=PathSize+sizeof(Mount);
    // Only the explicitly public subtree can be a virtual root. Never permit
    // the filesystem root or the history/settings directory itself.
    static_assert(__builtin_strcmp(Mount,"/public")==0 ||
                  __builtin_strcmp(Mount,"/zt-history/public")==0);
    struct Item {char name[PathSize]{};uint32_t size=0;};
    static bool valid(const char *name,size_t n){
        if(n<=sizeof(Prefix)-1||n>=PathSize||memcmp(name,Prefix,sizeof(Prefix)-1))return false;
        unsigned levels=0;bool start=true;
        for(size_t i=sizeof(Prefix)-1;i<n;++i){
            const unsigned c=static_cast<unsigned char>(name[i]);
            if(c=='/'){if(start||++levels>2)return false;start=true;continue;}
            if(c<32||c>=127||c=='\\'||(start&&c=='.'))return false;
            start=false;
        }
        return !start && !(n==sizeof("/static/rangetest.csv.tmp")-1&&!memcmp(name,"/static/rangetest.csv.tmp",n));
    }
    static int path(const char *name,size_t n,char (&out)[PhysicalSize]){
        if(!valid(name,n))return -EINVAL;
        memcpy(out,Mount,sizeof(Mount)-1);memcpy(out+sizeof(Mount)-1,name+sizeof(Prefix)-2,n-(sizeof(Prefix)-2));
        out[n+sizeof(Mount)-sizeof(Prefix)+1]=0;return 0;
    }
private:
    // Each pass selects the next lexical name. An insertion before a cursor
    // cannot duplicate a delivered row, and removing a file cannot skip its successor.
    static int scan(char (&wire)[PathSize],size_t used,unsigned depth,const char *after,Item &next,fs_dirent &entry){
        char physical[PhysicalSize];memcpy(physical,Mount,sizeof(Mount)-1);
        memcpy(physical+sizeof(Mount)-1,wire+sizeof(Prefix)-2,used-(sizeof(Prefix)-2)+1);
        fs_dir_t dir;fs_dir_t_init(&dir);int rc=fs_opendir(&dir,physical);
        if(rc)return rc;
        while(!(rc=fs_readdir(&dir,&entry))&&entry.name[0]){
            const size_t n=strnlen(entry.name,sizeof(entry.name));
            if(n>=sizeof(entry.name)||used+1+n>=sizeof(wire)||entry.name[0]=='.')continue;
            wire[used]='/';memcpy(wire+used+1,entry.name,n+1);
            if(valid(wire,used+1+n)){
                if(entry.type==FS_DIR_ENTRY_DIR&&depth<2)rc=scan(wire,used+1+n,depth+1,after,next,entry);
                else if(entry.type==FS_DIR_ENTRY_FILE&&entry.size<=UINT32_MAX&&strcmp(wire,after)>0&&
                        (!next.name[0]||strcmp(wire,next.name)<0)){strcpy(next.name,wire);next.size=uint32_t(entry.size);}
            }
            wire[used]=0;if(rc)break;
        }
        const int closed=fs_closedir(&dir);return rc?rc:closed;
    }
public:
    static int next(const char *after,Item &item){
        item={};char wire[PathSize]="/static";fs_dirent entry{};
        const int rc=scan(wire,strlen(wire),0,after,item,entry);
        return rc?rc:item.name[0]?1:0;
    }
};

// Each transport owns a separate cursor. The worker walks the mounted volume
// outside the service lock. A new session cannot receive an old walk result.
class TreeManifest {
    struct Cursor {PublicTree::Item item{};uint32_t generation=0;uint8_t count=0;bool pending=false,ready=false;};
    Cursor cursors_[2]{};
    k_spinlock lock_{};
public:
    template<class Info> int request(uint8_t owner,Info &out){
        if(owner>=2)return 0;
        const auto key=k_spin_lock(&lock_);auto &c=cursors_[owner];int result=0;
        if(c.count<PublicTree::Limit){
            if(!c.ready){c.pending=true;result=-EAGAIN;}
            else if(c.item.name[0]){strcpy(out.file_name,c.item.name);out.size_bytes=c.item.size;result=2;}
        }
        k_spin_unlock(&lock_,key);return result;
    }
    void advance(uint8_t owner){
        if(owner>=2)return;
        const auto key=k_spin_lock(&lock_);auto &c=cursors_[owner];
        if(c.ready&&c.item.name[0]){++c.count;c.ready=false;}
        k_spin_unlock(&lock_,key);
    }
    void reset(uint8_t owner){
        if(owner>=2)return;
        const auto key=k_spin_lock(&lock_);auto &c=cursors_[owner];
        const auto next=c.generation+1;c={};c.generation=next;k_spin_unlock(&lock_,key);
    }
    void poll(){
        for(auto &c:cursors_){
            auto key=k_spin_lock(&lock_);
            if(!c.pending){k_spin_unlock(&lock_,key);continue;}
            c.pending=false;const auto generation=c.generation;
            char after[PublicTree::PathSize];strcpy(after,c.item.name);k_spin_unlock(&lock_,key);
            PublicTree::Item item{};const int rc=PublicTree::next(after,item);
            key=k_spin_lock(&lock_);if(c.generation==generation){c.item=rc>0?item:PublicTree::Item{};c.ready=true;c.pending=false;}
            k_spin_unlock(&lock_,key);
        }
    }
};
}
