// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PeerDirectoryVolume.h"
namespace mt::companion {
struct SatellitePageNames {
    static constexpr const char *stage="/zt-history/.zt-rpt-stage";
    static void path(char (&out)[40],int p,unsigned){snprintf(out,sizeof(out),"/zt-history/.zt-rpt-%02x",unsigned(p));}
};
struct SatellitePageVolume:PrivatePeerVolume<SatellitePageNames>{
    int get(int p,void *out,size_t n){return PrivatePeerVolume::get(p,0,out,n);}
    int put(int p,const void *in,size_t n){return PrivatePeerVolume::put(p,0,in,n);}
    int eraseAll(){
        char file[40];for(unsigned p=0;p<30;p++){
            path(file,int(p),0);int rc=fs_unlink(file);if(rc&&rc!=-ENOENT)return rc;
        }
        int rc=fs_unlink(stage);return rc==-ENOENT?0:rc;
    }
    bool legacySnapshot()const{
        fs_dirent item{};return fs_stat("/zt-history/.zt-sat-v1",&item)!=-ENOENT||
                                fs_stat("/zt-history/.zt-sat-v1.tmp",&item)!=-ENOENT;
    }
};
}
